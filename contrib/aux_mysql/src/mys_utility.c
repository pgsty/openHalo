/*-------------------------------------------------------------------------
 *
 * mys_utility.c
 *	  ProcessUtility hook: execute MySQL-only ALTER TABLE subcommands.
 *
 * openHalo implemented these in a forked tablecmds.c (mys_tablecmds.c,
 * ~17k lines).  aux_mysql translates them into ordinary PostgreSQL
 * utility statements at execution time, when the catalog can be consulted
 * (e.g. to find the primary key name for DROP PRIMARY KEY).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_trigger.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "commands/seclabel.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "tcop/utility.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/acl.h"
#include "miscadmin.h"

#include "halo_mysql.h"
#include "mys_parsenodes.h"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
/* utility commands aux_mysql runs itself to implement a MySQL statement */
static int	internal_utility_depth = 0;
static bool collation_provider_registered = false;

/* row triggers maintained by mysql_table_setup() */
#define ON_UPDATE_TRIGGER	"aux_mysql_on_update"
#define AUTOINC_TRIGGER		"aux_mysql_auto_increment"

/*
 * Store the table default in PostgreSQL's provider-owned object labels.
 * Unlike an OID-keyed extension table this follows rename/drop and pg_dump
 * without an auxiliary table, event triggers or catalog/ABI modifications.
 * This provider stores comparison metadata; it does not grant privileges.
 */
static void
check_collation_label(const ObjectAddress *object, const char *label)
{
	if (object->classId != RelationRelationId || object->objectSubId != 0 ||
		(get_rel_relkind(object->objectId) != RELKIND_RELATION &&
		 get_rel_relkind(object->objectId) != RELKIND_PARTITIONED_TABLE))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("aux_mysql collation labels apply only to tables")));
	if (label && strcmp(label, "case_insensitive") != 0 &&
		strcmp(label, "case_sensitive") != 0 &&
		strcmp(label, "ignore_accents") != 0 && strcmp(label, "default") != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid aux_mysql table collation label")));
}

const char *
mys_table_default_collation(Oid relid)
{
	ObjectAddress object;
	char *label;

	ObjectAddressSet(object, RelationRelationId, relid);
	label = GetSecurityLabel(&object, "aux_mysql");
	if (label)
		return label;
	return halo_mysql_default_collation && halo_mysql_default_collation[0] ?
		halo_mysql_default_collation : "default";
}

static void
run_utility(Node *stmt, PlannedStmt *orig, const char *queryString,
			ParamListInfo params, QueryEnvironment *queryEnv)
{
	PlannedStmt *wrapper = makeNode(PlannedStmt);

	wrapper->commandType = CMD_UTILITY;
	wrapper->canSetTag = false;
	wrapper->utilityStmt = stmt;
	wrapper->stmt_location = orig->stmt_location;
	wrapper->stmt_len = orig->stmt_len;

	/*
	 * Each generated command is a complete DDL command of its own: run it as
	 * a QUERY so that event triggers fire for it.  Event-trigger based
	 * extensions depend on that -- pg_tde decides whether a rewritten table
	 * file is encrypted in its ddl_command_start trigger, so as a
	 * SUBCOMMAND an ALTER of an encrypted table would write plaintext.
	 */
	internal_utility_depth++;
	PG_TRY();
	{
		ProcessUtility(wrapper, queryString, false, PROCESS_UTILITY_QUERY,
					   params, queryEnv, None_Receiver, NULL);
	}
	PG_FINALLY();
	{
		internal_utility_depth--;
	}
	PG_END_TRY();
	CommandCounterIncrement();
}

static AlterTableCmd *
make_cmd(AlterTableType subtype, char *name, Node *def)
{
	AlterTableCmd *cmd = makeNode(AlterTableCmd);

	cmd->subtype = subtype;
	cmd->name = name;
	cmd->def = def;
	cmd->behavior = DROP_RESTRICT;
	return cmd;
}

static void
flush_cmds(AlterTableStmt *orig, List **cmds, PlannedStmt *pstmt,
		   const char *queryString, ParamListInfo params,
		   QueryEnvironment *queryEnv)
{
	AlterTableStmt *stmt;

	if (*cmds == NIL)
		return;
	stmt = makeNode(AlterTableStmt);
	stmt->relation = copyObject(orig->relation);
	stmt->objtype = OBJECT_TABLE;
	stmt->missing_ok = orig->missing_ok;
	stmt->cmds = *cmds;
	run_utility((Node *) stmt, pstmt, queryString, params, queryEnv);
	*cmds = NIL;
}

static Node *
make_column_comment(Oid relid, char *colname, char *comment)
{
	CommentStmt *cs = makeNode(CommentStmt);

	cs->objtype = OBJECT_COLUMN;
	cs->object = (Node *) list_make3(makeString(get_namespace_name(get_rel_namespace(relid))),
									 makeString(get_rel_name(relid)),
									 makeString(colname));
	cs->comment = comment;
	return (Node *) cs;
}

static bool
column_in_primary_key(Oid relid, const char *colname)
{
	Relation	rel = relation_open(relid, NoLock);
	Oid			pkoid = RelationGetPrimaryKeyIndex(rel, true);
	AttrNumber	attnum = get_attnum(relid, colname);
	bool		found = false;

	relation_close(rel, NoLock);

	if (OidIsValid(pkoid) && attnum > 0)
	{
		Relation	idx = index_open(pkoid, AccessShareLock);

		for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
			if (idx->rd_index->indkey.values[i] == attnum)
				found = true;
		index_close(idx, AccessShareLock);
	}
	return found;
}

static bool
column_is_identity(Oid relid, AttrNumber attnum)
{
	HeapTuple	tp = SearchSysCache2(ATTNUM, ObjectIdGetDatum(relid), Int16GetDatum(attnum));
	bool		result = false;

	if (HeapTupleIsValid(tp))
	{
		result = ((Form_pg_attribute) GETSTRUCT(tp))->attidentity != '\0';
		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * MODIFY/CHANGE [COLUMN] col <full column definition>: MySQL replaces the
 * whole definition (type, nullability, default, AUTO_INCREMENT).
 *
 * AUTO_INCREMENT columns are identity columns (mys_rewrite.c): keep the
 * identity when the new definition still says AUTO_INCREMENT (openHalo
 * 154ce7a fixed the same case in its kernel), drop it when it does not, and
 * add one -- counter after the current maximum -- when the column gains it.
 */
static void
modify_column(AlterTableStmt *orig, Oid relid, char *colname, ColumnDef *def,
			  List **cmds, List **after, bool *added_identity,
			  PlannedStmt *pstmt, const char *queryString,
			  ParamListInfo params, QueryEnvironment *queryEnv)
{
	ColumnDef  *tdef = makeNode(ColumnDef);
	TypeCast   *using = makeNode(TypeCast);
	ColumnRef  *cref = makeNode(ColumnRef);
	bool		notnull = def->is_not_null;
	Node	   *defexpr = NULL;
	Constraint *identity = NULL;
	AttrNumber	attnum = get_attnum(relid, colname);
	bool		was_identity = attnum > 0 && column_is_identity(relid, attnum);
	List	   *checks = NIL;
	ListCell   *lc;

	foreach(lc, def->constraints)
	{
		Node	   *n = lfirst(lc);

		if (IsA(n, Constraint))
		{
			Constraint *c = (Constraint *) n;

			if (c->contype == CONSTR_NOTNULL || c->contype == CONSTR_PRIMARY)
				notnull = true;
			else if (c->contype == CONSTR_DEFAULT)
				defexpr = c->raw_expr;
			else if (c->contype == CONSTR_IDENTITY)
				identity = c;
			else if (c->contype == CONSTR_CHECK)
				checks = lappend(checks, c);
		}
		else if (IsA(n, CommentStmt))
			*after = lappend(*after, make_column_comment(relid, colname,
														  ((CommentStmt *) n)->comment));
	}
	if (identity)
	{
		notnull = true;			/* AUTO_INCREMENT columns are NOT NULL */
		defexpr = NULL;			/* and have no DEFAULT */
	}

	/*
	 * Drop the old default first: it may not be castable to the new type.
	 * An identity column has no default; drop the identity instead when the
	 * new definition has no AUTO_INCREMENT.
	 */
	if (!was_identity)
		*cmds = lappend(*cmds, make_cmd(AT_ColumnDefault, colname, NULL));
	else if (!identity)
		*cmds = lappend(*cmds, make_cmd(AT_DropIdentity, colname, NULL));
	/* Remove only our domain guard for this attribute, including after CHANGE. */
	{
		Relation rel = relation_open(relid, NoLock);
		TupleConstr *constr = RelationGetDescr(rel)->constr;

		if (constr)
			for (int i = 0; i < constr->num_check; i++)
			{
				char *name = constr->check[i].ccname;
				HeapTuple tuple;
				ArrayType *keys;

				if (strncmp(name, "aux_mysql_domain_", 17) != 0)
					continue;
				tuple = SearchSysCache1(CONSTROID,
						ObjectIdGetDatum(get_relation_constraint_oid(relid, name, false)));
				keys = DatumGetArrayTypeP(SysCacheGetAttrNotNull(CONSTROID, tuple,
												Anum_pg_constraint_conkey));
				if (ARR_NDIM(keys) == 1 && ARR_DIMS(keys)[0] == 1 &&
					*((int16 *) ARR_DATA_PTR(keys)) == attnum)
					*cmds = lappend(*cmds, make_cmd(AT_DropConstraint, pstrdup(name), NULL));
				ReleaseSysCache(tuple);
			}
		relation_close(rel, NoLock);
	}
	flush_cmds(orig, cmds, pstmt, queryString, params, queryEnv);

	/*
	 * The AUTO_INCREMENT trigger fires on UPDATE OF the identity column,
	 * which blocks ALTER TYPE; the table setup that follows MODIFY / CHANGE
	 * recreates it.
	 */
	if (was_identity)
	{
		DropStmt   *drop = makeNode(DropStmt);

		drop->removeType = OBJECT_TRIGGER;
		drop->objects = list_make1(list_make3(makeString(get_namespace_name(get_rel_namespace(relid))),
											  makeString(get_rel_name(relid)),
											  makeString(AUTOINC_TRIGGER)));
		drop->behavior = DROP_RESTRICT;
		drop->missing_ok = true;
		run_utility((Node *) drop, pstmt, queryString, params, queryEnv);
	}

	/* ALTER COLUMN col TYPE t USING col::t  (MySQL converts implicitly) */
	cref->fields = list_make1(makeString(colname));
	cref->location = -1;
	using->arg = (Node *) cref;
	using->typeName = def->typeName;
	using->location = -1;
	tdef->colname = colname;
	tdef->typeName = def->typeName;
	tdef->collClause = def->collClause;
	tdef->raw_default = (Node *) using;
	*cmds = lappend(*cmds, make_cmd(AT_AlterColumnType, colname, (Node *) tdef));

	if (notnull)
		*cmds = lappend(*cmds, make_cmd(AT_SetNotNull, colname, NULL));
	else if (!column_in_primary_key(relid, colname))
		*cmds = lappend(*cmds, make_cmd(AT_DropNotNull, colname, NULL));

	if (defexpr)
		*cmds = lappend(*cmds, make_cmd(AT_ColumnDefault, colname, defexpr));

	if (identity && !was_identity)
	{
		Constraint *c = makeNode(Constraint);

		c->contype = CONSTR_IDENTITY;
		c->generated_when = ATTRIBUTE_IDENTITY_BY_DEFAULT;
		c->options = NIL;
		c->location = -1;
		*cmds = lappend(*cmds, make_cmd(AT_AddIdentity, colname, (Node *) c));
		*added_identity = true;
	}
	foreach_ptr(Constraint, check, checks)
		*cmds = lappend(*cmds, make_cmd(AT_AddConstraint, NULL, (Node *) check));
}

static void
mysql_alter_table(AlterTableStmt *stmt, PlannedStmt *pstmt, const char *queryString,
				  ParamListInfo params, QueryEnvironment *queryEnv)
{
	Oid			relid;
	Relation	rel;
	List	   *cmds = NIL;
	List	   *after = NIL;
	bool		added_identity = false;
	bool		has_start = false;
	int64		start = 1;
	ListCell   *lc;

	relid = RangeVarGetRelid(stmt->relation, AccessExclusiveLock, stmt->missing_ok);
	if (!OidIsValid(relid))
		return;

	/*
	 * Only peek at the relation between sub-statements: an open relcache
	 * reference would make ALTER TABLE fail with "being used by active
	 * queries in this session".
	 */
#define REL_OPEN()	(rel = relation_open(relid, NoLock))
#define REL_CLOSE()	relation_close(rel, NoLock)

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		switch ((int) cmd->subtype)
		{
			case AT_ModifyColumn:
				modify_column(stmt, relid, cmd->name, (ColumnDef *) cmd->def, &cmds, &after,
							  &added_identity, pstmt, queryString, params, queryEnv);
				break;

			case AT_ChangeColumn:
				{
					ColumnDef  *def = (ColumnDef *) cmd->def;

					if (strcmp(cmd->name, def->colname) != 0)
					{
						RenameStmt *rn = makeNode(RenameStmt);

						flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);
						rn->renameType = OBJECT_COLUMN;
						rn->relationType = OBJECT_TABLE;
						rn->relation = copyObject(stmt->relation);
						rn->subname = cmd->name;
						rn->newname = def->colname;
						run_utility((Node *) rn, pstmt, queryString, params, queryEnv);
					}
					modify_column(stmt, relid, def->colname, def, &cmds, &after,
								  &added_identity, pstmt, queryString, params, queryEnv);
					break;
				}

			case AT_DropPrimaryKey:
				{
					Oid			pkoid;
					Oid			conoid;

					REL_OPEN();
					pkoid = RelationGetPrimaryKeyIndex(rel, true);
					REL_CLOSE();
					conoid = OidIsValid(pkoid) ? get_index_constraint(pkoid) : InvalidOid;

					if (!OidIsValid(conoid))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("Can't DROP 'PRIMARY'; check that column/key exists")));
					cmds = lappend(cmds, make_cmd(AT_DropConstraint, get_constraint_name(conoid), NULL));
					break;
				}

			case AT_DropIndex:
				if (OidIsValid(get_relation_constraint_oid(relid, cmd->name, true)))
					cmds = lappend(cmds, make_cmd(AT_DropConstraint, cmd->name, NULL));
				else
				{
					DropStmt   *drop = makeNode(DropStmt);

					flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);
					drop->removeType = OBJECT_INDEX;
					drop->objects = list_make1(list_make2(makeString(get_namespace_name(get_rel_namespace(relid))),
														  makeString(cmd->name)));
					drop->behavior = DROP_RESTRICT;
					run_utility((Node *) drop, pstmt, queryString, params, queryEnv);
				}
				break;

			case AT_DropForeignKey:
			case AT_DropCheck:
				cmds = lappend(cmds, make_cmd(AT_DropConstraint, cmd->name, NULL));
				break;

			case AT_TableOption:
				{
					ListCell   *lc2;

					foreach(lc2, (List *) cmd->def)
					{
						DefElem    *d = lfirst_node(DefElem, lc2);

						if (strcmp(d->defname, "auto_increment") == 0)
						{
							start = defGetInt64(d);
							has_start = true;
						}
						else if (strcmp(d->defname, "engine") == 0 ||
								 strcmp(d->defname, "encryption") == 0)
						{
							/* ENGINE=x / ENCRYPTION='Y': SET ACCESS METHOD */
							char	   *am = mys_engine_access_method(d);

							if (am)
								cmds = lappend(cmds, make_cmd(AT_SetAccessMethod, am, NULL));
						}
						else if (strcmp(d->defname, "comment") == 0)
						{
							CommentStmt *cs = makeNode(CommentStmt);

							cs->objtype = OBJECT_TABLE;
							cs->object = (Node *) list_make2(makeString(get_namespace_name(get_rel_namespace(relid))),
															 makeString(get_rel_name(relid)));
							cs->comment = strVal(d->arg);
							after = lappend(after, cs);
						}
						/* ENGINE, CHARSET ... : accepted, ignored */
					}
					break;
				}

			default:
				cmds = lappend(cmds, cmd);
				break;
		}
	}
	flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);

	/* a column that just became AUTO_INCREMENT continues after its maximum */
	if (added_identity)
		mys_sync_auto_increment(relid);
	if (has_start)
		mys_set_auto_increment(relid, start);

	foreach(lc, after)
		run_utility(lfirst(lc), pstmt, queryString, params, queryEnv);
}

/* ----------------------------------------------------------------
 * Per-table setup after MySQL DDL: triggers for ON UPDATE CURRENT_TIMESTAMP
 * and the AUTO_INCREMENT counter (see mys_rewrite.c make_table_setup()).
 * ----------------------------------------------------------------
 */

static bool
is_table_setup(Node *stmt)
{
	DoStmt	   *d = (DoStmt *) stmt;

	return IsA(stmt, DoStmt) && d->args != NIL &&
		strcmp(linitial_node(DefElem, d->args)->defname, MYS_TABLE_SETUP_MARKER) == 0;
}

static void
create_row_trigger(const char *nsp, const char *rel, const char *name,
				   const char *func, int16 events, List *args, List *columns,
				   PlannedStmt *pstmt, const char *queryString,
				   ParamListInfo params, QueryEnvironment *queryEnv)
{
	CreateTrigStmt *t = makeNode(CreateTrigStmt);

	t->replace = true;
	t->isconstraint = false;
	t->trigname = pstrdup(name);
	t->relation = makeRangeVar(pstrdup(nsp), pstrdup(rel), -1);
	t->funcname = list_make2(makeString("mysql"), makeString(pstrdup(func)));
	t->args = args;
	t->row = true;
	t->timing = TRIGGER_TYPE_BEFORE;
	t->events = events;
	t->columns = columns;
	run_utility((Node *) t, pstmt, queryString, params, queryEnv);

	/* also with FOREIGN_KEY_CHECKS=0, i.e. session_replication_role = replica */
	{
		AlterTableStmt *alter = makeNode(AlterTableStmt);
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_EnableAlwaysTrig;
		cmd->name = pstrdup(name);
		alter->relation = makeRangeVar(pstrdup(nsp), pstrdup(rel), -1);
		alter->cmds = list_make1(cmd);
		alter->objtype = OBJECT_TABLE;
		run_utility((Node *) alter, pstmt, queryString, params, queryEnv);
	}
}

static void
mysql_table_setup(DoStmt *d, PlannedStmt *pstmt, const char *queryString,
				  ParamListInfo params, QueryEnvironment *queryEnv)
{
	RangeVar   *rv = (RangeVar *) linitial_node(DefElem, d->args)->arg;
	Oid			relid = RangeVarGetRelid(rv, NoLock, true);
	Relation	rel;
	bool	   *on_update;
	bool		had_on_update = false;
	bool		had_autoinc = false;
	char	   *identity = NULL;
	char	   *nsp;
	char	   *relname;
	List	   *args = NIL;
	int			natts;

	if (!OidIsValid(relid))
		return;

	rel = relation_open(relid, AccessShareLock);
	natts = RelationGetDescr(rel)->natts;
	on_update = palloc0(sizeof(bool) * (natts + 1));
	if (rel->trigdesc)
	{
		for (int i = 0; i < rel->trigdesc->numtriggers; i++)
		{
			Trigger    *tg = &rel->trigdesc->triggers[i];

			if (strcmp(tg->tgname, ON_UPDATE_TRIGGER) == 0)
			{
				had_on_update = true;
				for (int k = 0; k < tg->tgnargs; k++)
				{
					int			attnum = atoi(tg->tgargs[k]);

					if (attnum > 0 && attnum <= natts)
						on_update[attnum] = true;
				}
			}
			else if (strcmp(tg->tgname, AUTOINC_TRIGGER) == 0)
				had_autoinc = true;
		}
	}
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);

		if (!att->attisdropped && att->attidentity)
			identity = pstrdup(NameStr(att->attname));
	}
	nsp = get_namespace_name(RelationGetNamespace(rel));
	relname = pstrdup(RelationGetRelationName(rel));
	relation_close(rel, AccessShareLock);

	for (ListCell *lc = lnext(d->args, list_head(d->args)); lc; lc = lnext(d->args, lc))
	{
		DefElem    *e = lfirst_node(DefElem, lc);
		AttrNumber	attnum;

		if (strcmp(e->defname, "default_collation") == 0)
		{
			ObjectAddress object;

			ObjectAddressSet(object, RelationRelationId, relid);
			if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));
			check_collation_label(&object, strVal(e->arg));
			SetSecurityLabel(&object, "aux_mysql", strVal(e->arg));
			CommandCounterIncrement();
			continue;
		}
		attnum = get_attnum(relid, strVal(e->arg));

		if (attnum <= 0 || attnum > natts)
			continue;
		on_update[attnum] = strcmp(e->defname, "on_update_add") == 0;
	}

	for (int attnum = 1; attnum <= natts; attnum++)
	{
		if (on_update[attnum])
			args = lappend(args, makeString(psprintf("%d", attnum)));
	}
	if (args != NIL)
		create_row_trigger(nsp, relname, ON_UPDATE_TRIGGER, "on_update_current_timestamp",
						   TRIGGER_TYPE_UPDATE, args, NIL,
						   pstmt, queryString, params, queryEnv);
	else if (had_on_update)
	{
		DropStmt   *drop = makeNode(DropStmt);

		drop->removeType = OBJECT_TRIGGER;
		drop->objects = list_make1(list_make3(makeString(nsp), makeString(relname),
											  makeString(ON_UPDATE_TRIGGER)));
		drop->behavior = DROP_RESTRICT;
		run_utility((Node *) drop, pstmt, queryString, params, queryEnv);
	}

	/* UPDATE OF the identity column only: other updates do not need it */
	if (identity && !had_autoinc)
		create_row_trigger(nsp, relname, AUTOINC_TRIGGER, "auto_increment_advance",
						   TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE, NIL,
						   list_make1(makeString(identity)),
						   pstmt, queryString, params, queryEnv);
	else if (!identity && had_autoinc)
	{
		/* MODIFY dropped AUTO_INCREMENT */
		DropStmt   *drop = makeNode(DropStmt);

		drop->removeType = OBJECT_TRIGGER;
		drop->objects = list_make1(list_make3(makeString(nsp), makeString(relname),
											  makeString(AUTOINC_TRIGGER)));
		drop->behavior = DROP_RESTRICT;
		run_utility((Node *) drop, pstmt, queryString, params, queryEnv);
	}
}

static bool
has_mysql_subcommands(AlterTableStmt *stmt)
{
	ListCell   *lc;

	foreach(lc, stmt->cmds)
	{
		if ((int) lfirst_node(AlterTableCmd, lc)->subtype > AT_MysBase)
			return true;
	}
	return false;
}

/* START TRANSACTION / BEGIN */
static bool
is_transaction_start(Node *stmt)
{
	return IsA(stmt, TransactionStmt) &&
		(((TransactionStmt *) stmt)->kind == TRANS_STMT_START ||
		 ((TransactionStmt *) stmt)->kind == TRANS_STMT_BEGIN);
}

static Node *
bool_const(bool value)
{
	A_Const    *n = makeNode(A_Const);

	n->val.ival.type = T_Integer;
	n->val.ival.ival = value;
	n->location = -1;
	return (Node *) n;
}

/*
 * FOREIGN_KEY_CHECKS=0: PostgreSQL refuses TRUNCATE of a table that another
 * table's foreign key references, also under session_replication_role =
 * replica.  MySQL empties just that table: delete its rows instead (no RI
 * action fires in replica mode).  TRUNCATE does not reset AUTO_INCREMENT
 * here either.
 */
static bool
truncate_referenced_tables(TruncateStmt *stmt)
{
	List	   *relids = NIL;

	if (mys_foreign_key_checks() || stmt->restart_seqs || stmt->behavior != DROP_RESTRICT)
		return false;
	foreach_node(RangeVar, rv, stmt->relations)
	{
		Oid			relid = RangeVarGetRelid(rv, NoLock, true);

		if (!OidIsValid(relid))
			return false;		/* TRUNCATE reports it */
		relids = lappend_oid(relids, relid);
	}
	if (heap_truncate_find_FKs(relids) == NIL)
		return false;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	foreach_node(RangeVar, rv, stmt->relations)
	{
		Relation	rel = table_openrv(rv, RowExclusiveLock);
		Oid			relid = RelationGetRelid(rel);
		AclResult	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_TRUNCATE);
		Oid			save_userid;
		int			save_sec_context;
		char	   *sql;

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));
		sql = psprintf("DELETE FROM %s%s", rv->inh ? "" : "ONLY ",
					   quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
												  RelationGetRelationName(rel)));

		/*
		 * TRUNCATE semantics: the TRUNCATE privilege checked above suffices
		 * and no row is filtered out, so delete as the owner with row level
		 * security bypassed, as the RI triggers do.  An error restores the
		 * user at abort.
		 */
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(rel->rd_rel->relowner,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
							   SECURITY_NOFORCE_RLS);
		if (SPI_execute(sql, false, 0) != SPI_OK_DELETE)
			elog(ERROR, "could not empty table \"%s\"", RelationGetRelationName(rel));
		SetUserIdAndSecContext(save_userid, save_sec_context);
		table_close(rel, NoLock);
	}
	SPI_finish();
	return true;
}

/* a relation in the session's temporary schema */
static bool
is_temp_rangevar(RangeVar *rv)
{
	Oid			relid;

	if (rv == NULL)
		return false;
	if (rv->relpersistence == RELPERSISTENCE_TEMP)
		return true;
	relid = RangeVarGetRelid(rv, NoLock, true);
	return OidIsValid(relid) && isTempNamespace(get_rel_namespace(relid));
}

/* MySQL's read_only still allows temporary tables */
static bool
temp_table_utility(Node *stmt)
{
	switch (nodeTag(stmt))
	{
		case T_CreateStmt:
			return ((CreateStmt *) stmt)->relation->relpersistence == RELPERSISTENCE_TEMP;
		case T_CreateTableAsStmt:
			return ((CreateTableAsStmt *) stmt)->into->rel->relpersistence == RELPERSISTENCE_TEMP;
		case T_AlterTableStmt:
			return is_temp_rangevar(((AlterTableStmt *) stmt)->relation);
		case T_IndexStmt:
			return is_temp_rangevar(((IndexStmt *) stmt)->relation);
		case T_RenameStmt:
			return is_temp_rangevar(((RenameStmt *) stmt)->relation);
		case T_CommentStmt:
			{
				CommentStmt *comment = (CommentStmt *) stmt;
				List	   *name = (List *) comment->object;

				/* TABLE t / COLUMN t.c, as CREATE TEMPORARY TABLE ... COMMENT makes */
				if (comment->objtype == OBJECT_COLUMN)
					name = list_truncate(list_copy(name), list_length(name) - 1);
				else if (comment->objtype != OBJECT_TABLE)
					return false;
				return is_temp_rangevar(makeRangeVarFromNameList(name));
			}
		case T_TruncateStmt:
			foreach_node(RangeVar, rv, ((TruncateStmt *) stmt)->relations)
				if (!is_temp_rangevar(rv))
					return false;
			return true;
		case T_DropStmt:
			if (((DropStmt *) stmt)->removeType != OBJECT_TABLE)
				return false;
			foreach_node(List, name, ((DropStmt *) stmt)->objects)
			{
				RangeVar   *rv = makeRangeVarFromNameList(name);
				Oid			relid = RangeVarGetRelid(rv, NoLock, true);

				if (OidIsValid(relid) && !isTempNamespace(get_rel_namespace(relid)))
					return false;
			}
			return true;
		case T_DoStmt:
			return is_table_setup(stmt) &&
				is_temp_rangevar((RangeVar *) linitial_node(DefElem, ((DoStmt *) stmt)->args)->arg);
		default:
			return false;
	}
}

/*
 * GLOBAL read_only / super_read_only for a utility statement: whether to run
 * it read-only, so that PostgreSQL refuses what modifies the database,
 * reported as 1290.  Checked per statement, so a COMMIT earlier in the packet
 * cannot open a writable transaction, and for that statement alone, so a USE
 * does not stop a later CREATE TEMPORARY TABLE.  Commands aux_mysql or
 * PostgreSQL issue for a statement already checked (an AUTO_INCREMENT
 * column's sequence), and temporary tables, are exempt.
 */
static bool
server_read_only_utility(Node *stmt, ProcessUtilityContext context)
{
	return internal_utility_depth == 0 && context != PROCESS_UTILITY_SUBCOMMAND &&
		!IsA(stmt, TransactionStmt) && mys_server_read_only_applies() && !XactReadOnly &&
		!temp_table_utility(stmt);
}

/* GLOBAL read_only for DML, as ExecCheckXactReadOnly(): temp tables stay writable */
static void
check_server_read_only_dml(PlannedStmt *stmt)
{
	if (stmt->commandType == CMD_SELECT && !stmt->hasModifyingCTE && stmt->rowMarks == NIL)
		return;					/* SELECT ... FOR UPDATE is refused, as in MySQL */
	if (!mys_server_read_only_applies())
		return;
	foreach_node(RTEPermissionInfo, perminfo, stmt->permInfos)
	{
		if ((perminfo->requiredPerms & ~ACL_SELECT) == 0 ||
			isTempNamespace(get_rel_namespace(perminfo->relid)))
			continue;
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("The MySQL server is running with the --read-only option so it cannot execute this statement")));
	}
}

/*
 * A table of the application's, not one of aux_mysql's that SHOW and
 * information_schema read: MySQL's equivalents start no InnoDB transaction.
 */
static bool
uses_application_table(List *rtable)
{
	foreach_node(RangeTblEntry, rte, rtable)
	{
		char	   *nsp;

		if (rte->rtekind != RTE_RELATION || rte->relkind == RELKIND_VIEW ||
			rte->relkind == RELKIND_SEQUENCE ||
			rte->relid < FirstNormalObjectId)
			continue;
		nsp = get_namespace_name(get_rel_namespace(rte->relid));
		if (nsp == NULL || strcmp(nsp, "mysql") == 0 || strcmp(nsp, "mys_informa_schema") == 0 ||
			strcmp(nsp, "performance_schema") == 0 || strcmp(nsp, "mys_sys") == 0)
			continue;
		return true;
	}
	return false;
}

/*
 * Before every statement, also within a multi-statement packet:
 * session_replication_role follows @@foreign_key_checks (a ROLLBACK earlier
 * in the packet may have restored the GUC), and GLOBAL read_only applies.
 * Once the executor has opened its tables (a write in a read-only transaction
 * fails before), note that the statement used its transaction: that spends
 * SET TRANSACTION's one-shot values (sysvar_core.c).
 */
static void
mys_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	bool		mysql = mys_is_mysql_session();

	if (mysql)
	{
		mys_sync_foreign_key_checks();
		check_server_read_only_dml(queryDesc->plannedstmt);
	}
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
	if (mysql && uses_application_table(queryDesc->plannedstmt->rtable))
		mys_statement_touched_tables();
}

static void
process_utility(PlannedStmt *pstmt, const char *queryString,
				bool readOnlyTree, ProcessUtilityContext context,
				ParamListInfo params, QueryEnvironment *queryEnv,
				DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	/* Do not change bare SECURITY LABEL semantics in ordinary PG sessions. */
	if (IsA(parsetree, SecLabelStmt) &&
		((SecLabelStmt *) parsetree)->provider &&
		strcmp(((SecLabelStmt *) parsetree)->provider, "aux_mysql") == 0 &&
		!collation_provider_registered)
	{
		register_label_provider("aux_mysql", check_collation_label);
		collation_provider_registered = true;
	}

	if (mys_is_mysql_session())
		mys_sync_foreign_key_checks();

	/* Resolve ADD/MODIFY defaults at execution, including multi-statement DDL. */
	if (mys_is_mysql_session() && halo_mysql_parser == MYS_PARSER_MYSQL &&
		IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *alter = copyObject((AlterTableStmt *) parsetree);
		Oid relid = RangeVarGetRelid(alter->relation, AccessExclusiveLock, alter->missing_ok);

		if (OidIsValid(relid))
			foreach_node(AlterTableCmd, cmd, alter->cmds)
			{
				if (cmd->def && IsA(cmd->def, ColumnDef) &&
					(cmd->subtype == AT_AddColumn || (int) cmd->subtype == AT_ModifyColumn ||
					 (int) cmd->subtype == AT_ChangeColumn))
					mys_rectify_column_collation((ColumnDef *) cmd->def,
												mys_table_default_collation(relid));
				/* FOREIGN_KEY_CHECKS=0: MySQL does not check existing rows */
				if (cmd->subtype == AT_AddConstraint && cmd->def &&
					IsA(cmd->def, Constraint) &&
					((Constraint *) cmd->def)->contype == CONSTR_FOREIGN &&
					!mys_foreign_key_checks())
				{
					((Constraint *) cmd->def)->skip_validation = true;
					((Constraint *) cmd->def)->initially_valid = false;
				}
			}
		pstmt = copyObject(pstmt);
		pstmt->utilityStmt = parsetree = (Node *) alter;
	}

	if (IsA(parsetree, CreateExtensionStmt) &&
		strcmp(((CreateExtensionStmt *) parsetree)->extname, "aux_mysql") == 0)
		halo_mysql_lock_setup();

	/* syntax error of a statement inside a transaction (mys_txn.c) */
	if (mys_txn_is_deferred_error(parsetree))
		mys_txn_raise_deferred_error();

	if (mys_is_mysql_session() && IsA(parsetree, TruncateStmt) &&
		truncate_referenced_tables((TruncateStmt *) parsetree))
		return;

	if (is_table_setup(parsetree))
	{
		mysql_table_setup((DoStmt *) parsetree, pstmt, queryString, params, queryEnv);
		return;
	}

	if (IsA(parsetree, AlterTableStmt) &&
		has_mysql_subcommands((AlterTableStmt *) parsetree))
	{
		mysql_alter_table(copyObject((AlterTableStmt *) parsetree), pstmt,
						  queryString, params, queryEnv);
		return;
	}

	/*
	 * START TRANSACTION / BEGIN: the characteristics of the new transaction,
	 * decided now rather than when the packet was parsed, so a SET earlier in
	 * the packet counts.  (A multi-statement packet runs in an implicit block,
	 * so IsTransactionBlock() is no test; MySQL's implicit COMMIT before START
	 * leaves a transaction without a snapshot.)  WITH CONSISTENT SNAPSHOT
	 * takes the snapshot at START, as InnoDB creates its read view then, not
	 * at the first query: parallel dumpers rely on that.
	 */
	if (mys_is_mysql_session() && is_transaction_start(parsetree) &&
		!FirstSnapshotSet && !IsSubTransaction())
	{
		TransactionStmt *start = copyObject((TransactionStmt *) parsetree);
		bool		snapshot = false;
		bool		isolation = false;
		int			explicit_read_only = -1;
		bool		read_only;
		const char *level;

		foreach_node(DefElem, opt, start->options)
		{
			if (strcmp(opt->defname, "mysql_consistent_snapshot") == 0)
				snapshot = true;
			else if (strcmp(opt->defname, "transaction_isolation") == 0)
				isolation = true;
			else if (strcmp(opt->defname, "transaction_read_only") == 0 &&
					 IsA(opt->arg, A_Const) && IsA(&((A_Const *) opt->arg)->val, Integer))
				explicit_read_only = intVal(&((A_Const *) opt->arg)->val) != 0;
		}
		level = mys_transaction_start(explicit_read_only, &snapshot, &read_only);
		if (!isolation)
			start->options = lappend(start->options,
									 makeDefElem("transaction_isolation",
												 makeStringConst(pstrdup(level), -1), -1));
		if (explicit_read_only < 0 && (read_only || !RecoveryInProgress()))
			start->options = lappend(start->options,
									 makeDefElem("transaction_read_only",
												 bool_const(read_only), -1));
		pstmt = copyObject(pstmt);
		pstmt->utilityStmt = (Node *) start;
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
									params, queryEnv, dest, qc);
		if (snapshot)
			(void) GetTransactionSnapshot();
		return;
	}

	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context,
							params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context,
								params, queryEnv, dest, qc);
}

/* GLOBAL read_only applies to this utility statement alone */
static void
mys_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	bool		saved = XactReadOnly;

	if (!mys_is_mysql_session() || !server_read_only_utility(pstmt->utilityStmt, context))
	{
		process_utility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
		return;
	}
	XactReadOnly = true;
	PG_TRY();
	{
		process_utility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	}
	PG_FINALLY();
	{
		XactReadOnly = saved;
	}
	PG_END_TRY();
}

void
mysql_utility_init(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = mys_ProcessUtility;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = mys_ExecutorStart;
}
