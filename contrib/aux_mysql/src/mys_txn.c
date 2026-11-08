/*-------------------------------------------------------------------------
 *
 * mys_txn.c
 *	  MySQL transaction rules on top of PostgreSQL transaction blocks.
 *
 *	- autocommit = 0: a statement gets a provisional PG block.  It becomes
 *	  a MySQL transaction when it opens an application table; otherwise the
 *	  unused block is closed without consuming SET TRANSACTION's defaults.
 *	- implicit commit before BEGIN / START TRANSACTION, before DDL, and when
 *	  autocommit goes from 0 to 1 (an aborted transaction is rolled back,
 *	  which is what COMMIT does to it in PostgreSQL).
 *	- DDL is committed by itself.
 *	- autocommit = 1: every statement of a multi-statement COM_QUERY is its
 *	  own transaction (PostgreSQL would run the batch as one implicit block).
 *	- statement-level rollback: inside a transaction, a failing statement
 *	  rolls back only its own effects and the transaction goes on (PG would
 *	  abort the whole transaction block).  Each statement runs under a
 *	  hidden savepoint; after an error the ReadyForQuery hook rolls back to
 *	  it.  Write statements then use a subtransaction XID each: a transaction
 *	  with more than 64 of them overflows the PGPROC subxid cache, so
 *	  mysql.statement_rollback can turn this off.
 *
 * The rules are applied to the raw statement list of a COM_QUERY: hidden
 * BEGIN / COMMIT statements are inserted where MySQL would start or end a
 * transaction.  exec_simple_query() finishes the transaction command after
 * each TransactionStmt, so this needs no kernel support.  The inserted
 * statements send no response and their warnings ("there is no transaction
 * in progress") are not counted.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"

#include "halo_mysql.h"
#include "mys_parsenodes.h"

/* sysvar_core.c: 0 = off, otherwise on */
extern int	autoCommit;

bool		halo_mysql_statement_rollback = true;

#define MYS_SAVEPOINT_NAME	"__aux_mysql_stmt"

/* nest level of the hidden savepoint of the running statement, 0 if none */
static int	stmt_savepoint_level = 0;

/* value of a SET autocommit = <expr>, or -1 if it is not a constant */
static int
autocommit_value(Node *expr)
{
	if (IsA(expr, A_Const) && !((A_Const *) expr)->isnull)
	{
		A_Const    *c = (A_Const *) expr;

		switch (nodeTag(&c->val))
		{
			case T_Integer:
				return intVal(&c->val) != 0;
			case T_Boolean:
				return boolVal(&c->val);
			case T_String:
				{
					const char *s = strVal(&c->val);

					if (pg_strcasecmp(s, "1") == 0 || pg_strcasecmp(s, "on") == 0 ||
						pg_strcasecmp(s, "true") == 0)
						return 1;
					if (pg_strcasecmp(s, "0") == 0 || pg_strcasecmp(s, "off") == 0 ||
						pg_strcasecmp(s, "false") == 0)
						return 0;
					return -1;
				}
			default:
				return -1;
		}
	}
	if (IsA(expr, ColumnRef))
	{
		/* SET autocommit = ON / OFF */
		ColumnRef  *cr = (ColumnRef *) expr;
		Node	   *f = llast(cr->fields);

		if (IsA(f, String))
		{
			if (pg_strcasecmp(strVal(f), "on") == 0 || pg_strcasecmp(strVal(f), "true") == 0)
				return 1;
			if (pg_strcasecmp(strVal(f), "off") == 0 || pg_strcasecmp(strVal(f), "false") == 0)
				return 0;
		}
	}
	return -1;
}

/* SET ... list: look for mysql.set_system_session_variable('autocommit', v) */
static int
find_set_autocommit(Node *set)
{
	SelectStmt *sel = (SelectStmt *) set;
	ListCell   *lc;
	int			result = -2;	/* not present */

	if (set == NULL || !IsA(set, SelectStmt))
		return result;
	foreach(lc, sel->targetList)
	{
		ResTarget  *rt = (ResTarget *) lfirst(lc);
		FuncCall   *fc;
		Node	   *name;

		if (!IsA(rt, ResTarget) || rt->val == NULL || !IsA(rt->val, FuncCall))
			continue;
		fc = (FuncCall *) rt->val;
		if (list_length(fc->args) != 2 ||
			strcmp(strVal(llast(fc->funcname)), "set_system_session_variable") != 0)
			continue;
		name = linitial(fc->args);
		if (IsA(name, A_Const) && IsA(&((A_Const *) name)->val, String) &&
			pg_strcasecmp(strVal(&((A_Const *) name)->val), "autocommit") == 0)
			result = autocommit_value(lsecond(fc->args));
	}
	return result;
}

/* Classify the raw statement of one MySQL statement (before rewriting) */
void
mys_txn_classify(Node *stmt, MysStmtInfo *info)
{
	info->set_autocommit = -1;
	if (info->diagnostic)
	{
		info->txn_kind = MYS_TXN_NEUTRAL;
		return;
	}

	switch ((int) nodeTag(stmt))
	{
		case T_TransactionStmt:
			switch (((TransactionStmt *) stmt)->kind)
			{
				case TRANS_STMT_BEGIN:
				case TRANS_STMT_START:
					info->txn_kind = MYS_TXN_BEGIN;
					break;
				case TRANS_STMT_COMMIT:
				case TRANS_STMT_ROLLBACK:
					info->txn_kind = ((TransactionStmt *) stmt)->chain ?
						MYS_TXN_NEUTRAL : MYS_TXN_END;
					info->is_commit = ((TransactionStmt *) stmt)->kind == TRANS_STMT_COMMIT;
					break;
				default:
					/* SAVEPOINT etc. need a transaction */
					info->txn_kind = MYS_TXN_SAVEPOINT;
					break;
			}
			break;

		case T_MysVariableSetStmt:
			{
				int			v = find_set_autocommit(((MysVariableSetStmt *) stmt)->varSetStmt);

				info->txn_kind = MYS_TXN_NEUTRAL;
				if (v != -2)
				{
					info->txn_kind = MYS_TXN_SET_AUTOCOMMIT;
					info->set_autocommit = v;
				}
				break;
			}

		case T_VariableSetStmt:
		case T_VariableShowStmt:
			/* EXECUTE: its statement is classified when it runs */
		case T_PrepareStmt:
		case T_ExecuteStmt:
		case T_DeallocateStmt:
			info->txn_kind = MYS_TXN_NEUTRAL;
			break;

		case T_CreateStmt:
			info->txn_kind = ((CreateStmt *) stmt)->relation->relpersistence == RELPERSISTENCE_TEMP ?
				MYS_TXN_REGULAR : MYS_TXN_IMPLICIT_COMMIT;
			break;
		case T_CreateTableAsStmt:
			info->txn_kind = ((CreateTableAsStmt *) stmt)->into->rel->relpersistence == RELPERSISTENCE_TEMP ?
				MYS_TXN_REGULAR : MYS_TXN_IMPLICIT_COMMIT;
			break;

			/* statements that MySQL commits implicitly */
		case T_AlterTableStmt:
		case T_DropStmt:
		case T_IndexStmt:
		case T_RenameStmt:
		case T_ViewStmt:
		case T_CreateSchemaStmt:
		case T_CreatedbStmt:
		case T_DropdbStmt:
		case T_TruncateStmt:
		case T_CreateFunctionStmt:
		case T_AlterFunctionStmt:
		case T_CreateTrigStmt:
		case T_CreateRoleStmt:
		case T_AlterRoleStmt:
		case T_DropRoleStmt:
		case T_GrantStmt:
		case T_GrantRoleStmt:
		case T_CreateSeqStmt:
		case T_AlterSeqStmt:
		case T_VacuumStmt:
			info->txn_kind = MYS_TXN_IMPLICIT_COMMIT;
			break;

		default:
			info->txn_kind = MYS_TXN_REGULAR;
			break;
	}
}

static RawStmt *
make_txn_stmt(TransactionStmtKind kind, int location)
{
	TransactionStmt *ts = makeNode(TransactionStmt);
	RawStmt    *rs = makeNode(RawStmt);

	ts->kind = kind;
	ts->location = -1;
	if (kind == TRANS_STMT_SAVEPOINT || kind == TRANS_STMT_RELEASE)
		ts->savepoint_name = MYS_SAVEPOINT_NAME;
	rs->stmt = (Node *) ts;
	rs->stmt_location = location;
	rs->stmt_len = 0;
	return rs;
}

static void
emit_txn(List **stmts, List **infos, TransactionStmtKind kind, int location)
{
	MysStmtInfo *info = palloc0(sizeof(MysStmtInfo));

	info->suppress_response = true;
	info->txn_control = true;
	info->txn_op = kind == TRANS_STMT_BEGIN ? MYS_TXNOP_BEGIN :
		kind == TRANS_STMT_COMMIT ? MYS_TXNOP_COMMIT :
		kind == TRANS_STMT_SAVEPOINT ? MYS_TXNOP_SAVEPOINT : MYS_TXNOP_RELEASE;
	info->set_autocommit = -1;
	*stmts = lappend(*stmts, make_txn_stmt(kind, location));
	*infos = lappend(*infos, info);
}

static void
emit_idle_commit(List **stmts, List **infos, int location)
{
	emit_txn(stmts, infos, TRANS_STMT_COMMIT, location);
	((MysStmtInfo *) llast(*infos))->txn_op = MYS_TXNOP_COMMIT_IDLE;
}

/*
 * Insert the hidden BEGIN / COMMIT statements.  "stmts" and "infos" are the
 * rewritten statements; group_start marks the first PG statement of each
 * MySQL statement.
 */
List *
mys_txn_apply(List *stmts, List **infos)
{
	List	   *out = NIL;
	List	   *out_infos = NIL;
	bool		in_block = IsTransactionBlock();
	bool		autocommit = (autoCommit != 0);
	int			ngroups = 0;
	int			group = 0;
	int			n = list_length(stmts);
	int			i = 0;

	/* A failed statement can leave a provisional block behind.  End it
	 * before planning another statement, so its snapshot/isolation cannot
	 * prevent a subsequent SET TRANSACTION from applying to the first table. */
	if (in_block && !mys_transaction_active())
	{
		emit_idle_commit(&out, &out_infos, 0);
		in_block = false;
	}

	foreach_ptr(MysStmtInfo, info, *infos)
	{
		if (info->group_start)
			ngroups++;
	}

	/* the common case: one statement, autocommit, no transaction rules */
	if (ngroups <= 1 && autocommit && !in_block && out == NIL && n > 0)
	{
		MysStmtInfo *first = linitial(*infos);

		if (first->txn_kind != MYS_TXN_SET_AUTOCOMMIT)
			return stmts;
	}

	while (i < n)
	{
		MysStmtInfo *first = list_nth(*infos, i);
		RawStmt    *rs = list_nth(stmts, i);
		int			kind = first->txn_kind;
		int			loc = rs->stmt_location;
		bool		wrap;

		/* implicit commit before */
		if (in_block &&
			(kind == MYS_TXN_BEGIN || kind == MYS_TXN_IMPLICIT_COMMIT ||
			 (kind == MYS_TXN_SET_AUTOCOMMIT && first->set_autocommit == 1 && !autocommit)))
		{
			emit_txn(&out, &out_infos, TRANS_STMT_COMMIT, loc);
			in_block = false;
		}

		/* autocommit = 0: implicit begin */
		if (!autocommit &&
			(kind == MYS_TXN_REGULAR || kind == MYS_TXN_SAVEPOINT))
		{
			emit_txn(&out, &out_infos, TRANS_STMT_BEGIN, loc);
			in_block = true;
		}

		/* statement-level rollback point */
		wrap = in_block && halo_mysql_statement_rollback &&
			(kind == MYS_TXN_REGULAR || kind == MYS_TXN_NEUTRAL ||
			 kind == MYS_TXN_SET_AUTOCOMMIT);
		if (wrap)
			emit_txn(&out, &out_infos, TRANS_STMT_SAVEPOINT, loc);

		/* the statements of this MySQL statement */
		do
		{
			out = lappend(out, list_nth(stmts, i));
			out_infos = lappend(out_infos, list_nth(*infos, i));
			i++;
		} while (i < n && !((MysStmtInfo *) list_nth(*infos, i))->group_start);
		group++;

		if (wrap)
			emit_txn(&out, &out_infos, TRANS_STMT_RELEASE, loc);

		switch (kind)
		{
			case MYS_TXN_BEGIN:
				in_block = true;
				break;
			case MYS_TXN_END:
				in_block = false;
				break;
			case MYS_TXN_SET_AUTOCOMMIT:
				if (first->set_autocommit >= 0)
					autocommit = (first->set_autocommit == 1);
				break;
			default:
				break;
		}

		/* Runtime table access decides whether the provisional BEGIN stays.
		 * The next BEGIN/SAVEPOINT is conditional too: a packet can contain
		 * any mixture of table reads, table-free SELECTs, SET and EXECUTE. */
		if (in_block && !autocommit)
			emit_idle_commit(&out, &out_infos, loc);

		/*
		 * Outside a transaction, a statement of a multi-statement query is
		 * committed on its own (and DDL always is).
		 */
		if (!in_block && group < ngroups && kind != MYS_TXN_END)
			emit_txn(&out, &out_infos, TRANS_STMT_COMMIT, loc);
	}

	*infos = out_infos;
	return out;
}

/* Called only for our top-level hidden TransactionStmt, before execution. */
bool
mys_txn_skip_control(MysStmtInfo *info)
{
	if (info->txn_op == MYS_TXNOP_COMMIT_IDLE)
		return mys_transaction_active();
	if (info->txn_op == MYS_TXNOP_BEGIN)
		return mys_transaction_has_block();
	if ((info->txn_op == MYS_TXNOP_SAVEPOINT || info->txn_op == MYS_TXNOP_RELEASE) &&
		!mys_transaction_has_block())
	{
		/* No savepoint was made; end_command must not record one. */
		info->txn_op = MYS_TXNOP_NONE;
		return true;
	}
	return false;
}

/* end_command of every statement: track the hidden savepoint */
void
mys_txn_stmt_done(MysStmtInfo *info)
{
	if (info->txn_op == MYS_TXNOP_SAVEPOINT)
		stmt_savepoint_level = GetCurrentTransactionNestLevel();
	else if (info->txn_op == MYS_TXNOP_RELEASE)
		stmt_savepoint_level = 0;
}

/*
 * ReadyForQuery after an error: if the failed statement ran under its
 * hidden savepoint, roll back to it so that the transaction goes on, as in
 * MySQL.  (PostgresMain has already aborted the subtransaction.)
 */
void
mys_txn_recover(void)
{
	MemoryContext oldcxt;

	if (stmt_savepoint_level == 0)
		return;
	if (IsAbortedTransactionBlockState() &&
		GetCurrentTransactionNestLevel() == stmt_savepoint_level)
	{
		oldcxt = CurrentMemoryContext;
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
	}
	stmt_savepoint_level = 0;
}

/*
 * A parse error inside a transaction: replace the statement list by one
 * marker statement that raises the error when it is executed, i.e. under
 * the hidden savepoint (see mys_parse_query and mys_ProcessUtility).
 */
#define DEFERRED_ERROR_MARKER "aux_mysql_deferred_error"

static ErrorData *deferred_parse_error = NULL;

List *
mys_txn_deferred_error(ErrorData *edata)
{
	DoStmt	   *d = makeNode(DoStmt);
	RawStmt    *rs = makeNode(RawStmt);

	d->args = list_make1(makeDefElem(DEFERRED_ERROR_MARKER, NULL, -1));
	rs->stmt = (Node *) d;
	rs->stmt_location = 0;
	rs->stmt_len = 0;
	deferred_parse_error = edata;
	return list_make1(rs);
}

bool
mys_txn_is_deferred_error(Node *stmt)
{
	DoStmt	   *d = (DoStmt *) stmt;

	return IsA(stmt, DoStmt) && list_length(d->args) == 1 &&
		strcmp(linitial_node(DefElem, d->args)->defname, DEFERRED_ERROR_MARKER) == 0 &&
		deferred_parse_error != NULL;
}

void
mys_txn_raise_deferred_error(void)
{
	ErrorData  *edata = deferred_parse_error;

	deferred_parse_error = NULL;
	ReThrowError(edata);
}
