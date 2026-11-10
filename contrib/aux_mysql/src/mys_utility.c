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
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "commands/seclabel.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/functions.h"
#include "executor/spi.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
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
#include "utils/fmgroids.h"
#include "utils/ruleutils.h"
#include "miscadmin.h"

#include "halo_mysql.h"
#include "mys_fk.h"
#include "mys_parsenodes.h"
#include "mys_generated_policy.h"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static planner_hook_type prev_planner = NULL;
/* utility commands aux_mysql runs itself to implement a MySQL statement */
static int	internal_utility_depth = 0;
static bool collation_provider_registered = false;

/* row triggers maintained by mysql_table_setup() */
#define ON_UPDATE_TRIGGER	"aux_mysql_on_update"
#define AUTOINC_TRIGGER		"aux_mysql_auto_increment"

/* generated columns during a MySQL ALTER TABLE (mysql_alter_table()) */
typedef struct GenAlter
{
	List	   *changed;		/* names of the columns MODIFY / CHANGE redefine */
	List	   *detached;		/* generated columns set aside (GenColumn) */
} GenAlter;

static void detach_generated_readers(Oid relid, AttrNumber attnum, GenAlter *gen,
									 List **cmds);
static void refresh_generated(AlterTableStmt *stmt, Oid relid, GenAlter *gen,
							  PlannedStmt *pstmt, const char *queryString,
							  ParamListInfo params, QueryEnvironment *queryEnv);

/*
 * Bootstrap SET EXPRESSION for an ordinary column, without dropping its
 * identity (attnum), indexes, privileges or dependants.  The caller holds
 * AccessExclusiveLock and immediately runs SET EXPRESSION: PG18 performs the
 * rewrite and rebuilds/rechecks indexes and constraints.  The placeholder is
 * never used to write rows.  A failed rewrite rolls all catalog changes back.
 */
static void
make_column_generated(Oid relid, const char *name)
{
	Relation attrs = table_open(AttributeRelationId, RowExclusiveLock);
	HeapTuple tuple = SearchSysCacheCopyAttName(relid, name);
	Form_pg_attribute att;
	AttrNumber attnum;
	Node *placeholder;
	Relation rel;

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "column %s does not exist", name);
	att = (Form_pg_attribute) GETSTRUCT(tuple);
	Assert(!att->attgenerated && !att->attidentity && !att->atthasdef);
	attnum = att->attnum;
	placeholder = (Node *) makeNullConst(att->atttypid, att->atttypmod,
										 att->attcollation);
	att->attgenerated = ATTRIBUTE_GENERATED_STORED;
	CatalogTupleUpdate(attrs, &tuple->t_self, tuple);
	heap_freetuple(tuple);
	table_close(attrs, RowExclusiveLock);
	CommandCounterIncrement();
	rel = table_open(relid, NoLock);
	StoreAttrDefault(rel, attnum, placeholder, true);
	table_close(rel, NoLock);
	CommandCounterIncrement();
}

/*
 * Store the table default in PostgreSQL's provider-owned object labels.
 * Unlike an OID-keyed extension table this follows rename/drop and pg_dump
 * without an auxiliary table, event triggers or catalog/ABI modifications.
 * This provider stores comparison metadata; it does not grant privileges.
 */
static void
check_collation_label(const ObjectAddress *object, const char *label)
{
	/* a column: the declared kind of a generated column (mys_rewrite.c) */
	if (object->classId == RelationRelationId && object->objectSubId > 0 &&
		(get_rel_relkind(object->objectId) == RELKIND_RELATION ||
		 get_rel_relkind(object->objectId) == RELKIND_PARTITIONED_TABLE))
	{
		if (label && strcmp(label, MYS_GENERATED_VIRTUAL) != 0)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("invalid aux_mysql column label")));
		return;
	}
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

/* whether a generated column was declared VIRTUAL (it is stored) */
bool
mys_generated_virtual(Oid relid, AttrNumber attnum)
{
	ObjectAddress object;

	ObjectAddressSubSet(object, RelationRelationId, relid, attnum);
	return GetSecurityLabel(&object, "aux_mysql") != NULL;
}

static void
set_generated_virtual(Oid relid, AttrNumber attnum, bool is_virtual)
{
	ObjectAddress object;

	ObjectAddressSubSet(object, RelationRelationId, relid, attnum);
	SetSecurityLabel(&object, "aux_mysql", is_virtual ? MYS_GENERATED_VIRTUAL : NULL);
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

/* whether a generated column of the table other than attnum reads it */
static bool
column_used_by_generated(Oid relid, AttrNumber attnum)
{
	Relation	rel = relation_open(relid, NoLock);
	TupleConstr *constr = RelationGetDescr(rel)->constr;
	bool		used = false;

	for (int i = 0; constr && i < constr->num_defval && !used; i++)
	{
		Bitmapset  *attrs = NULL;

		if (constr->defval[i].adnum == attnum ||
			!TupleDescAttr(RelationGetDescr(rel), constr->defval[i].adnum - 1)->attgenerated)
			continue;
		pull_varattnos(stringToNode(constr->defval[i].adbin), 1, &attrs);
		used = bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, attrs);
	}
	relation_close(rel, NoLock);
	return used;
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
			  List **cmds, List **after, bool *added_identity, GenAlter *gen,
			  PlannedStmt *pstmt, const char *queryString,
			  ParamListInfo params, QueryEnvironment *queryEnv)
{
	ColumnDef  *tdef = makeNode(ColumnDef);
	TypeCast   *using = makeNode(TypeCast);
	ColumnRef  *cref = makeNode(ColumnRef);
	bool		notnull = def->is_not_null;
	Node	   *defexpr = NULL;
	Constraint *identity = NULL;
	Constraint *generated = NULL;
	AttrNumber	attnum = get_attnum(relid, colname);
	bool		was_identity = attnum > 0 && column_is_identity(relid, attnum);
	bool		was_generated = attnum > 0 && get_attgenerated(relid, attnum) != '\0';
	bool		keep_type = false;
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
			else if (c->contype == CONSTR_GENERATED)
				generated = c;
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
	 * new definition has no AUTO_INCREMENT.  A generated column keeps its
	 * expression until SET EXPRESSION below.
	 */
	if (was_generated)
	{
		if (!generated)
			*cmds = lappend(*cmds, make_cmd(AT_DropExpression, colname, NULL));
	}
	else if (!was_identity)
		*cmds = lappend(*cmds, make_cmd(AT_ColumnDefault, colname, NULL));
	else if (!identity)
		*cmds = lappend(*cmds, make_cmd(AT_DropIdentity, colname, NULL));
	if (generated || was_generated)
		gen->changed = lappend(gen->changed, colname);

	/*
	 * PostgreSQL refuses ALTER TYPE of a column a generated column reads,
	 * even to the same type: keep the type when MODIFY does, else set those
	 * columns aside until refresh_generated().
	 */
	if (generated || column_used_by_generated(relid, attnum))
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;
		Oid			new_typid;
		int32		new_typmod;

		get_atttypetypmodcoll(relid, attnum, &typid, &typmod, &collid);
		typenameTypeIdAndMod(NULL, def->typeName, &new_typid, &new_typmod);
		keep_type = new_typid == typid && new_typmod == typmod &&
			GetColumnDefCollation(NULL, def, new_typid) == collid;
		if (!keep_type && column_used_by_generated(relid, attnum))
			detach_generated_readers(relid, attnum, gen, cmds);
	}
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
	/* MySQL still converts existing ordinary values when MODIFY changes their
	 * type, before recomputing the generated values.  An already generated
	 * column instead takes its type from its expression, without USING. */
	tdef->raw_default = generated && was_generated ? NULL : (Node *) using;
	if (!keep_type)
		*cmds = lappend(*cmds, make_cmd(AT_AlterColumnType, colname, (Node *) tdef));
	if (generated && !was_generated)
	{
		if (!notnull && !column_in_primary_key(relid, colname))
			*cmds = lappend(*cmds, make_cmd(AT_DropNotNull, colname, NULL));
		flush_cmds(orig, cmds, pstmt, queryString, params, queryEnv);
		make_column_generated(relid, colname);
	}
	if (generated)
		*cmds = lappend(*cmds, make_cmd(AT_SetExpression, colname, generated->raw_expr));

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
	GenAlter	gen = {NIL, NIL};
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
							  &added_identity, &gen, pstmt, queryString, params, queryEnv);
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
								  &added_identity, &gen, pstmt, queryString, params, queryEnv);
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
				{
					/* DROP INDEX k [ON t]: the key of this table, PRIMARY included */
					Oid			indexoid = mys_find_key(relid, cmd->name);
					Oid			conoid;

					if (!OidIsValid(indexoid))
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_OBJECT),
								 errmsg("Can't DROP '%s'; check that column/key exists", cmd->name)));
					conoid = get_index_constraint(indexoid);
					if (OidIsValid(conoid))
						cmds = lappend(cmds, make_cmd(AT_DropConstraint, get_constraint_name(conoid), NULL));
					else
					{
						DropStmt   *drop = makeNode(DropStmt);

						flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);
						drop->removeType = OBJECT_INDEX;
						drop->objects = list_make1(list_make2(makeString(get_namespace_name(get_rel_namespace(indexoid))),
															  makeString(get_rel_name(indexoid))));
						drop->behavior = DROP_RESTRICT;
						run_utility((Node *) drop, pstmt, queryString, params, queryEnv);
					}
					break;
				}

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
	refresh_generated(stmt, relid, &gen, pstmt, queryString, params, queryEnv);

	/* a column that just became AUTO_INCREMENT continues after its maximum */
	if (added_identity)
		mys_sync_auto_increment(relid);
	if (has_start)
		mys_set_auto_increment(relid, start);

	foreach(lc, after)
		run_utility(lfirst(lc), pstmt, queryString, params, queryEnv);
}

/*
 * ALTER TABLE t RENAME {INDEX | KEY} old TO new, which PostgreSQL would read
 * as a rename of the table itself: the key of that table, with MySQL's errors.
 */
static void
mysql_rename_index(RenameStmt *stmt, PlannedStmt *pstmt, const char *queryString,
				   ParamListInfo params, QueryEnvironment *queryEnv)
{
	Oid			relid = RangeVarGetRelid(stmt->relation, ShareUpdateExclusiveLock,
										 stmt->missing_ok);
	Oid			indexoid;
	Oid			other;
	RenameStmt *rename;

	if (!OidIsValid(relid))
		return;
	if (pg_strcasecmp(stmt->subname, "PRIMARY") == 0 || pg_strcasecmp(stmt->newname, "PRIMARY") == 0)
		ereport(ERROR,
				(errcode(MYS_ERRCODE_WRONG_NAME_FOR_INDEX),
				 errmsg("Incorrect index name '%s'",
						pg_strcasecmp(stmt->subname, "PRIMARY") == 0 ? stmt->subname : stmt->newname)));
	indexoid = mys_find_key(relid, stmt->subname);
	if (!OidIsValid(indexoid))
		ereport(ERROR,
				(errcode(MYS_ERRCODE_KEY_DOES_NOT_EXIST),
				 errmsg("Key '%s' doesn't exist in table '%s'", stmt->subname,
						stmt->relation->relname)));
	other = mys_find_key(relid, stmt->newname);
	if (OidIsValid(other) && other != indexoid)
		ereport(ERROR,
				(errcode(MYS_ERRCODE_DUP_KEYNAME),
				 errmsg("Duplicate key name '%s'", stmt->newname)));
	if (strcmp(mys_index_key_name(indexoid), stmt->newname) == 0)
		return;

	/* ALTER INDEX renames the constraint of a UNIQUE key, too */
	rename = makeNode(RenameStmt);
	rename->renameType = OBJECT_INDEX;
	rename->relation = makeRangeVar(get_namespace_name(get_rel_namespace(indexoid)),
									get_rel_name(indexoid), -1);
	rename->newname = mys_key_pg_name(get_rel_name(relid), stmt->newname,
									  get_rel_namespace(relid), NIL);
	run_utility((Node *) rename, pstmt, queryString, params, queryEnv);
}

/* ----------------------------------------------------------------
 * Generated column expressions: col type AS (expr) (mys_rewrite.c).
 *
 * PostgreSQL computes all generated columns from the row as written and
 * wants an immutable expression.  MySQL lets a generated column read the
 * generated columns before it, and accepts deterministic functions that are
 * STABLE or VOLATILE here (CONCAT, IFNULL, /, DIV, DATE_FORMAT: they read
 * sql_mode or are written in PL/pgSQL).  So, in MySQL DDL:
 *
 *	- a reference to another generated column reads that column's expression,
 *	  cast to its type, as mysql.generated_column(name, expression): SHOW
 *	  shows the reference, and the reading columns follow when ALTER TABLE
 *	  redefines the column they read;
 *	- an expression PostgreSQL finds mutable becomes a call of an IMMUTABLE
 *	  SQL function in the table's schema, MYS_GENERATOR_PREFIX<hash>(the
 *	  columns it reads) RETURN expr, its parameters collated as the columns.
 *	  An ordinary function for pg_dump; identical expressions share one (LIKE
 *	  copies them); DROP TABLE / ALTER TABLE drop the ones left unused.
 * ----------------------------------------------------------------
 */
typedef struct GenColumn
{
	char	   *name;
	TypeName   *type;
	List	   *collname;		/* COLLATE of a parameter reading it, or NIL */
	Node	   *generation;		/* raw expression, once known */
	bool		generated;		/* a generated column others may read */
	Oid			relid;			/* expression still in the catalog */
	AttrNumber	attnum;
	bool		notnull;
} GenColumn;

static bool
name_member(List *names, const char *name)
{
	foreach_ptr(char, n, names)
		if (strcmp(n, name) == 0)
			return true;
	return false;
}

static GenColumn *
find_gen_column(List *columns, const char *name)
{
	foreach_ptr(GenColumn, c, columns)
		if (strcmp(c->name, name) == 0)
			return c;
	return NULL;
}

/*
 * Raw expression of a generated column: from the catalog through its text
 * (a generator's body, not its call).
 */
static Node *
gen_column_expression(GenColumn *c)
{
	if (c->generation == NULL && OidIsValid(c->relid))
	{
		Relation	rel = relation_open(c->relid, AccessShareLock);
		TupleConstr *constr = RelationGetDescr(rel)->constr;
		char	   *text = NULL;

		for (int i = 0; constr && i < constr->num_defval; i++)
			if (constr->defval[i].adnum == c->attnum)
			{
				gen_policy_check(stringToNode(constr->defval[i].adbin), true);
				text = deparse_expression(mys_unwrap_generator(stringToNode(constr->defval[i].adbin)),
										  deparse_context_for(RelationGetRelationName(rel),
															  c->relid),
										  false, false);
			}
		relation_close(rel, AccessShareLock);
		if (text)
		{
			RawStmt    *raw = linitial_node(RawStmt, raw_parser(text, RAW_PARSE_PLPGSQL_EXPR));

			c->generation = linitial_node(ResTarget,
										  castNode(SelectStmt, raw->stmt)->targetList)->val;
		}
	}
	return c->generation;
}

/* the column name of mysql.generated_column(name, expr), else NULL */
static char *
generated_ref_name(Node *node)
{
	FuncCall   *f = (FuncCall *) node;
	Node	   *arg;

	if (!IsA(node, FuncCall) || list_length(f->args) != 2 ||
		strcmp(strVal(llast(f->funcname)), MYS_GENERATED_REF) != 0 ||
		(list_length(f->funcname) == 2 && strcmp(strVal(linitial(f->funcname)), "mysql") != 0) ||
		list_length(f->funcname) > 2)
		return NULL;
	arg = linitial(f->args);
	if (IsA(arg, TypeCast))
		arg = ((TypeCast *) arg)->arg;
	if (!IsA(arg, A_Const) || ((A_Const *) arg)->isnull || !IsA(&((A_Const *) arg)->val, String))
		return NULL;
	return strVal(&((A_Const *) arg)->val);
}

/*
 * References to generated columns read their current expressions; one read
 * before (mysql.generated_column) is read again, or becomes a plain
 * reference when the column is no longer generated.
 */
static Node *
inline_generated(Node *node, List *columns)
{
	ListCell   *lc;
	char	   *name = NULL;
	GenColumn  *c;

	if (node == NULL)
		return NULL;
	if (IsA(node, ColumnRef) && list_length(((ColumnRef *) node)->fields) == 1 &&
		IsA(linitial(((ColumnRef *) node)->fields), String))
		name = strVal(linitial(((ColumnRef *) node)->fields));
	else
		name = generated_ref_name(node);
	if (name && (c = find_gen_column(columns, name)) != NULL)
	{
		TypeCast   *tc;
		A_Const    *ref;

		if (!c->generated || gen_column_expression(c) == NULL)
		{
			ColumnRef  *cr = makeNode(ColumnRef);

			if (IsA(node, ColumnRef))
				return node;
			cr->fields = list_make1(makeString(name));
			cr->location = -1;
			return (Node *) cr;
		}
		tc = makeNode(TypeCast);
		tc->arg = copyObject(c->generation);
		tc->typeName = copyObject(c->type);
		tc->location = -1;
		ref = makeNode(A_Const);
		ref->val.sval.type = T_String;
		ref->val.sval.sval = pstrdup(name);
		ref->location = -1;
		return (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString(MYS_GENERATED_REF)),
									 list_make2(ref, tc), COERCE_EXPLICIT_CALL, -1);
	}
	if (name)
		return node;			/* a column of another kind of reference */
	switch (nodeTag(node))
	{
		case T_List:
			foreach(lc, (List *) node)
				lfirst(lc) = inline_generated(lfirst(lc), columns);
			break;
		case T_A_Expr:
			((A_Expr *) node)->lexpr = inline_generated(((A_Expr *) node)->lexpr, columns);
			((A_Expr *) node)->rexpr = inline_generated(((A_Expr *) node)->rexpr, columns);
			break;
		case T_FuncCall:
			((FuncCall *) node)->args = (List *) inline_generated((Node *) ((FuncCall *) node)->args, columns);
			break;
		case T_TypeCast:
			((TypeCast *) node)->arg = inline_generated(((TypeCast *) node)->arg, columns);
			break;
		case T_CollateClause:
			((CollateClause *) node)->arg = inline_generated(((CollateClause *) node)->arg, columns);
			break;
		case T_BoolExpr:
			((BoolExpr *) node)->args = (List *) inline_generated((Node *) ((BoolExpr *) node)->args, columns);
			break;
		case T_NullTest:
			((NullTest *) node)->arg = (Expr *) inline_generated((Node *) ((NullTest *) node)->arg, columns);
			break;
		case T_BooleanTest:
			((BooleanTest *) node)->arg = (Expr *) inline_generated((Node *) ((BooleanTest *) node)->arg, columns);
			break;
		case T_CaseExpr:
			((CaseExpr *) node)->arg = (Expr *) inline_generated((Node *) ((CaseExpr *) node)->arg, columns);
			((CaseExpr *) node)->args = (List *) inline_generated((Node *) ((CaseExpr *) node)->args, columns);
			((CaseExpr *) node)->defresult = (Expr *) inline_generated((Node *) ((CaseExpr *) node)->defresult, columns);
			break;
		case T_CaseWhen:
			((CaseWhen *) node)->expr = (Expr *) inline_generated((Node *) ((CaseWhen *) node)->expr, columns);
			((CaseWhen *) node)->result = (Expr *) inline_generated((Node *) ((CaseWhen *) node)->result, columns);
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) node)->args = (List *) inline_generated((Node *) ((CoalesceExpr *) node)->args, columns);
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) node)->args = (List *) inline_generated((Node *) ((MinMaxExpr *) node)->args, columns);
			break;
		case T_NamedArgExpr:
			((NamedArgExpr *) node)->arg = (Expr *) inline_generated((Node *) ((NamedArgExpr *) node)->arg, columns);
			break;
		default:
			/* other nodes keep their references; PostgreSQL reports them */
			break;
	}
	return node;
}

/* names of the columns an expression reads, in order */
static bool
collect_column_refs(Node *node, List **names)
{
	if (node == NULL || nodeTag(node) >= T_MysNodeBase)
		return false;
	if (IsA(node, ColumnRef))
	{
		ColumnRef  *cr = (ColumnRef *) node;

		if (list_length(cr->fields) == 1 && IsA(linitial(cr->fields), String))
		{
			foreach_ptr(char, name, *names)
				if (strcmp(name, strVal(linitial(cr->fields))) == 0)
					return false;
			*names = lappend(*names, strVal(linitial(cr->fields)));
		}
		return false;
	}
	return raw_expression_tree_walker(node, collect_column_refs, names);
}

/* the expression with its column references collated as the columns */
static Node *
collate_params(Node *node, List *columns)
{
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	if (IsA(node, ColumnRef))
	{
		ColumnRef  *cr = (ColumnRef *) node;
		GenColumn  *c = list_length(cr->fields) == 1 && IsA(linitial(cr->fields), String) ?
			find_gen_column(columns, strVal(linitial(cr->fields))) : NULL;
		CollateClause *coll;

		if (c == NULL || c->collname == NIL)
			return node;
		coll = makeNode(CollateClause);
		coll->arg = node;
		coll->collname = copyObject(c->collname);
		coll->location = -1;
		return (Node *) coll;
	}
	if (IsA(node, List))
	{
		foreach(lc, (List *) node)
			lfirst(lc) = collate_params(lfirst(lc), columns);
		return node;
	}
	/* the node types inline_generated() handles, same fields */
	switch (nodeTag(node))
	{
		case T_A_Expr:
			((A_Expr *) node)->lexpr = collate_params(((A_Expr *) node)->lexpr, columns);
			((A_Expr *) node)->rexpr = collate_params(((A_Expr *) node)->rexpr, columns);
			break;
		case T_FuncCall:
			((FuncCall *) node)->args = (List *) collate_params((Node *) ((FuncCall *) node)->args, columns);
			break;
		case T_TypeCast:
			((TypeCast *) node)->arg = collate_params(((TypeCast *) node)->arg, columns);
			break;
		case T_CollateClause:
			((CollateClause *) node)->arg = collate_params(((CollateClause *) node)->arg, columns);
			break;
		case T_BoolExpr:
			((BoolExpr *) node)->args = (List *) collate_params((Node *) ((BoolExpr *) node)->args, columns);
			break;
		case T_NullTest:
			((NullTest *) node)->arg = (Expr *) collate_params((Node *) ((NullTest *) node)->arg, columns);
			break;
		case T_BooleanTest:
			((BooleanTest *) node)->arg = (Expr *) collate_params((Node *) ((BooleanTest *) node)->arg, columns);
			break;
		case T_CaseExpr:
			((CaseExpr *) node)->arg = (Expr *) collate_params((Node *) ((CaseExpr *) node)->arg, columns);
			((CaseExpr *) node)->args = (List *) collate_params((Node *) ((CaseExpr *) node)->args, columns);
			((CaseExpr *) node)->defresult = (Expr *) collate_params((Node *) ((CaseExpr *) node)->defresult, columns);
			break;
		case T_CaseWhen:
			((CaseWhen *) node)->expr = (Expr *) collate_params((Node *) ((CaseWhen *) node)->expr, columns);
			((CaseWhen *) node)->result = (Expr *) collate_params((Node *) ((CaseWhen *) node)->result, columns);
			break;
		case T_CoalesceExpr:
			((CoalesceExpr *) node)->args = (List *) collate_params((Node *) ((CoalesceExpr *) node)->args, columns);
			break;
		case T_MinMaxExpr:
			((MinMaxExpr *) node)->args = (List *) collate_params((Node *) ((MinMaxExpr *) node)->args, columns);
			break;
		case T_NamedArgExpr:
			((NamedArgExpr *) node)->arg = (Expr *) collate_params((Node *) ((NamedArgExpr *) node)->arg, columns);
			break;
		default:
			break;
	}
	return node;
}

/*
 * The expression of a generated column, raw: as is when PostgreSQL finds it
 * immutable, else a call of its generator function (created if needed).
 */
static Node *
generation_expression(Node *raw, TypeName *type, List *columns, Oid nspid,
					  PlannedStmt *pstmt, const char *queryString,
					  ParamListInfo params, QueryEnvironment *queryEnv)
{
	List	   *names = NIL;
	List	   *known = NIL;
	SQLFunctionParseInfo *pinfo = palloc0(sizeof(SQLFunctionParseInfo));
	ParseState *pstate = make_parsestate(NULL);
	Node	   *expr;
	Node	   *body;
	Oid		   *argtypes;
	List	   *funcname;
	List	   *args = NIL;
	StringInfoData key;
	char		name[NAMEDATALEN];
	int			n = 0;
	Oid			target;
	Oid			base;
	int32		typmod;
	TypeCast   *cast;
	Oid			marker_arg = ANYELEMENTOID;
	bool		has_conversion_helpers;
	bool		row_anchor;
	bool		bool_string;

	(void) gen_policy_raw(raw, NULL);
	(void) collect_column_refs(raw, &names);
	foreach_ptr(char, colname, names)
		if (find_gen_column(columns, colname))
			known = lappend(known, colname);
	argtypes = palloc(sizeof(Oid) * (list_length(known) + 1));
	pinfo->fname = "generated column";
	pinfo->argnames = palloc(sizeof(char *) * (list_length(known) + 1));
	foreach_ptr(char, colname, known)
	{
		pinfo->argnames[n] = colname;
		argtypes[n++] = typenameTypeId(NULL, find_gen_column(columns, colname)->type);
	}
	pinfo->nargs = n;
	pinfo->argtypes = argtypes;
	sql_fn_parser_setup(pstate, pinfo);
	expr = transformExpr(pstate, copyObject(raw), EXPR_KIND_GENERATED_COLUMN);
	free_parsestate(pstate);
	gen_policy_check(expr, false);
	typenameTypeIdAndMod(NULL, type, &target, &typmod);
	base = getBaseTypeAndTypmod(target, &typmod);
	/* MySQL stores a predicate as 1/0 in a string column, never true/false
	 * (mys_assign_bool_as_int); the wrapper keeps SHOW's original text. */
	bool_string = exprType(expr) == BOOLOID &&
		(base == VARCHAROID || base == BPCHAROID || base == TEXTOID);
	if (!bool_string && !contain_mutable_functions(expr) &&
		!((base == VARCHAROID || base == BPCHAROID || base == NUMERICOID) && typmod >= VARHDRSZ))
		return raw;

	/* SQL function RETURN uses implicit coercion, whereas a generated column
	 * uses assignment coercion.  Cast explicitly inside the wrapper so domain
	 * (notably BLOB) and typmod conversion happens before SQL RETURN checking.
	 * The marker lets SHOW/ALTER recover the original expression losslessly. */
	/* A binary upgrade precedes ALTER EXTENSION. Keep the legacy expression
	 * form while the 1.9 SQL helpers are absent; invalidate naturally by
	 * looking them up for each generated-column DDL operation. */
	has_conversion_helpers = OidIsValid(LookupFuncName(
		list_make2(makeString("mysql"), makeString("generated_value")),
		1, &marker_arg, true));
	body = collate_params(copyObject(raw), columns);
	/* A bare string/NULL is UNKNOWN until assignment.  The polymorphic source
	 * marker needs a concrete type, without applying the target typmod yet. */
	if (exprType(expr) == UNKNOWNOID)
	{
		cast = makeNode(TypeCast);
		cast->arg = body;
		cast->typeName = makeTypeNameFromOid(TEXTOID, -1);
		cast->location = -1;
		body = (Node *) cast;
	}
	if (has_conversion_helpers)
		body = (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("generated_value")),
									 list_make1(body), COERCE_EXPLICIT_CALL, -1);
	if (bool_string)
	{
		cast = makeNode(TypeCast);
		cast->arg = body;
		cast->typeName = makeTypeNameFromOid(INT4OID, -1);
		cast->location = -1;
		body = (Node *) cast;
	}
	if (has_conversion_helpers && (base == VARCHAROID || base == BPCHAROID) && typmod >= VARHDRSZ)
	{
		A_Const *limit = makeNode(A_Const);
		A_Const *bpchar = makeNode(A_Const);

		limit->val.ival.type = T_Integer;
		limit->val.ival.ival = typmod - VARHDRSZ;
		limit->location = -1;
		bpchar->val.boolval.type = T_Boolean;
		bpchar->val.boolval.boolval = base == BPCHAROID;
		bpchar->location = -1;
		cast = makeNode(TypeCast);
		cast->arg = body;
		cast->typeName = makeTypeNameFromOid(TEXTOID, -1);
		cast->location = -1;
		body = (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("generated_varchar")),
			list_make3(cast, limit, bpchar), COERCE_EXPLICIT_CALL, -1);
	}
	else if (has_conversion_helpers && base == NUMERICOID && typmod >= VARHDRSZ)
	{
		Oid helper_args[2] = {NUMERICOID, INT4OID};

		/* Keep binary-before-SQL upgrades usable when this helper is absent. */
		if (OidIsValid(LookupFuncName(list_make2(makeString("mysql"), makeString("generated_numeric")),
			2, helper_args, true)))
		{
			A_Const *limit = makeNode(A_Const);

			limit->val.ival.type = T_Integer;
			limit->val.ival.ival = typmod;
			limit->location = -1;
			cast = makeNode(TypeCast);
			cast->arg = body;
			cast->typeName = makeTypeNameFromOid(NUMERICOID, -1);
			cast->location = -1;
			body = (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("generated_numeric")),
				list_make2(cast, limit), COERCE_EXPLICIT_CALL, -1);
		}
	}
	cast = makeNode(TypeCast);
	cast->arg = body;
	cast->typeName = copyObject(type);
	cast->location = -1;
	body = (Node *) cast;
	/* Even a constant source must run its assignment conversion for each row
	 * in the writer's current sql_mode/IGNORE context.  A zero-argument
	 * IMMUTABLE wrapper is otherwise folded at plan creation.  tableoid is
	 * PG's permitted generated-column system Var; the body need not read it.
	 * SET search_path below also prevents SQL function inlining. */
	row_anchor = n == 0;
	if (row_anchor)
		argtypes[n++] = OIDOID;

	/* named after what it computes, including the typmod and conversion */
	initStringInfo(&key);
	appendStringInfo(&key, "2|%s|%s|%u", nodeToString(body),
					 GetConfigOption("search_path", false, false),
					 typenameTypeId(NULL, type));
	for (int i = 0; i < n; i++)
		appendStringInfo(&key, ",%u", argtypes[i]);
	snprintf(name, sizeof(name), "%s%016" PRIx64 "%016" PRIx64, MYS_GENERATOR_PREFIX,
			 hash_bytes_extended((unsigned char *) key.data, key.len, 0),
			 hash_bytes_extended((unsigned char *) key.data, key.len, UINT64CONST(0x9e3779b97f4a7c15)));
	funcname = list_make2(makeString(get_namespace_name(nspid)), makeString(pstrdup(name)));

	if (!OidIsValid(LookupFuncName(funcname, n, argtypes, true)))
	{
		CreateFunctionStmt *f = makeNode(CreateFunctionStmt);
		ReturnStmt *ret = makeNode(ReturnStmt);
		VariableSetStmt *path = makeNode(VariableSetStmt);

		foreach_ptr(char, colname, known)
		{
			FunctionParameter *p = makeNode(FunctionParameter);

			p->name = colname;
			p->argType = copyObject(find_gen_column(columns, colname)->type);
			p->mode = FUNC_PARAM_IN;
			p->location = -1;
			f->parameters = lappend(f->parameters, p);
		}
		if (row_anchor)
		{
			FunctionParameter *p = makeNode(FunctionParameter);

			p->argType = makeTypeNameFromOid(OIDOID, -1);
			p->mode = FUNC_PARAM_IN;
			p->location = -1;
			f->parameters = lappend(f->parameters, p);
		}
		/*
		 * aux_mysql's PL/pgSQL functions resolve their names at run time,
		 * through search_path: mysql, as in MySQL sessions, also when a
		 * PostgreSQL session or pg_restore writes the table.
		 */
		path->kind = VAR_SET_VALUE;
		path->name = "search_path";
		path->args = list_make2(makeStringConst("mysql", -1), makeStringConst("pg_catalog", -1));
		f->funcname = funcname;
		f->returnType = copyObject(type);
		f->options = list_make2(makeDefElem("volatility", (Node *) makeString("immutable"), -1),
								makeDefElem("set", (Node *) path, -1));
		ret->returnval = body;
		f->sql_body = (Node *) ret;
		run_utility((Node *) f, pstmt, queryString, params, queryEnv);
		CommandCounterIncrement();
	}
	foreach_ptr(char, colname, known)
	{
		ColumnRef  *cr = makeNode(ColumnRef);

		cr->fields = list_make1(makeString(colname));
		cr->location = -1;
		args = lappend(args, cr);
	}
	if (row_anchor)
	{
		ColumnRef *cr = makeNode(ColumnRef);

		cr->fields = list_make1(makeString("tableoid"));
		cr->location = -1;
		args = lappend(args, cr);
	}
	return (Node *) makeFuncCall(funcname, args, COERCE_EXPLICIT_CALL, -1);
}

static Constraint *
generated_constraint(ColumnDef *def)
{
	foreach_ptr(Node, n, def->constraints)
		if (IsA(n, Constraint) && ((Constraint *) n)->contype == CONSTR_GENERATED)
			return (Constraint *) n;
	return NULL;
}

/* the columns of a table, in order */
static List *
table_gen_columns(Oid relid)
{
	List	   *columns = NIL;
	Relation	rel;

	if (!OidIsValid(relid))
		return NIL;
	rel = relation_open(relid, AccessShareLock);
	for (int i = 0; i < RelationGetDescr(rel)->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), i);
		GenColumn  *c;

		if (att->attisdropped)
			continue;
		c = palloc0(sizeof(GenColumn));
		c->name = pstrdup(NameStr(att->attname));
		c->type = makeTypeNameFromOid(att->atttypid, att->atttypmod);
		if (OidIsValid(att->attcollation) &&
			att->attcollation != get_typcollation(att->atttypid))
		{
			HeapTuple	tup = SearchSysCache1(COLLOID, ObjectIdGetDatum(att->attcollation));
			Form_pg_collation coll = (Form_pg_collation) GETSTRUCT(tup);

			c->collname = list_make2(makeString(get_namespace_name(coll->collnamespace)),
									 makeString(pstrdup(NameStr(coll->collname))));
			ReleaseSysCache(tup);
		}
		c->generated = att->attgenerated != '\0';
		c->relid = relid;
		c->attnum = att->attnum;
		c->notnull = att->attnotnull;
		columns = lappend(columns, c);
	}
	relation_close(rel, AccessShareLock);
	return columns;
}

/*
 * The generated columns among defs, the column definitions of a CREATE TABLE
 * or ALTER TABLE (relid) in order: their expressions as PostgreSQL takes them.
 */
static void
mysql_generated_columns(List *defs, Oid relid, Oid nspid,
						PlannedStmt *pstmt, const char *queryString,
						ParamListInfo params, QueryEnvironment *queryEnv)
{
	List	   *columns;
	bool		any = false;

	foreach_node(ColumnDef, def, defs)
		any |= generated_constraint(def) != NULL;
	if (!any)
		return;
	columns = table_gen_columns(relid);
	/* the statement's definitions replace the catalog's (MODIFY) */
	foreach_node(ColumnDef, def, defs)
	{
		GenColumn  *c = find_gen_column(columns, def->colname);

		if (c == NULL)
		{
			c = palloc0(sizeof(GenColumn));
			c->name = def->colname;
			columns = lappend(columns, c);
		}
		c->type = def->typeName;
		c->collname = def->collClause ? def->collClause->collname : NIL;
		c->generation = NULL;
		c->generated = false;
		c->relid = InvalidOid;
	}
	foreach_node(ColumnDef, def, defs)
	{
		Constraint *g = generated_constraint(def);
		GenColumn  *c = find_gen_column(columns, def->colname);

		if (g == NULL || g->raw_expr == NULL)
			continue;
		c->generation = inline_generated(copyObject(g->raw_expr), columns);
		c->generated = true;
		g->raw_expr = generation_expression(c->generation, def->typeName, columns, nspid,
											pstmt, queryString, params, queryEnv);
	}
}

/* generator functions called by the generated columns of a table */
static List *
table_generators(Oid relid, List *result)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *attrdefs = NIL;

	if (!OidIsValid(relid))
		return result;
	depRel = table_open(DependRelationId, AccessShareLock);
	/* the column expressions, which depend on the table */
	ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);

		if (dep->classid == AttrDefaultRelationId)
			attrdefs = list_append_unique_oid(attrdefs, dep->objid);
	}
	systable_endscan(scan);
	/* and the functions they call */
	foreach_oid(attrdef, attrdefs)
	{
		ScanKeyInit(&key[0], Anum_pg_depend_classid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AttrDefaultRelationId));
		ScanKeyInit(&key[1], Anum_pg_depend_objid, BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(attrdef));
		scan = systable_beginscan(depRel, DependDependerIndexId, true, NULL, 2, key);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
			char	   *name;

			if (dep->refclassid != ProcedureRelationId)
				continue;
			name = get_func_name(dep->refobjid);
			if (name && strncmp(name, MYS_GENERATOR_PREFIX, strlen(MYS_GENERATOR_PREFIX)) == 0)
				result = list_append_unique_oid(result, dep->refobjid);
		}
		systable_endscan(scan);
	}
	table_close(depRel, AccessShareLock);
	return result;
}

/* generator functions a DROP TABLE / ALTER TABLE may leave unused */
static List *
statement_generators(Node *stmt)
{
	List	   *result = NIL;

	if (IsA(stmt, DropStmt) && ((DropStmt *) stmt)->removeType == OBJECT_TABLE)
	{
		foreach_node(List, name, ((DropStmt *) stmt)->objects)
			result = table_generators(RangeVarGetRelid(makeRangeVarFromNameList(name),
													   NoLock, true), result);
	}
	else if (IsA(stmt, AlterTableStmt))
		result = table_generators(RangeVarGetRelid(((AlterTableStmt *) stmt)->relation,
												   NoLock, true), result);
	return result;
}

static void
drop_unused_generators(List *generators)
{
	bool		dropped = true;

	/* a generator may call another */
	while (dropped)
	{
		dropped = false;
		foreach_oid(func, generators)
		{
			Relation	depRel;
			ScanKeyData key[2];
			SysScanDesc scan;
			bool		used;
			ObjectAddress object;

			if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(func)))
				continue;
			depRel = table_open(DependRelationId, AccessShareLock);
			ScanKeyInit(&key[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(ProcedureRelationId));
			ScanKeyInit(&key[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(func));
			scan = systable_beginscan(depRel, DependReferenceIndexId, true, NULL, 2, key);
			used = HeapTupleIsValid(systable_getnext(scan));
			systable_endscan(scan);
			table_close(depRel, AccessShareLock);
			if (used)
				continue;
			ObjectAddressSet(object, ProcedureRelationId, func);
			performDeletion(&object, DROP_RESTRICT,
							PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);
			CommandCounterIncrement();
			dropped = true;
		}
	}
}

/* whether a raw expression reads a column named (as a generated column) */
static bool
reads_columns(Node *node, List *names)
{
	char	   *name;

	if (node == NULL || nodeTag(node) >= T_MysNodeBase)
		return false;
	if ((name = generated_ref_name(node)) != NULL && name_member(names, name))
		return true;
	if (IsA(node, ColumnRef) && list_length(((ColumnRef *) node)->fields) == 1 &&
		IsA(linitial(((ColumnRef *) node)->fields), String))
		return name_member(names, strVal(linitial(((ColumnRef *) node)->fields)));
	return raw_expression_tree_walker(node, reads_columns, names);
}

/* whether a cooked expression reads generated column name */
static bool
reads_generated(Node *node, const char *name)
{
	if (node == NULL)
		return false;
	if (IsA(node, FuncExpr) && list_length(((FuncExpr *) node)->args) == 2 &&
		IsA(linitial(((FuncExpr *) node)->args), Const))
	{
		Const	   *ref = linitial_node(Const, ((FuncExpr *) node)->args);
		char	   *func = get_func_name(((FuncExpr *) node)->funcid);

		if (func && strcmp(func, MYS_GENERATED_REF) == 0 && !ref->constisnull &&
			ref->consttype == TEXTOID &&
			strcmp(TextDatumGetCString(ref->constvalue), name) == 0)
			return true;
	}
	return expression_tree_walker(node, reads_generated, (void *) name);
}

/*
 * DROP COLUMN or a rename of a generated column other generated columns
 * read (not dropped as well): refused, as in MySQL.
 */
static void
check_generated_readers(Oid relid, const char *name, bool drop, List *dropped)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleConstr *constr = RelationGetDescr(rel)->constr;

	for (int i = 0; constr && i < constr->num_defval; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), constr->defval[i].adnum - 1);

		if (!att->attgenerated || name_member(dropped, NameStr(att->attname)) ||
			!reads_generated(mys_unwrap_generator(stringToNode(constr->defval[i].adbin)), name))
			continue;
		if (drop)
			ereport(ERROR,
					(errcode(MAKE_SQLSTATE('M','3','1','0','8')),
					 errmsg("Column '%s' has a generated column dependency.", name)));
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("Unknown column '%s' in 'generated column function'", name)));
	}
	relation_close(rel, AccessShareLock);
}

/*
 * Before ALTER TYPE of column attnum: the generated columns reading it keep
 * no expression (PostgreSQL refuses the type change otherwise) until
 * refresh_generated() gives them theirs again, read now.
 */
static void
detach_generated_readers(Oid relid, AttrNumber attnum, GenAlter *gen, List **cmds)
{
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleConstr *constr = RelationGetDescr(rel)->constr;
	List	   *readers = NIL;

	for (int i = 0; constr && i < constr->num_defval; i++)
	{
		Bitmapset  *attrs = NULL;

		if (!TupleDescAttr(RelationGetDescr(rel), constr->defval[i].adnum - 1)->attgenerated)
			continue;
		pull_varattnos(stringToNode(constr->defval[i].adbin), 1, &attrs);
		if (bms_is_member(attnum - FirstLowInvalidHeapAttributeNumber, attrs))
			readers = lappend_int(readers, constr->defval[i].adnum);
	}
	relation_close(rel, AccessShareLock);

	foreach_ptr(GenColumn, c, table_gen_columns(relid))
	{
		A_Const    *null = makeNode(A_Const);

		if (!list_member_int(readers, c->attnum))
			continue;
		(void) gen_column_expression(c);
		c->relid = InvalidOid;
		gen->detached = lappend(gen->detached, c);
		if (c->notnull)
			*cmds = lappend(*cmds, make_cmd(AT_DropNotNull, c->name, NULL));
		null->isnull = true;
		null->location = -1;
		*cmds = lappend(*cmds, make_cmd(AT_SetExpression, c->name, (Node *) null));
	}
}

/*
 * A table LIKE another in another schema calls that schema's generators: its
 * generated columns would go with that schema (DROP DATABASE).  Give them
 * generators in the table's own schema.
 */
static void
rehome_generators(Oid relid, PlannedStmt *pstmt, const char *queryString,
				  ParamListInfo params, QueryEnvironment *queryEnv)
{
	Oid			nspid = get_rel_namespace(relid);
	List	   *columns = table_gen_columns(relid);
	List	   *foreign = NIL;
	List	   *cmds = NIL;
	AlterTableStmt *stmt;
	Relation	rel = relation_open(relid, AccessShareLock);
	TupleConstr *constr = RelationGetDescr(rel)->constr;

	for (int i = 0; constr && i < constr->num_defval; i++)
	{
		Node	   *call = strip_implicit_coercions(stringToNode(constr->defval[i].adbin));

		if (TupleDescAttr(RelationGetDescr(rel), constr->defval[i].adnum - 1)->attgenerated &&
			IsA(call, FuncExpr) && get_func_namespace(((FuncExpr *) call)->funcid) != nspid &&
			mys_unwrap_generator(call) != call)
			foreign = lappend_int(foreign, constr->defval[i].adnum);
	}
	relation_close(rel, AccessShareLock);
	if (foreign == NIL)
		return;
	foreach_ptr(GenColumn, c, columns)
	{
		Node	   *raw;

		if (!list_member_int(foreign, c->attnum) || (raw = gen_column_expression(c)) == NULL)
			continue;
		cmds = lappend(cmds, make_cmd(AT_SetExpression, c->name,
									  generation_expression(raw, c->type, columns, nspid,
															pstmt, queryString, params, queryEnv)));
	}
	stmt = makeNode(AlterTableStmt);
	stmt->relation = makeRangeVar(get_namespace_name(nspid), get_rel_name(relid), -1);
	stmt->objtype = OBJECT_TABLE;
	flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);
}

/*
 * After MODIFY / CHANGE: the generated columns set aside, and those reading
 * a generated column the statement redefined (through
 * mysql.generated_column), get their expressions again, in column order.
 */
static void
refresh_generated(AlterTableStmt *stmt, Oid relid, GenAlter *gen,
				  PlannedStmt *pstmt, const char *queryString,
				  ParamListInfo params, QueryEnvironment *queryEnv)
{
	List	   *columns;
	List	   *changed;
	List	   *cmds = NIL;

	if (gen->changed == NIL && gen->detached == NIL)
		return;
	columns = table_gen_columns(relid);
	changed = list_copy(gen->changed);
	foreach_ptr(GenColumn, c, columns)
	{
		GenColumn  *detached = NULL;
		Node	   *raw;

		foreach_ptr(GenColumn, d, gen->detached)
			if (strcmp(d->name, c->name) == 0)
				detached = d;
		if (!c->generated)
			continue;
		if (detached)
			raw = detached->generation;
		else if (name_member(gen->changed, c->name) ||
				 (raw = gen_column_expression(c)) == NULL || !reads_columns(raw, changed))
			continue;
		c->relid = InvalidOid;
		c->generation = inline_generated(copyObject(raw), columns);
		cmds = lappend(cmds, make_cmd(AT_SetExpression, c->name,
									  generation_expression(c->generation, c->type, columns,
															get_rel_namespace(relid),
															pstmt, queryString, params, queryEnv)));
		if (detached && detached->notnull)
			cmds = lappend(cmds, make_cmd(AT_SetNotNull, c->name, NULL));
		changed = lappend(changed, c->name);
	}
	flush_cmds(stmt, &cmds, pstmt, queryString, params, queryEnv);
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

/*
 * CREATE TABLE t LIKE source: PostgreSQL clones the source's indexes, in
 * OID order, under names of its own; MySQL keeps the key names.
 */
static void
copy_key_names(Oid relid, RangeVar *source, PlannedStmt *pstmt, const char *queryString,
			   ParamListInfo params, QueryEnvironment *queryEnv)
{
	Oid			sourceid = RangeVarGetRelid(source, NoLock, true);
	Relation	rel;
	List	   *from;
	List	   *to;
	ListCell   *lc1;
	ListCell   *lc2;

	if (!OidIsValid(sourceid))
		return;
	rel = relation_open(sourceid, AccessShareLock);
	from = RelationGetIndexList(rel);
	relation_close(rel, AccessShareLock);
	rel = relation_open(relid, AccessShareLock);
	to = RelationGetIndexList(rel);
	relation_close(rel, AccessShareLock);
	if (list_length(from) != list_length(to))
		return;

	forboth(lc1, from, lc2, to)
	{
		char	   *key = mys_index_key_name(lfirst_oid(lc1));
		char	   *now = mys_index_key_name(lfirst_oid(lc2));
		RenameStmt *rename;

		if (key == NULL || now == NULL || strcmp(key, now) == 0 || strcmp(key, "PRIMARY") == 0)
			continue;
		rename = makeNode(RenameStmt);
		rename->renameType = OBJECT_INDEX;
		rename->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)),
										get_rel_name(lfirst_oid(lc2)), -1);
		rename->newname = mys_key_pg_name(get_rel_name(relid), key, get_rel_namespace(relid), NIL);
		run_utility((Node *) rename, pstmt, queryString, params, queryEnv);
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

		if (strcmp(e->defname, "like_key_names") == 0)
		{
			copy_key_names(relid, (RangeVar *) e->arg, pstmt, queryString, params, queryEnv);
			continue;
		}
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
		/* generated columns: the kind they were declared with */
		if (strcmp(e->defname, "generated_virtual") == 0 ||
			strcmp(e->defname, "generated_stored") == 0)
		{
			attnum = get_attnum(relid, strVal(e->arg));
			if (attnum > 0)
				set_generated_virtual(relid, attnum,
									  strcmp(e->defname, "generated_virtual") == 0);
			continue;
		}
		if (strcmp(e->defname, "generated_like") == 0)
		{
			Oid			source = RangeVarGetRelid((RangeVar *) e->arg, NoLock, true);

			if (OidIsValid(source))
				for (AttrNumber col = 1; col <= natts; col++)
				{
					char	   *name = get_attname(relid, col, true);
					AttrNumber	from = name ? get_attnum(source, name) : InvalidAttrNumber;

					if (from > 0 && get_attgenerated(relid, col) &&
						mys_generated_virtual(source, from))
						set_generated_virtual(relid, col, true);
				}
			rehome_generators(relid, pstmt, queryString, params, queryEnv);
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
		case T_CreateSeqStmt:
			return ((CreateSeqStmt *) stmt)->sequence->relpersistence == RELPERSISTENCE_TEMP;
		case T_AlterSeqStmt:
			return is_temp_rangevar(((AlterSeqStmt *) stmt)->sequence);
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
			if (((DropStmt *) stmt)->removeType != OBJECT_TABLE &&
				((DropStmt *) stmt)->removeType != OBJECT_SEQUENCE)
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
 * Check writes directly, without changing XactReadOnly.  A temporary override
 * can register a SERIALIZABLE snapshot as read-only, be restored by subxact
 * cleanup while a utility is still running, or survive across a utility's
 * internal COMMIT.  None of those is a transaction access-mode change.
 *
 * DDL tags follow PG18's ClassifyUtilityCommandAsReadOnly (which is static).
 * Executable containers (EXPLAIN, DO, CALL, EXECUTE) are checked when their
 * inner commands run.  Maintenance commands keep PG's read-only allowance.
 */
static bool
server_read_only_utility(Node *stmt, ProcessUtilityContext context)
{
	if (!mys_server_read_only_applies() || internal_utility_depth != 0 ||
		context == PROCESS_UTILITY_SUBCOMMAND || temp_table_utility(stmt))
		return false;

	switch (nodeTag(stmt))
	{
		case T_AlterCollationStmt:
		case T_AlterDatabaseRefreshCollStmt:
		case T_AlterDatabaseSetStmt:
		case T_AlterDatabaseStmt:
		case T_AlterDefaultPrivilegesStmt:
		case T_AlterDomainStmt:
		case T_AlterEnumStmt:
		case T_AlterEventTrigStmt:
		case T_AlterExtensionContentsStmt:
		case T_AlterExtensionStmt:
		case T_AlterFdwStmt:
		case T_AlterForeignServerStmt:
		case T_AlterFunctionStmt:
		case T_AlterObjectDependsStmt:
		case T_AlterObjectSchemaStmt:
		case T_AlterOpFamilyStmt:
		case T_AlterOperatorStmt:
		case T_AlterOwnerStmt:
		case T_AlterPolicyStmt:
		case T_AlterPublicationStmt:
		case T_AlterRoleSetStmt:
		case T_AlterRoleStmt:
		case T_AlterSeqStmt:
		case T_AlterStatsStmt:
		case T_AlterSubscriptionStmt:
		case T_AlterTSConfigurationStmt:
		case T_AlterTSDictionaryStmt:
		case T_AlterTableMoveAllStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_AlterTableStmt:
		case T_AlterTypeStmt:
		case T_AlterUserMappingStmt:
		case T_CommentStmt:
		case T_CompositeTypeStmt:
		case T_CreateAmStmt:
		case T_CreateCastStmt:
		case T_CreateConversionStmt:
		case T_CreateDomainStmt:
		case T_CreateEnumStmt:
		case T_CreateEventTrigStmt:
		case T_CreateExtensionStmt:
		case T_CreateFdwStmt:
		case T_CreateForeignServerStmt:
		case T_CreateForeignTableStmt:
		case T_CreateFunctionStmt:
		case T_CreateOpClassStmt:
		case T_CreateOpFamilyStmt:
		case T_CreatePLangStmt:
		case T_CreatePolicyStmt:
		case T_CreatePublicationStmt:
		case T_CreateRangeStmt:
		case T_CreateRoleStmt:
		case T_CreateSchemaStmt:
		case T_CreateSeqStmt:
		case T_CreateStatsStmt:
		case T_CreateStmt:
		case T_CreateSubscriptionStmt:
		case T_CreateTableAsStmt:
		case T_CreateTableSpaceStmt:
		case T_CreateTransformStmt:
		case T_CreateTrigStmt:
		case T_CreateUserMappingStmt:
		case T_CreatedbStmt:
		case T_DefineStmt:
		case T_DropOwnedStmt:
		case T_DropRoleStmt:
		case T_DropStmt:
		case T_DropSubscriptionStmt:
		case T_DropTableSpaceStmt:
		case T_DropUserMappingStmt:
		case T_DropdbStmt:
		case T_GrantRoleStmt:
		case T_GrantStmt:
		case T_ImportForeignSchemaStmt:
		case T_IndexStmt:
		case T_ReassignOwnedStmt:
		case T_RefreshMatViewStmt:
		case T_RenameStmt:
		case T_RuleStmt:
		case T_SecLabelStmt:
		case T_TruncateStmt:
		case T_ViewStmt:
			return true;
		case T_CopyStmt:
			return ((CopyStmt *) stmt)->is_from &&
				((CopyStmt *) stmt)->relation != NULL &&
				!is_temp_rangevar(((CopyStmt *) stmt)->relation);
		default:
			return false;
	}
}

/* GLOBAL read_only for DML, as ExecCheckXactReadOnly(): temp tables stay writable */
static void
check_server_read_only_dml(PlannedStmt *stmt)
{
	if (!mys_server_read_only_applies())
		return;
	if (stmt->commandType == CMD_SELECT && !stmt->hasModifyingCTE && stmt->rowMarks == NIL)
		return;					/* SELECT ... FOR UPDATE is refused, as in MySQL */
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

/* Defaults are expanded by the rewriter, after post_parse_analyze_hook. */
static PlannedStmt *
mys_planner(Query *query, const char *queryString, int cursorOptions,
			ParamListInfo params)
{
	mys_assign_bool_as_int(query);
	if (prev_planner)
		return prev_planner(query, queryString, cursorOptions, params);
	return standard_planner(query, queryString, cursorOptions, params);
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
		if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
			check_server_read_only_dml(queryDesc->plannedstmt);
	}
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
	if (mysql && !(eflags & EXEC_FLAG_EXPLAIN_ONLY) &&
		uses_application_table(queryDesc->plannedstmt->rtable))
		mys_statement_touched_tables();
}

static void
process_utility_impl(PlannedStmt *pstmt, const char *queryString,
				bool readOnlyTree, ProcessUtilityContext context,
				ParamListInfo params, QueryEnvironment *queryEnv,
				DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	MysStmtInfo *info = mys_current_stmt_info();

	if (context == PROCESS_UTILITY_TOPLEVEL && IsA(parsetree, TransactionStmt) &&
		info != NULL && info->txn_control && mys_txn_skip_control(info))
	{
		if (qc)
			SetQueryCompletion(qc, CreateCommandTag(parsetree), 0);
		return;
	}

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
	{
		mys_sync_foreign_key_checks();
		/* mysqldump issues both DROP TABLE IF EXISTS and DROP VIEW IF EXISTS
		 * for view placeholders. MySQL treats the other kind as absent.
		 * Keep the FK DDL fence before relation locks to preserve lock order. */
		if (IsA(pstmt->utilityStmt, DropStmt))
		{
			DropStmt *drop = (DropStmt *) pstmt->utilityStmt;

			if (drop->missing_ok && (drop->removeType == OBJECT_TABLE ||
									drop->removeType == OBJECT_VIEW))
			{
				List *objects = NIL;

				foreach_node(List, name, drop->objects)
				{
					RangeVar *rv = makeRangeVarFromNameList(name);
					Oid relid = RangeVarGetRelid(rv, AccessExclusiveLock, true);
					char kind = OidIsValid(relid) ? get_rel_relkind(relid) : '\0';
					bool mismatch = drop->removeType == OBJECT_TABLE ?
						kind == RELKIND_VIEW :
						(kind == RELKIND_RELATION || kind == RELKIND_PARTITIONED_TABLE ||
						 kind == RELKIND_FOREIGN_TABLE);

					if (mismatch)
						mys_add_warning(1051, psprintf("Unknown table '%s.%s'",
							get_namespace_name(get_rel_namespace(relid)), rv->relname));
					else
						objects = lappend(objects, name);
				}
				if (objects == NIL)
				{
					if (qc)
						SetQueryCompletion(qc, CreateCommandTag(pstmt->utilityStmt), 0);
					return;
				}
				pstmt = copyObject(pstmt);
				((DropStmt *) pstmt->utilityStmt)->objects = copyObject(objects);
				parsetree = pstmt->utilityStmt;
				readOnlyTree = false;
			}
		}
	}

	/* keys that ALTER TABLE / CREATE INDEX add get their names now */
	if (IsA(parsetree, IndexStmt) || IsA(parsetree, AlterTableStmt))
	{
		Node	   *named = mys_name_keys(parsetree);

		if (named)
		{
			pstmt = copyObject(pstmt);
			pstmt->utilityStmt = parsetree = named;
		}
	}

	/* Resolve ADD/MODIFY defaults at execution, including multi-statement DDL. */
	if (mys_is_mysql_session() && halo_mysql_parser == MYS_PARSER_MYSQL &&
		IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *alter = copyObject((AlterTableStmt *) parsetree);
		Oid relid = RangeVarGetRelid(alter->relation, AccessExclusiveLock, alter->missing_ok);
		List	   *defs = NIL;

		if (OidIsValid(relid))
			foreach_node(AlterTableCmd, cmd, alter->cmds)
			{
				if (cmd->def && IsA(cmd->def, ColumnDef) &&
					(cmd->subtype == AT_AddColumn || (int) cmd->subtype == AT_ModifyColumn ||
					 (int) cmd->subtype == AT_ChangeColumn))
				{
					mys_rectify_column_collation((ColumnDef *) cmd->def,
												mys_table_default_collation(relid), false);
					defs = lappend(defs, cmd->def);
				}
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
		if (OidIsValid(relid))
		{
			List	   *dropped = NIL;
			List	   *replaced = NIL;

			foreach_node(AlterTableCmd, cmd, alter->cmds)
				if (cmd->subtype == AT_DropColumn || cmd->subtype == AT_SetExpression ||
					cmd->subtype == AT_DropExpression || (int) cmd->subtype == AT_ModifyColumn ||
					(int) cmd->subtype == AT_ChangeColumn)
					replaced = lappend(replaced, cmd->name);
			gen_policy_table(relid, replaced);

			/* generated columns other generated columns read stay */
			foreach_node(AlterTableCmd, cmd, alter->cmds)
				if (cmd->subtype == AT_DropColumn)
					dropped = lappend(dropped, cmd->name);
			foreach_node(AlterTableCmd, cmd, alter->cmds)
				if (cmd->subtype == AT_DropColumn ||
					((int) cmd->subtype == AT_ChangeColumn && cmd->def &&
					 strcmp(cmd->name, ((ColumnDef *) cmd->def)->colname) != 0))
					check_generated_readers(relid, cmd->name, cmd->subtype == AT_DropColumn,
											dropped);
			mysql_generated_columns(defs, relid, get_rel_namespace(relid),
									pstmt, queryString, params, queryEnv);
		}
		pstmt = copyObject(pstmt);
		pstmt->utilityStmt = parsetree = (Node *) alter;
	}

	if (mys_is_mysql_session() && IsA(parsetree, RenameStmt) &&
		((RenameStmt *) parsetree)->renameType == OBJECT_COLUMN)
	{
		Oid			relid = RangeVarGetRelid(((RenameStmt *) parsetree)->relation,
											 AccessExclusiveLock, true);

		if (OidIsValid(relid))
			check_generated_readers(relid, ((RenameStmt *) parsetree)->subname, false, NIL);
	}

	/* the same for the generated columns of CREATE TABLE */
	if (mys_is_mysql_session() && halo_mysql_parser == MYS_PARSER_MYSQL &&
		IsA(parsetree, CreateStmt))
	{
		CreateStmt *create = (CreateStmt *) parsetree;
		List	   *defs = NIL;

		foreach_ptr(Node, elt, create->tableElts)
			if (IsA(elt, TableLikeClause) &&
				(((TableLikeClause *) elt)->options & CREATE_TABLE_LIKE_GENERATED))
				gen_policy_table(RangeVarGetRelid(((TableLikeClause *) elt)->relation,
												AccessShareLock, false), NIL);
		foreach_ptr(Node, elt, create->tableElts)
			if (IsA(elt, ColumnDef) && generated_constraint((ColumnDef *) elt))
				defs = lappend(defs, elt);
		if (defs != NIL &&
			!(create->if_not_exists &&
			  OidIsValid(RangeVarGetRelid(create->relation, NoLock, true))))
		{
			create = copyObject(create);
			defs = NIL;
			foreach_ptr(Node, elt, create->tableElts)
				if (IsA(elt, ColumnDef))
					defs = lappend(defs, elt);
			mysql_generated_columns(defs, InvalidOid,
									RangeVarGetCreationNamespace(create->relation),
									pstmt, queryString, params, queryEnv);
			pstmt = copyObject(pstmt);
			pstmt->utilityStmt = parsetree = (Node *) create;
		}
	}

	if (IsA(parsetree, CreateExtensionStmt) &&
		strcmp(((CreateExtensionStmt *) parsetree)->extname, "aux_mysql") == 0)
		halo_mysql_lock_setup();

	/* syntax error of a statement inside a transaction (mys_txn.c) */
	if (mys_txn_is_deferred_error(parsetree))
		mys_txn_raise_deferred_error();

	/* SQL PREPARE / EXECUTE / DEALLOCATE PREPARE (mys_stmt.c) */
	if (mys_sql_ps_utility(parsetree, queryString))
		return;

	if (is_table_setup(parsetree))
	{
		mysql_table_setup((DoStmt *) parsetree, pstmt, queryString, params, queryEnv);
		return;
	}

	/* only the MySQL grammar names the table and the index of an index rename */
	if (IsA(parsetree, RenameStmt) &&
		((RenameStmt *) parsetree)->renameType == OBJECT_INDEX &&
		((RenameStmt *) parsetree)->subname != NULL)
	{
		mysql_rename_index((RenameStmt *) parsetree, pstmt, queryString, params, queryEnv);
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
		level = mys_transaction_start(explicit_read_only, &snapshot, &read_only,
									  info == NULL || !info->txn_control);
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

/* Pending declarations are maintained inside the read-only utility guard. */
static void
process_utility(PlannedStmt *pstmt, const char *queryString,
				bool readOnlyTree, ProcessUtilityContext context,
				ParamListInfo params, QueryEnvironment *queryEnv,
				DestReceiver *dest, QueryCompletion *qc)
{
	MysFKUtility *fk = mys_fk_before_utility(&pstmt);

	process_utility_impl(pstmt, queryString, fk ? false : readOnlyTree,
						 context, params, queryEnv, dest, qc);
	mys_fk_after_utility(fk);
}

/* GLOBAL read_only is a statement restriction, never a GUC override. */
static void
mys_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
				   bool readOnlyTree, ProcessUtilityContext context,
				   ParamListInfo params, QueryEnvironment *queryEnv,
				   DestReceiver *dest, QueryCompletion *qc)
{
	List	   *generators = NIL;

	if (mys_is_mysql_session())
	{
		if (server_read_only_utility(pstmt->utilityStmt, context))
			ereport(ERROR,
					(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
					 errmsg("The MySQL server is running with the --read-only option so it cannot execute this statement")));

		generators = statement_generators(pstmt->utilityStmt);
	}
	process_utility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	if (mys_is_mysql_session() && IsA(pstmt->utilityStmt, CreateStmt) &&
		((CreateStmt *) pstmt->utilityStmt)->relation->relpersistence == RELPERSISTENCE_TEMP)
		mys_statement_touched_tables();
	drop_unused_generators(generators);
}

void
mysql_utility_init(void)
{
	mys_fk_init();
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = mys_ProcessUtility;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = mys_ExecutorStart;
	prev_planner = planner_hook;
	planner_hook = mys_planner;
}
