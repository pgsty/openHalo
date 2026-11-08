/*-------------------------------------------------------------------------
 * Foreign keys whose parent is temporarily absent (FOREIGN_KEY_CHECKS=0).
 *
 * The declaration belongs to the child: a versioned trigger argument carries
 * names, column order and actions through pg_dump/restore. A row
 * trigger guards new keys while the parent is absent. Once the parent exists,
 * ordinary PG18 NOT VALID foreign keys take over; old unchecked rows stay
 * unchecked, exactly as when MySQL FOREIGN_KEY_CHECKS is turned back on.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc_d.h"
#include "catalog/indexing.h"
#include "catalog/pg_trigger.h"

#include "commands/trigger.h"
#include "fmgr.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parser.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/json.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "halo_mysql.h"
#include "mys_parsenodes.h"
#include "mys_fk.h"

#define FK_GUARD "pending_fk_guard"

struct MysFKUtility
{
	Node *stmt;
	Oid target;
	RangeVar *created;
	List *added;
	char *old_schema;
	char *old_table;
	char *old_column;
	char *new_column;
	char *new_schema;
	bool rebind;
};

static int fk_internal_depth;

static void
fk_error(const char *detail)
{
	ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("invalid pending foreign key metadata"), errdetail("%s", detail)));
}

static JsonbValue *
field(JsonbContainer *obj, const char *name, enum jbvType type)
{
	JsonbValue *v = getKeyJsonValueFromContainer(obj, name, strlen(name), NULL);

	if (v == NULL || v->type != type)
		fk_error(name);
	return v;
}

static char *
string_field(JsonbContainer *obj, const char *name)
{
	JsonbValue *v = field(obj, name, jbvString);

	if (v->val.string.len == 0 || v->val.string.len >= NAMEDATALEN ||
		memchr(v->val.string.val, '\0', v->val.string.len))
		fk_error(name);
	return pnstrdup(v->val.string.val, v->val.string.len);
}

static List *
column_fields(JsonbContainer *obj, const char *name)
{
	JsonbValue *v = field(obj, name, jbvBinary);
	JsonbIterator *it;
	JsonbValue value;
	JsonbIteratorToken token;
	List *result = NIL;

	if (!JsonContainerIsArray(v->val.binary.data))
		fk_error(name);
	it = JsonbIteratorInit(v->val.binary.data);
	while ((token = JsonbIteratorNext(&it, &value, true)) != WJB_DONE)
		if (token == WJB_ELEM)
		{
			if (value.type != jbvString || value.val.string.len == 0 ||
				value.val.string.len >= NAMEDATALEN)
				fk_error(name);
			result = lappend(result, makeString(pnstrdup(value.val.string.val, value.val.string.len)));
		}
	return result;
}

static List *
parse_label(const char *label)
{
	Jsonb *json;
	JsonbValue *keys;
	JsonbIterator *it;
	JsonbIteratorToken token;
	JsonbValue value;
	List *result = NIL;

	if (label == NULL)
		return NIL;
	json = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(label)));
	if (!JB_ROOT_IS_OBJECT(json) || strcmp(string_field(&json->root, "version"), "1") != 0)
		fk_error("version");
	keys = field(&json->root, "keys", jbvBinary);
	if (!JsonContainerIsArray(keys->val.binary.data))
		fk_error("keys");
	it = JsonbIteratorInit(keys->val.binary.data);
	while ((token = JsonbIteratorNext(&it, &value, true)) != WJB_DONE)
	{
		Constraint *c;
		JsonbContainer *obj;
		char *action;

		if (token != WJB_ELEM)
			continue;
		if (value.type != jbvBinary || !JsonContainerIsObject(value.val.binary.data))
			fk_error("key");
		obj = value.val.binary.data;
		c = makeNode(Constraint);
		c->contype = CONSTR_FOREIGN;
		c->conname = string_field(obj, "name");
		foreach_node(Constraint, old, result)
			if (strcmp(old->conname, c->conname) == 0)
				fk_error("duplicate constraint name");
		c->pktable = makeRangeVar(string_field(obj, "schema"), string_field(obj, "table"), -1);
		c->fk_attrs = column_fields(obj, "columns");
		c->pk_attrs = column_fields(obj, "refcolumns");
		if (getKeyJsonValueFromContainer(obj, "delete_columns", 14, NULL))
			c->fk_del_set_cols = column_fields(obj, "delete_columns");
		if (getKeyJsonValueFromContainer(obj, "period", 6, NULL))
			c->fk_with_period = field(obj, "period", jbvBool)->val.boolean;
		if (c->fk_attrs == NIL || list_length(c->fk_attrs) > INDEX_MAX_KEYS ||
			(c->pk_attrs && list_length(c->fk_attrs) != list_length(c->pk_attrs)))
			fk_error("column count");
		action = string_field(obj, "update");
		if (strlen(action) != 1 || !strchr("arcdn", *action)) fk_error("update");
		c->fk_upd_action = *action;
		action = string_field(obj, "delete");
		if (strlen(action) != 1 || !strchr("arcdn", *action)) fk_error("delete");
		c->fk_del_action = *action;
		action = string_field(obj, "match");
		if (strlen(action) != 1 || !strchr("sfp", *action)) fk_error("match");
		c->fk_matchtype = *action;
		c->deferrable = field(obj, "deferrable", jbvBool)->val.boolean;
		c->initdeferred = field(obj, "deferred", jbvBool)->val.boolean;
		c->is_enforced = true;
		c->skip_validation = true;
		c->initially_valid = false;
		c->location = -1;
		result = lappend(result, c);
	}
	return result;
}

static Oid
guard_function(bool missing_ok)
{
	if (missing_ok && !OidIsValid(get_namespace_oid("mysql", true))) return InvalidOid;
	return LookupFuncName(list_make2(makeString("mysql"), makeString(FK_GUARD)), 0, NULL, missing_ok);
}

static char *
guard_metadata(Oid relid, Oid *trigger)
{
	Oid func = guard_function(true);
	Relation rel;
	char *data = NULL;

	*trigger = InvalidOid;
	if (!OidIsValid(func)) return NULL;
	rel = relation_open(relid, AccessShareLock);
	for (int i = 0; rel->trigdesc && i < rel->trigdesc->numtriggers; i++)
	{
		Trigger *t = &rel->trigdesc->triggers[i];

		if (t->tgfoid != func) continue;
		if (OidIsValid(*trigger) || t->tgnargs != 1) fk_error("expected one metadata-bearing guard");
		*trigger = t->tgoid;
		data = pstrdup(t->tgargs[0]);
	}
	relation_close(rel, AccessShareLock);
	return data;
}

static List *
pending_keys(Oid relid)
{
	Oid trigger;

	return parse_label(guard_metadata(relid, &trigger));
}

static void
store_metadata(Oid relid, const char *data)
{
	Oid trigger;

	(void) guard_metadata(relid, &trigger);
	if (!data)
	{
		if (OidIsValid(trigger))
		{
			ObjectAddress object;

			ObjectAddressSet(object, TriggerRelationId, trigger);
			performDeletion(&object, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);
			CommandCounterIncrement();
		}
		return;
	}
	if (OidIsValid(trigger))
	{
		Relation catalog = table_open(TriggerRelationId, RowExclusiveLock);
		ScanKeyData key;
		SysScanDesc scan;
		HeapTuple old, tuple;
		Datum values[Natts_pg_trigger] = {0};
		bool nulls[Natts_pg_trigger] = {0}, replace[Natts_pg_trigger] = {0};
		int len = strlen(data) + 1;
		bytea *args = palloc(VARHDRSZ + len);

		SET_VARSIZE(args, VARHDRSZ + len);
		memcpy(VARDATA(args), data, len);
		ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(trigger));
		scan = systable_beginscan(catalog, TriggerOidIndexId, true, NULL, 1, &key);
		old = systable_getnext(scan);
		if (!HeapTupleIsValid(old)) elog(ERROR, "pending FK trigger disappeared");
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(args);
		replace[Anum_pg_trigger_tgargs - 1] = true;
		tuple = heap_modify_tuple(old, RelationGetDescr(catalog), values, nulls, replace);
		CatalogTupleUpdate(catalog, &tuple->t_self, tuple);
		heap_freetuple(tuple);
		systable_endscan(scan);
		table_close(catalog, RowExclusiveLock);
		CacheInvalidateRelcacheByRelid(relid);
		CommandCounterIncrement();
	}
	else
	{
		CreateTrigStmt *s = makeNode(CreateTrigStmt);
		Oid user;
		int context;
		unsigned int suffix = 0;

		do { s->trigname = psprintf("aux_mysql_pending_fk_%u_%u", relid, suffix++); }
		while (OidIsValid(get_trigger_oid(relid, s->trigname, true)));
		s->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid), -1);
		s->funcname = list_make2(makeString("mysql"), makeString(FK_GUARD));
		s->args = list_make1(makeString(pstrdup(data)));
		s->row = true;
		s->timing = TRIGGER_TYPE_AFTER;
		s->events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE;
		GetUserIdAndSecContext(&user, &context);
		PG_TRY();
		{
			SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
				context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
			/* A normal trigger is dumped POST_DATA, so historical unchecked
			 * rows load before the declaration and its guard are restored. */
			CreateTrigger(s, "pending foreign key guard", relid, InvalidOid, InvalidOid,
				InvalidOid, guard_function(false), InvalidOid, NULL, false, false);
		}
		PG_FINALLY();
		{
			SetUserIdAndSecContext(user, context);
		}
		PG_END_TRY();
		CommandCounterIncrement();
	}
}

static void
json_columns(StringInfo buf, List *columns)
{
	bool first = true;

	appendStringInfoChar(buf, '[');
	foreach_node(String, col, columns)
	{
		if (!first) appendStringInfoChar(buf, ',');
		escape_json(buf, strVal(col));
		first = false;
	}
	appendStringInfoChar(buf, ']');
}

static void
save_keys(Oid relid, List *keys)
{
	StringInfoData buf;
	bool first = true;

	if (keys == NIL)
	{
		store_metadata(relid, NULL);
		return;
	}
	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"version\":\"1\",\"keys\":[");
	foreach_node(Constraint, c, keys)
	{
		if (!first) appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, "{\"name\":"); escape_json(&buf, c->conname);
		appendStringInfoString(&buf, ",\"schema\":"); escape_json(&buf, c->pktable->schemaname);
		appendStringInfoString(&buf, ",\"table\":"); escape_json(&buf, c->pktable->relname);
		appendStringInfoString(&buf, ",\"columns\":"); json_columns(&buf, c->fk_attrs);
		appendStringInfoString(&buf, ",\"refcolumns\":"); json_columns(&buf, c->pk_attrs);
		appendStringInfoString(&buf, ",\"delete_columns\":"); json_columns(&buf, c->fk_del_set_cols);
		appendStringInfo(&buf, ",\"period\":%s,\"update\":\"%c\",\"delete\":\"%c\",\"match\":\"%c\",\"deferrable\":%s,\"deferred\":%s}",
			c->fk_with_period ? "true" : "false",
			c->fk_upd_action, c->fk_del_action, c->fk_matchtype,
			c->deferrable ? "true" : "false", c->initdeferred ? "true" : "false");
		first = false;
	}
	appendStringInfoString(&buf, "]}");
	store_metadata(relid, buf.data);
}

static List *
pending_tables(bool first_only)
{
	Oid func = guard_function(true);
	Relation catalog, triggers = NULL;
	SysScanDesc scan;
	ScanKeyData keys[2];
	HeapTuple tup;
	List *result = NIL;

	if (!OidIsValid(func)) return NIL;
	/* Every normal trigger depends on its function. Use that reference index
	 * rather than walking all user and RI triggers for every relevant DDL. */
	catalog = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_depend_refclassid, BTEqualStrategyNumber,
		F_OIDEQ, ObjectIdGetDatum(ProcedureRelationId));
	ScanKeyInit(&keys[1], Anum_pg_depend_refobjid, BTEqualStrategyNumber,
		F_OIDEQ, ObjectIdGetDatum(func));
	scan = systable_beginscan(catalog, DependReferenceIndexId, true, NULL, 2, keys);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
		SysScanDesc trigger_scan;
		ScanKeyData key;
		HeapTuple trigger_tuple;

		if (dep->classid != TriggerRelationId || dep->objsubid != 0) continue;
		if (triggers == NULL) triggers = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&key, Anum_pg_trigger_oid, BTEqualStrategyNumber,
			F_OIDEQ, ObjectIdGetDatum(dep->objid));
		trigger_scan = systable_beginscan(triggers, TriggerOidIndexId, true, NULL, 1, &key);
		trigger_tuple = systable_getnext(trigger_scan);
		if (HeapTupleIsValid(trigger_tuple))
		{
			Form_pg_trigger row = (Form_pg_trigger) GETSTRUCT(trigger_tuple);

			if (row->tgfoid == func && row->tgparentid == InvalidOid)
				result = list_append_unique_oid(result, row->tgrelid);
		}
		systable_endscan(trigger_scan);
		if (first_only && result != NIL) break;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (triggers != NULL) table_close(triggers, AccessShareLock);
	return result;
}

void
mys_fk_init(void)
{
	/* No provider registration: pg_dump persists the normal trigger and its
	 * versioned argument, in dependency order, after copying table data. */
}

/* Only internally constructed constraint statements run privileged.  They
 * contain catalog-validated identifiers/actions, never arbitrary SQL.  Dropping
 * a parent or recreating it must also maintain FKs on children owned by another
 * role, just as native dependency and RI maintenance does. */
static void
fk_utility(Node *stmt)
{
	PlannedStmt *p = makeNode(PlannedStmt);
	Oid user;
	int context;

	p->commandType = CMD_UTILITY;
	p->utilityStmt = stmt;
	GetUserIdAndSecContext(&user, &context);
	fk_internal_depth++;
	PG_TRY();
	{
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
			context | SECURITY_LOCAL_USERID_CHANGE | SECURITY_RESTRICTED_OPERATION);
		ProcessUtility(p, "pending foreign key maintenance", false, PROCESS_UTILITY_QUERY,
					   NULL, NULL, None_Receiver, NULL);
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(user, context);
		fk_internal_depth--;
	}
	PG_END_TRY();
	CommandCounterIncrement();
}

static void
alter_constraint(Oid relid, Constraint *c, bool drop)
{
	AlterTableStmt *s = makeNode(AlterTableStmt);
	AlterTableCmd *cmd = makeNode(AlterTableCmd);

	s->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid), -1);
	s->objtype = OBJECT_TABLE;
	cmd->subtype = drop ? AT_DropConstraint : AT_AddConstraint;
	cmd->name = c->conname;
	cmd->def = drop ? NULL : (Node *) copyObject(c);
	cmd->behavior = DROP_RESTRICT;
	s->cmds = list_make1(cmd);
	fk_utility((Node *) s);
}

static bool
same_names(List *columns, HeapTuple old, HeapTuple new, TupleDesc desc, Oid relid)
{
	foreach_node(String, name, columns)
	{
		AttrNumber num = get_attnum(relid, strVal(name));
		bool a_null, b_null;
		Datum a, b;
		TypeCacheEntry *type;
		Form_pg_attribute att;

		if (num <= 0) fk_error("child column is missing");
		a = heap_getattr(old, num, desc, &a_null);
		b = heap_getattr(new, num, desc, &b_null);
		if (a_null != b_null) return false;
		if (a_null) continue;
		att = TupleDescAttr(desc, num - 1);
		type = lookup_type_cache(att->atttypid, TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(type->eq_opr_finfo.fn_oid) ||
			!DatumGetBool(FunctionCall2Coll(&type->eq_opr_finfo, att->attcollation, a, b)))
			return false;
	}
	return true;
}

static void
check_pending_tuple(Relation rel, HeapTuple new, HeapTuple old)
{
	Oid relid = RelationGetRelid(rel);
	TupleDesc desc = RelationGetDescr(rel);

	if (mys_is_mysql_session() && !mys_foreign_key_checks()) return;
	foreach_node(Constraint, c, pending_keys(relid))
	{
		int nulls = 0;

		foreach_node(String, name, c->fk_attrs)
		{
			AttrNumber num = get_attnum(relid, strVal(name));

			if (num <= 0) fk_error("child column is missing");
			nulls += heap_attisnull(new, num, desc) ? 1 : 0;
		}
		if ((nulls && c->fk_matchtype == FKCONSTR_MATCH_SIMPLE) ||
			nulls == list_length(c->fk_attrs) || (old != NULL &&
			same_names(c->fk_attrs, old, new, desc, relid)))
			continue;
		ereport(ERROR, (errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
						errmsg("Cannot add or update a child row: foreign key constraint '%s' has no referenced table '%s.%s'",
							   c->conname, c->pktable->schemaname, c->pktable->relname),
						errtableconstraint(rel, c->conname)));
	}
}

/* Parent integration: invoke inside RI_FKey_check_insert's subtransaction,
 * before its built-in RI trigger loop. Its existing 23503 handler turns this
 * into warning 1452 and skips the row for INSERT IGNORE. */
void
mys_pending_fk_check_insert(Relation rel, TupleTableSlot *slot)
{
	bool free_tuple;
	HeapTuple tuple = ExecFetchSlotHeapTuple(slot, false, &free_tuple);

	check_pending_tuple(rel, tuple, NULL);
	if (free_tuple) heap_freetuple(tuple);
}

PG_FUNCTION_INFO_V1(mys_pending_fk_guard);
Datum
mys_pending_fk_guard(PG_FUNCTION_ARGS)
{
	TriggerData *trig;
	HeapTuple new;

	if (!CALLED_AS_TRIGGER(fcinfo)) elog(ERROR, "pending_fk_guard is a trigger");
	trig = (TriggerData *) fcinfo->context;
	new = TRIGGER_FIRED_BY_UPDATE(trig->tg_event) ? trig->tg_newtuple : trig->tg_trigtuple;
	/* AFTER lets ON DUPLICATE KEY UPDATE take the UPDATE path before checking
	 * FK keys. Unchanged orphan keys are allowed, just as native/MySQL RI. */
	check_pending_tuple(trig->tg_relation, new,
		TRIGGER_FIRED_BY_UPDATE(trig->tg_event) ? trig->tg_trigtuple : NULL);
	PG_RETURN_POINTER(new);
}

static bool
named_key(List *keys, const char *name)
{
	foreach_node(Constraint, c, keys)
		if (strcmp(c->conname, name) == 0) return true;
	return false;
}

static Oid
parent_oid(Constraint *c)
{
	Oid nsp = get_namespace_oid(c->pktable->schemaname, true);

	return OidIsValid(nsp) ? get_relname_relid(c->pktable->relname, nsp) : InvalidOid;
}

static void
require_owner(Oid relid)
{
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relid));
}

/* Remove only declarations whose parent is absent. All other constraints go
 * through normal PG analysis and permissions, including self-referencing FKs. */
static List *
extract_missing(List *nodes, const char *column, RangeVar *child, Oid relid,
				List *existing, MysFKUtility *state)
{
	List *result = NIL;

	foreach_ptr(Node, node, nodes)
	{
		Constraint *c;

		if (!IsA(node, Constraint) || ((Constraint *) node)->contype != CONSTR_FOREIGN)
		{
			result = lappend(result, node);
			continue;
		}
		c = (Constraint *) node;
		if (c->pktable->schemaname == NULL)
			c->pktable->schemaname = pstrdup(child->schemaname);
		if (OidIsValid(parent_oid(c)) ||
			(strcmp(child->relname, c->pktable->relname) == 0 &&
			 strcmp(child->schemaname, c->pktable->schemaname) == 0))
		{
			result = lappend(result, c);
			continue;
		}
		if (!c->conname)
		{
			List *names = NIL;

			foreach_node(Constraint, k, existing) names = lappend(names, makeString(k->conname));
			foreach_node(Constraint, k, state->added) names = lappend(names, makeString(k->conname));
			c->conname = ChooseConstraintName(child->relname, NULL, "ibfk",
				get_namespace_oid(child->schemaname, false), names);
		}
		if (named_key(existing, c->conname) || named_key(state->added, c->conname) ||
			(OidIsValid(relid) && OidIsValid(get_relation_constraint_oid(relid, c->conname, true))))
			ereport(ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("Duplicate foreign key constraint name '%s'", c->conname)));
		if (c->fk_attrs == NIL && column)
			c->fk_attrs = list_make1(makeString(pstrdup(column)));
		if (!c->fk_attrs || (c->pk_attrs && list_length(c->pk_attrs) != list_length(c->fk_attrs)))
			ereport(ERROR, (errcode(ERRCODE_INVALID_FOREIGN_KEY), errmsg("foreign key column counts do not match")));
		c->is_enforced = true;
		c->skip_validation = true;
		c->initially_valid = false;
		state->added = lappend(state->added, c);
	}
	return result;
}

static void
prepare_create(CreateStmt *s, MysFKUtility *state)
{
	RangeVar *child = copyObject(s->relation);
	Oid nsp = RangeVarGetCreationNamespace(child);

	child->schemaname = get_namespace_name(nsp);
	if (s->if_not_exists && OidIsValid(get_relname_relid(child->relname, nsp)))
		return;
	state->created = child;
	s->tableElts = extract_missing(s->tableElts, NULL, child, InvalidOid, NIL, state);
	foreach_ptr(Node, node, s->tableElts)
		if (IsA(node, ColumnDef))
		{
			ColumnDef *col = (ColumnDef *) node;

			col->constraints = extract_missing(col->constraints, col->colname, child, InvalidOid, NIL, state);
		}
}

static void
prepare_alter(AlterTableStmt *s, MysFKUtility *state, bool checks_off)
{
	Oid relid = RangeVarGetRelid(s->relation, AccessExclusiveLock, s->missing_ok);
	List *keys, *cmds = NIL;
	RangeVar *child;
	bool removed = false;

	if (!OidIsValid(relid)) return;
	state->target = relid;
	keys = pending_keys(relid);
	child = makeRangeVar(get_namespace_name(get_rel_namespace(relid)), get_rel_name(relid), -1);
	/* DROP FK and DROP its column in the same ALTER are legal. */
	foreach_node(AlterTableCmd, cmd, s->cmds)
	{
		if ((cmd->subtype == AT_DropConstraint || (int) cmd->subtype == AT_DropForeignKey) &&
			cmd->name && named_key(keys, cmd->name))
		{
			List *kept = NIL;

			require_owner(relid);
			foreach_node(Constraint, c, keys)
				if (strcmp(c->conname, cmd->name) != 0) kept = lappend(kept, c);
			keys = kept;
			removed = true;
		}
		else cmds = lappend(cmds, cmd);
	}
	s->cmds = cmds;
	cmds = NIL;
	foreach_node(AlterTableCmd, cmd, s->cmds)
	{
		if (cmd->subtype == AT_DropColumn)
			foreach_node(Constraint, c, keys)
				foreach_node(String, name, c->fk_attrs)
					if (strcmp(strVal(name), cmd->name) == 0)
						ereport(ERROR, (errcode(MAKE_SQLSTATE('M','1','5','5','3')),
							errmsg("Cannot drop column '%s': needed in foreign key '%s'", cmd->name, c->conname)));
		if (checks_off && cmd->subtype == AT_AddConstraint && cmd->def &&
			IsA(cmd->def, Constraint) && ((Constraint *) cmd->def)->contype == CONSTR_FOREIGN)
		{
			if (extract_missing(list_make1(cmd->def), NULL, child, relid, keys, state) == NIL)
			{
				require_owner(relid);
				continue;
			}
		}
		if (checks_off && cmd->subtype == AT_AddColumn && IsA(cmd->def, ColumnDef))
		{
			ColumnDef *col = (ColumnDef *) cmd->def;

			col->constraints = extract_missing(col->constraints, col->colname, child, relid, keys, state);
		}
		cmds = lappend(cmds, cmd);
	}
	s->cmds = cmds;
	if (removed)
	{
		save_keys(relid, keys);
	}
}

static List *
drop_relations(DropStmt *s)
{
	List *result = NIL;

	foreach_ptr(Node, name, s->objects)
	{
		if (s->removeType == OBJECT_TABLE)
		{
			Oid relid = RangeVarGetRelid(makeRangeVarFromNameList((List *) name), AccessExclusiveLock, s->missing_ok);

			if (OidIsValid(relid))
			{
				require_owner(relid);
				result = lappend_oid(result, relid);
			}
		}
		else if (s->removeType == OBJECT_SCHEMA)
		{
			Oid nsp = get_namespace_oid(strVal(name), s->missing_ok);
			Relation catalog;
			SysScanDesc scan;
			HeapTuple tup;

			if (!OidIsValid(nsp)) continue;
			if (!object_ownercheck(NamespaceRelationId, nsp, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SCHEMA, get_namespace_name(nsp));
			catalog = table_open(RelationRelationId, AccessShareLock);
			scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
			while (HeapTupleIsValid(tup = systable_getnext(scan)))
			{
				Form_pg_class row = (Form_pg_class) GETSTRUCT(tup);

				if (row->relnamespace == nsp && (row->relkind == RELKIND_RELATION || row->relkind == RELKIND_PARTITIONED_TABLE))
					result = lappend_oid(result, row->oid);
			}
			systable_endscan(scan);
			table_close(catalog, AccessShareLock);
		}
	}
	return result;
}

static void
detach_incoming(List *dropping)
{
	Relation catalog;
	SysScanDesc scan;
	HeapTuple tup;
	List *constraints = NIL;

	if (dropping == NIL) return;
	catalog = table_open(ConstraintRelationId, AccessShareLock);
	scan = systable_beginscan(catalog, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint c = (Form_pg_constraint) GETSTRUCT(tup);

		if (c->contype == CONSTRAINT_FOREIGN && c->conparentid == InvalidOid &&
			list_member_oid(dropping, c->confrelid) && !list_member_oid(dropping, c->conrelid))
			constraints = lappend_oid(constraints, c->oid);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	foreach_oid(conoid, constraints)
	{
		HeapTuple ct = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
		Form_pg_constraint row;
		Oid child, parent;
		char *name, *definition, *sql;
		AlterTableStmt *s;
		Constraint *c;
		List *keys;

		if (!HeapTupleIsValid(ct)) continue;
		row = (Form_pg_constraint) GETSTRUCT(ct);
		child = row->conrelid;
		parent = row->confrelid;
		name = pstrdup(NameStr(row->conname));
		ReleaseSysCache(ct);
		LockRelationOid(child, AccessExclusiveLock);
		definition = TextDatumGetCString(DirectFunctionCall2(pg_get_constraintdef_ext,
			ObjectIdGetDatum(conoid), BoolGetDatum(false)));
		sql = psprintf("ALTER TABLE dummy ADD CONSTRAINT %s %s", quote_identifier(name), definition);
		s = castNode(AlterTableStmt, linitial_node(RawStmt, raw_parser(sql, RAW_PARSE_DEFAULT))->stmt);
		c = castNode(Constraint, linitial_node(AlterTableCmd, s->cmds)->def);
		c->pktable = makeRangeVar(get_namespace_name(get_rel_namespace(parent)), get_rel_name(parent), -1);
		c->is_enforced = true;
		c->skip_validation = true;
		c->initially_valid = false;
		keys = pending_keys(child);
		if (named_key(keys, name)) fk_error("duplicate bound and pending foreign key");
		keys = lappend(keys, c);
		save_keys(child, keys);
		alter_constraint(child, c, true);
	}
}

MysFKUtility *
mys_fk_before_utility(PlannedStmt **pstmt)
{
	Node *node = (*pstmt)->utilityStmt;
	MysFKUtility *state;
	bool off, restoring_guard = false;
	Oid func;
	LOCKTAG tag;

	if (fk_internal_depth || !(IsA(node, CreateStmt) || IsA(node, AlterTableStmt) ||
		IsA(node, RenameStmt) || IsA(node, AlterObjectSchemaStmt) ||
		(IsA(node, DropStmt) && (((DropStmt *) node)->removeType == OBJECT_TABLE || ((DropStmt *) node)->removeType == OBJECT_SCHEMA)) ||
		IsA(node, CreateTrigStmt)))
		return NULL;
	/* Databases without the SQL helpers need no pending-FK hooks or locks. */
	func = guard_function(true);
	if (!OidIsValid(func)) return NULL;
	if (IsA(node, CreateTrigStmt))
	{
		restoring_guard = LookupFuncName(((CreateTrigStmt *) node)->funcname, 0, NULL, true) == func;
		if (!restoring_guard) return NULL;
	}
	if (IsA(node, RenameStmt) && ((RenameStmt *) node)->renameType != OBJECT_TABLE &&
		((RenameStmt *) node)->renameType != OBJECT_COLUMN && ((RenameStmt *) node)->renameType != OBJECT_SCHEMA)
		return NULL;
	if (IsA(node, AlterObjectSchemaStmt) && ((AlterObjectSchemaStmt *) node)->objectType != OBJECT_TABLE)
		return NULL;
	off = mys_is_mysql_session() && !mys_foreign_key_checks();
	/* Ordinary DDL holds a shared fence until commit: an FK-off declaration
	 * must not miss a parent name created by an uncommitted ordinary CREATE.
	 * Test emptiness only while fenced, and refresh catalogs after waiting.
	 * With no pending keys, unrelated DDL can run concurrently and bypass all
	 * AST copying, relation locking and after-utility maintenance here. */
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x4f48464b, 0x70656e64, 2);
	if (!off && !restoring_guard && !LockHeldByMe(&tag, ExclusiveLock, false))
	{
		List *pending;

		(void) LockAcquire(&tag, ShareLock, false, false);
		AcceptInvalidationMessages();
		pending = pending_tables(true);
		if (pending == NIL) return NULL;
		list_free(pending);
		/* Do not keep this probe's shared lock while waiting for exclusive:
		 * concurrent probes of existing pending keys must not deadlock on
		 * each other's lock conversion. Re-read state under exclusive below. */
		(void) LockRelease(&tag, ShareLock, false);
	}
	if (!LockHeldByMe(&tag, ExclusiveLock, false))
		(void) LockAcquire(&tag, ExclusiveLock, false, false);
	AcceptInvalidationMessages();
	*pstmt = copyObject(*pstmt);
	node = (*pstmt)->utilityStmt;
	state = palloc0(sizeof(MysFKUtility));
	state->stmt = node;
	state->rebind = !IsA(node, DropStmt);
	if (IsA(node, CreateStmt))
	{
		if (off) prepare_create((CreateStmt *) node, state);
		if (!state->created) state->created = copyObject(((CreateStmt *) node)->relation);
	}
	else if (IsA(node, AlterTableStmt))
		prepare_alter((AlterTableStmt *) node, state, off);
	else if (IsA(node, DropStmt) && off)
		detach_incoming(drop_relations((DropStmt *) node));
	else if (IsA(node, RenameStmt))
	{
		RenameStmt *s = (RenameStmt *) node;

		if (s->relation && (s->renameType == OBJECT_TABLE || s->renameType == OBJECT_COLUMN))
		{
			state->target = RangeVarGetRelid(s->relation, AccessExclusiveLock, s->missing_ok);
			if (s->renameType == OBJECT_COLUMN)
			{
				state->old_column = s->subname;
				state->new_column = s->newname;
			}
		}
		else if (s->renameType == OBJECT_SCHEMA)
		{
			state->old_schema = s->subname;
			state->new_schema = s->newname;
		}
	}
	else if (IsA(node, AlterObjectSchemaStmt) && ((AlterObjectSchemaStmt *) node)->objectType == OBJECT_TABLE)
		state->target = RangeVarGetRelid(((AlterObjectSchemaStmt *) node)->relation,
			AccessExclusiveLock, ((AlterObjectSchemaStmt *) node)->missing_ok);
	else if (IsA(node, CreateTrigStmt))
	{
		CreateTrigStmt *s = (CreateTrigStmt *) node;

		/* Restored pending declarations bind automatically after COPY, when
		 * their parent is present. Native CREATE TRIGGER checks child ACLs. */
		if (OidIsValid(guard_function(true)) &&
			LookupFuncName(s->funcname, 0, NULL, true) == guard_function(true))
		{
			List *keys;

			if (list_length(s->args) != 1 || !s->row || s->timing != TRIGGER_TYPE_AFTER ||
				s->events != (TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE) || s->whenClause || s->columns)
				fk_error("guard must check every inserted/updated row");
			keys = parse_label(strVal(linitial(s->args)));
			foreach_node(Constraint, c, keys)
			{
				Oid parent = parent_oid(c);

				if (OidIsValid(parent) && pg_class_aclcheck(parent, GetUserId(), ACL_REFERENCES) != ACLCHECK_OK)
					aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, get_rel_name(parent));
			}
			state->target = RangeVarGetRelid(s->relation, AccessExclusiveLock, false);
		}
		else state->rebind = false;
	}
	if (OidIsValid(state->target))
	{
		state->old_schema = get_namespace_name(get_rel_namespace(state->target));
		state->old_table = get_rel_name(state->target);
	}
	return state;
}

static bool
rename_column_list(List *columns, const char *old, const char *new)
{
	bool changed = false;

	foreach_node(String, name, columns)
		if (strcmp(strVal(name), old) == 0)
		{
			name->sval = pstrdup(new);
			changed = true;
		}
	return changed;
}

static void
rename_pending(MysFKUtility *state)
{
	if (!state->old_schema || (!state->new_schema && !OidIsValid(state->target))) return;
	foreach_oid(child, pending_tables(false))
	{
		List *keys = pending_keys(child);
		bool changed = false;

		foreach_node(Constraint, c, keys)
		{
			bool parent = strcmp(c->pktable->schemaname, state->old_schema) == 0 &&
				(!state->old_table || strcmp(c->pktable->relname, state->old_table) == 0);

			if (state->old_column)
			{
				if (child == state->target)
					changed |= rename_column_list(c->fk_attrs, state->old_column, state->new_column);
				if (parent)
					changed |= rename_column_list(c->pk_attrs, state->old_column, state->new_column);
			}
			else if (parent)
			{
				c->pktable->schemaname = state->new_schema ? pstrdup(state->new_schema) :
					get_namespace_name(get_rel_namespace(state->target));
				if (state->old_table) c->pktable->relname = get_rel_name(state->target);
				changed = true;
			}
		}
		if (changed)
		{
			LockRelationOid(child, AccessExclusiveLock);
			save_keys(child, keys);
		}
	}
}

void
mys_fk_after_utility(MysFKUtility *state)
{
	if (state == NULL || fk_internal_depth) return;
	CommandCounterIncrement();
	if (state->created)
		state->target = RangeVarGetRelid(state->created, AccessExclusiveLock, true);
	if (state->added && OidIsValid(state->target))
	{
		List *keys = pending_keys(state->target);

		/* Check child column existence even though native FK analysis was
		 * deferred. A typo must never produce a permanently unusable table. */
		foreach_node(Constraint, c, state->added)
		{
			if (OidIsValid(get_relation_constraint_oid(state->target, c->conname, true)))
				ereport(ERROR, (errcode(MAKE_SQLSTATE('M','1','8','2','6')),
					errmsg("Duplicate foreign key constraint name '%s'", c->conname)));
			foreach_node(String, col, c->fk_attrs)
				if (get_attnum(state->target, strVal(col)) <= 0)
					ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("column '%s' referenced by foreign key does not exist", strVal(col))));
		}
		keys = list_concat(keys, state->added);
		save_keys(state->target, keys);
	}
	if (!state->rebind) return;
	if (IsA(state->stmt, RenameStmt) || IsA(state->stmt, AlterObjectSchemaStmt))
		rename_pending(state);
	foreach_oid(child, pending_tables(false))
	{
		List *keys = pending_keys(child), *remaining = NIL;
		bool touched = false;

		foreach_node(Constraint, c, keys)
		{
			Oid parent = parent_oid(c);
			MemoryContext saved_context = CurrentMemoryContext;

			if (!OidIsValid(parent) || (parent != state->target && child != state->target && !state->new_schema))
			{
				remaining = lappend(remaining, c);
				continue;
			}
			LockRelationOid(child, AccessExclusiveLock);
			/* NOT VALID installs all new-write checks and referential actions,
			 * without rejecting rows written while checks were disabled. */
			PG_TRY();
			{
				alter_constraint(child, c, false);
			}
			PG_CATCH();
			{
				ErrorData *error;

				/* ERROR leaves us in ErrorContext. Copying there asserts in
				 * debug builds and would lose the copy on error-state reset. */
				MemoryContextSwitchTo(saved_context);
				error = CopyErrorData();
				FlushErrorState();

				if (error->sqlerrcode == ERRCODE_INVALID_FOREIGN_KEY ||
					error->sqlerrcode == ERRCODE_DATATYPE_MISMATCH ||
					error->sqlerrcode == ERRCODE_UNDEFINED_COLUMN ||
					error->sqlerrcode == ERRCODE_WRONG_OBJECT_TYPE)
					error->sqlerrcode = MAKE_SQLSTATE('M','1','2','1','5');
				ReThrowError(error);
			}
			PG_END_TRY();
			touched = true;
		}
		if (touched) save_keys(child, remaining);
	}
}
