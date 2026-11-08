/*-------------------------------------------------------------------------
 *
 * mys_semantics.c
 *	  MySQL value semantics that PostgreSQL does differently, implemented as
 *	  functions / operators / triggers in schema "mysql" (which precedes
 *	  pg_catalog on the search_path of MySQL sessions only).
 *
 *	- "/" on integers yields DECIMAL with div_precision_increment (4) more
 *	  digits than the dividend: 5/2 = 2.5000, 5.0/2 = 2.50000
 *	- division / modulo by zero yields NULL with warning 1365; an error only
 *	  in strict mode + ERROR_FOR_DIVISION_BY_ZERO for INSERT / UPDATE
 *	- ON UPDATE CURRENT_TIMESTAMP (row trigger)
 *	- AUTO_INCREMENT: an explicit value beyond the counter advances it
 *	  (row trigger)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/acl.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#include "halo_mysql.h"
#include "adapter/mysql/systemVar.h"

/* MySQL's div_precision_increment default */
#define DIV_PRECISION_INCREMENT 4
/* MySQL's maximum DECIMAL scale */
#define MYSQL_MAX_SCALE			30

extern uint64 mys_sqlMode;

/* ----------------------------------------------------------------
 * Division and modulo
 * ----------------------------------------------------------------
 */

/*
 * Division by zero: NULL plus warning 1365, but an error for INSERT/UPDATE
 * in strict mode with ERROR_FOR_DIVISION_BY_ZERO (MySQL 8 default modes).
 */
static void
division_by_zero(void)
{
	MysStmtInfo *info = mys_current_stmt_info();

	if (info && info->is_write &&
		(mys_sqlMode & (MYS_MODE_STRICT_TRANS_TABLES | MYS_MODE_STRICT_ALL_TABLES)) &&
		(mys_sqlMode & MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("Division by 0")));
	mys_add_warning(1365, "Division by 0");
}

static Datum
numeric_div_scale(Numeric a, Numeric b, int scale)
{
	Datum		q = DirectFunctionCall2(numeric_div, NumericGetDatum(a), NumericGetDatum(b));

	return DirectFunctionCall2(numeric_round, q, Int32GetDatum(Min(scale, MYSQL_MAX_SCALE)));
}

/* integer / integer -> DECIMAL(.., 4) */
PG_FUNCTION_INFO_V1(mys_div_int8);
Datum
mys_div_int8(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (b == 0)
	{
		division_by_zero();
		PG_RETURN_NULL();
	}
	PG_RETURN_DATUM(numeric_div_scale(int64_to_numeric(a), int64_to_numeric(b),
									  DIV_PRECISION_INCREMENT));
}

/* decimal / decimal -> scale of the dividend + 4 */
PG_FUNCTION_INFO_V1(mys_div_numeric);
Datum
mys_div_numeric(PG_FUNCTION_ARGS)
{
	Numeric		a = PG_GETARG_NUMERIC(0);
	Numeric		b = PG_GETARG_NUMERIC(1);
	Datum		scale;

	if (!numeric_is_nan(b) && !numeric_is_inf(b) &&
		DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(b),
										 NumericGetDatum(int64_to_numeric(0)))))
	{
		division_by_zero();
		PG_RETURN_NULL();
	}
	if (numeric_is_nan(a) || numeric_is_inf(a))
		PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, NumericGetDatum(a), NumericGetDatum(b)));
	scale = DirectFunctionCall1(numeric_scale, NumericGetDatum(a));
	PG_RETURN_DATUM(numeric_div_scale(a, b, DatumGetInt32(scale) + DIV_PRECISION_INCREMENT));
}

/* double / double */
PG_FUNCTION_INFO_V1(mys_div_float8);
Datum
mys_div_float8(PG_FUNCTION_ARGS)
{
	float8		a = PG_GETARG_FLOAT8(0);
	float8		b = PG_GETARG_FLOAT8(1);

	if (b == 0)
	{
		division_by_zero();
		PG_RETURN_NULL();
	}
	PG_RETURN_FLOAT8(a / b);
}

/* integer % integer, MOD(): NULL for 0 */
PG_FUNCTION_INFO_V1(mys_mod_int8);
Datum
mys_mod_int8(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (b == 0)
	{
		division_by_zero();
		PG_RETURN_NULL();
	}
	if (b == -1)
		PG_RETURN_INT64(0);		/* avoid INT64_MIN % -1 overflow */
	PG_RETURN_INT64(a % b);
}

PG_FUNCTION_INFO_V1(mys_mod_numeric);
Datum
mys_mod_numeric(PG_FUNCTION_ARGS)
{
	Numeric		b = PG_GETARG_NUMERIC(1);

	if (!numeric_is_nan(b) && !numeric_is_inf(b) &&
		DatumGetBool(DirectFunctionCall2(numeric_eq, NumericGetDatum(b),
										 NumericGetDatum(int64_to_numeric(0)))))
	{
		division_by_zero();
		PG_RETURN_NULL();
	}
	PG_RETURN_DATUM(DirectFunctionCall2(numeric_mod, PG_GETARG_DATUM(0), NumericGetDatum(b)));
}

/* ----------------------------------------------------------------
 * ON UPDATE CURRENT_TIMESTAMP
 *
 * BEFORE UPDATE row trigger; arguments are the attribute numbers of the
 * auto-updated columns (attnums survive column renames).  MySQL sets such a
 * column to the current timestamp when some other column changes, unless
 * the UPDATE assigns the column itself.
 * ----------------------------------------------------------------
 */
static bool
column_changed(HeapTuple oldt, HeapTuple newt, TupleDesc desc, int attnum)
{
	Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
	bool		oldnull,
				newnull;
	Datum		oldv = heap_getattr(oldt, attnum, desc, &oldnull);
	Datum		newv = heap_getattr(newt, attnum, desc, &newnull);

	if (oldnull || newnull)
		return oldnull != newnull;
	return !datumIsEqual(oldv, newv, att->attbyval, att->attlen);
}

static bool
name_in_list(List *names, const char *name)
{
	foreach_ptr(char, n, names)
	{
		if (pg_strcasecmp(n, name) == 0)
			return true;
	}
	return false;
}

/* the statement's start time, in the column's type and precision */
static bool
current_timestamp_for(Form_pg_attribute att, Datum *value)
{
	TimestampTz now = GetCurrentStatementStartTimestamp();
	Oid			typid = getBaseType(att->atttypid);
	int32		typmod = att->atttypmod;

	switch (typid)
	{
		case TIMESTAMPTZOID:
			*value = TimestampTzGetDatum(now);
			if (typmod >= 0)
				*value = DirectFunctionCall2(timestamptz_scale, *value, Int32GetDatum(typmod));
			return true;
		case TIMESTAMPOID:
			*value = DirectFunctionCall1(timestamptz_timestamp, TimestampTzGetDatum(now));
			if (typmod >= 0)
				*value = DirectFunctionCall2(timestamp_scale, *value, Int32GetDatum(typmod));
			return true;
		case DATEOID:
			*value = DirectFunctionCall1(timestamptz_date, TimestampTzGetDatum(now));
			return true;
		default:
			return false;
	}
}

PG_FUNCTION_INFO_V1(mys_on_update_trigger);
Datum
mys_on_update_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	TupleDesc	desc;
	HeapTuple	oldt,
				newt;
	Trigger    *tg;
	MysStmtInfo *info;
	bool		others_changed = false;
	int			natts;
	int		   *auto_cols;
	int		   *repl_cols;
	Datum	   *repl_vals;
	bool	   *repl_nulls;
	int			nrepl = 0;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_BY_UPDATE(td->tg_event) ||
		!TRIGGER_FIRED_BEFORE(td->tg_event) || !TRIGGER_FIRED_FOR_ROW(td->tg_event))
		elog(ERROR, "mysql.on_update_current_timestamp() must be a BEFORE UPDATE row trigger");

	/*
	 * MySQL's foreign key actions (ON DELETE SET NULL, ON UPDATE CASCADE) do
	 * not touch ON UPDATE CURRENT_TIMESTAMP columns; PostgreSQL runs them as
	 * UPDATEs from its RI triggers, which alone set SECURITY_NOFORCE_RLS.
	 */
	if (InNoForceRLSOperation())
		return PointerGetDatum(td->tg_newtuple);

	/*
	 * ENABLE ALWAYS lets the trigger fire for FOREIGN_KEY_CHECKS=0 (replica
	 * role) in MySQL sessions; logical replication apply and other replica
	 * sessions replay rows that already carry their timestamps.
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA &&
		!mys_is_mysql_session())
		return PointerGetDatum(td->tg_newtuple);

	desc = RelationGetDescr(td->tg_relation);
	oldt = td->tg_trigtuple;
	newt = td->tg_newtuple;
	tg = td->tg_trigger;
	natts = desc->natts;
	info = mys_current_stmt_info();

	auto_cols = palloc0(sizeof(int) * (natts + 1));
	for (int i = 0; i < tg->tgnargs; i++)
	{
		int			attnum = atoi(tg->tgargs[i]);

		if (attnum > 0 && attnum <= natts)
			auto_cols[attnum] = 1;
	}

	/* did any other column change? */
	for (int attnum = 1; attnum <= natts && !others_changed; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);

		if (att->attisdropped || att->attgenerated || auto_cols[attnum])
			continue;
		others_changed = column_changed(oldt, newt, desc, attnum);
	}
	if (!others_changed)
		return PointerGetDatum(newt);

	repl_cols = palloc(sizeof(int) * natts);
	repl_vals = palloc(sizeof(Datum) * natts);
	repl_nulls = palloc0(sizeof(bool) * natts);
	for (int attnum = 1; attnum <= natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum - 1);
		Datum		v;

		if (!auto_cols[attnum] || att->attisdropped)
			continue;
		/* assigned by the UPDATE itself (MySQL protocol: we know the SET list) */
		if (info ? name_in_list(info->update_cols, NameStr(att->attname))
			: column_changed(oldt, newt, desc, attnum))
			continue;
		if (!current_timestamp_for(att, &v))
			continue;
		repl_cols[nrepl] = attnum;
		repl_vals[nrepl] = v;
		nrepl++;
	}
	if (nrepl > 0)
		newt = heap_modify_tuple_by_cols(newt, desc, nrepl, repl_cols, repl_vals, repl_nulls);
	return PointerGetDatum(newt);
}

/* ----------------------------------------------------------------
 * AUTO_INCREMENT counter
 *
 * BEFORE INSERT OR UPDATE row trigger on tables with an AUTO_INCREMENT
 * (identity) column: a value beyond the sequence's last value moves the
 * sequence forward, so later generated values do not collide (MySQL 8 does
 * this for INSERT and UPDATE).
 * ----------------------------------------------------------------
 */
typedef struct AutoIncCache
{
	Oid			relid;
	AttrNumber	attnum;
	Oid			seqoid;
	Oid			typid;
} AutoIncCache;

/* Check the actual evaluated sequence OID, including computed arguments.
 * Keep a relation lock through the write so persistence cannot change after
 * this check.  The core functions still enforce sequence type, ACLs, parallel
 * safety and the transaction's own access mode. */
static void
check_sequence_write(Oid seqoid)
{
	Relation	seqrel;

	if (!mys_is_mysql_session() || !mys_server_read_only_applies())
		return;
	seqrel = relation_open(seqoid, RowExclusiveLock);
	if (!seqrel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("The MySQL server is running with the --read-only option so it cannot execute this statement")));
	relation_close(seqrel, NoLock);
}

/* The core calls this after locking a sequence, before touching its cache
 * or storage.  It also covers inlined SQL functions and computed arguments. */
void
mys_sequence_write(Relation sequence)
{
	if (mys_is_mysql_session() && mys_server_read_only_applies() &&
		!sequence->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("The MySQL server is running with the --read-only option so it cannot execute this statement")));
}

/* Atomic with nextval; uses the core sequence page lock, ACLs and WAL. */
static void
sequence_advance(Oid seqoid, int64 value, bool is_called)
{
	check_sequence_write(seqoid);
	SetSequenceValueAtLeast(seqoid, value, is_called);
}

/*
 * mysql.setval(regclass, bigint, boolean) (openHalo aux_mysql 1.5): setval()
 * that only moves the sequence forward, for AUTO_INCREMENT counters.
 * openHalo implemented it in its kernel (mys_setval3_oid); this runs as the
 * calling user, so setval()'s privilege checks apply.
 */
PG_FUNCTION_INFO_V1(mysSetval3Oid);
Datum
mysSetval3Oid(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		next = PG_GETARG_INT64(1);
	bool		iscalled = PG_GETARG_BOOL(2);

	sequence_advance(relid, next, iscalled);
	PG_RETURN_INT64(next);
}

PG_FUNCTION_INFO_V1(mys_autoinc_trigger);
Datum
mys_autoinc_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *td = (TriggerData *) fcinfo->context;
	Relation	rel;
	TupleDesc	desc;
	HeapTuple	tuple;
	AutoIncCache *cache;
	bool		isnull;
	Datum		d;
	int64		value;
	Oid			save_userid;
	int			save_sec_context;

	if (!CALLED_AS_TRIGGER(fcinfo) || !TRIGGER_FIRED_BEFORE(td->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(td->tg_event))
		elog(ERROR, "mysql.auto_increment_advance() must be a BEFORE row trigger");

	rel = td->tg_relation;
	desc = RelationGetDescr(rel);
	tuple = TRIGGER_FIRED_BY_UPDATE(td->tg_event) ? td->tg_newtuple : td->tg_trigtuple;

	/* identity column and its sequence, cached for the statement */
	cache = (AutoIncCache *) fcinfo->flinfo->fn_extra;
	if (cache == NULL || cache->relid != RelationGetRelid(rel))
	{
		cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(AutoIncCache));
		cache->relid = RelationGetRelid(rel);
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (!att->attisdropped && att->attidentity)
			{
				cache->attnum = att->attnum;
				cache->typid = att->atttypid;
				cache->seqoid = getIdentitySequence(rel, att->attnum, true);
				break;
			}
		}
		fcinfo->flinfo->fn_extra = cache;
	}
	if (!OidIsValid(cache->seqoid))
		return PointerGetDatum(tuple);

	d = heap_getattr(tuple, cache->attnum, desc, &isnull);
	if (isnull && !(mys_is_mysql_session() && TRIGGER_FIRED_BY_INSERT(td->tg_event)))
		return PointerGetDatum(tuple);
	switch (getBaseType(cache->typid))
	{
		case INT2OID:
			value = isnull ? 0 : DatumGetInt16(d);
			break;
		case INT4OID:
			value = isnull ? 0 : DatumGetInt32(d);
			break;
		case INT8OID:
			value = isnull ? 0 : DatumGetInt64(d);
			break;
		default:
			return PointerGetDatum(tuple);
	}

	/* The trigger also covers temporary tables and INSERT ... SELECT, whose
	 * target may not be visible to the raw grammar's literal substitution. */
	if (mys_is_mysql_session() && TRIGGER_FIRED_BY_INSERT(td->tg_event) &&
		(isnull || (value == 0 && !(mys_sqlMode & MYS_MODE_NO_AUTO_VALUE_ON_ZERO))))
	{
		bool notnull = false;
		int attnum = cache->attnum;

		value = nextval_internal(cache->seqoid, false);
		switch (getBaseType(cache->typid))
		{
			case INT2OID: d = Int16GetDatum((int16) value); break;
			case INT4OID: d = Int32GetDatum((int32) value); break;
			default: d = Int64GetDatum(value); break;
		}
		tuple = heap_modify_tuple_by_cols(tuple, desc, 1, &attnum, &d, &notnull);
		return PointerGetDatum(tuple);
	}

	/*
	 * The sequence belongs to the table's owner; inserting into the table is
	 * what entitles to move it (as nextval() for the identity does).
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(rel->rd_rel->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_RESTRICTED_OPERATION);

	if (value > 0)
		sequence_advance(cache->seqoid, value, true);

	SetUserIdAndSecContext(save_userid, save_sec_context);
	return PointerGetDatum(tuple);
}

/*
 * A column that became AUTO_INCREMENT (MODIFY ... AUTO_INCREMENT) keeps its
 * values; the counter continues after the largest one, as in MySQL.
 */
void
mys_sync_auto_increment(Oid relid)
{
	Relation	rel = relation_open(relid, AccessExclusiveLock);
	TupleDesc	desc = RelationGetDescr(rel);
	Oid			seqoid = InvalidOid;
	AttrNumber	attnum = InvalidAttrNumber;
	Oid			typid = InvalidOid;
	Snapshot	snapshot;
	TableScanDesc scan;
	TupleTableSlot *slot;
	int64		max = 0;
	bool		found = false;

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AUTO_INCREMENT changes on partitioned tables are not supported")));

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && att->attidentity)
		{
			attnum = att->attnum;
			typid = getBaseType(att->atttypid);
			seqoid = getIdentitySequence(rel, attnum, true);
			break;
		}
	}
	if (!OidIsValid(seqoid) ||
		(typid != INT2OID && typid != INT4OID && typid != INT8OID))
	{
		relation_close(rel, NoLock);
		return;
	}

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(rel, snapshot, 0, NULL);
	slot = table_slot_create(rel, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d = slot_getattr(slot, attnum, &isnull);
		int64		v;

		if (isnull)
			continue;
		v = typid == INT2OID ? DatumGetInt16(d) :
			typid == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
		if (!found || v > max)
			max = v;
		found = true;
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	relation_close(rel, NoLock);

	if (found && max > 0)
		sequence_advance(seqoid, max, true);
}

/* does the table have no visible rows? */
static bool
table_is_empty(Relation rel)
{
	Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());
	TableScanDesc scan = table_beginscan(rel, snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	bool		empty = !table_scan_getnextslot(scan, ForwardScanDirection, slot);

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	return empty;
}

/*
 * ALTER TABLE ... AUTO_INCREMENT = n: the next generated value is n, unless
 * the counter is already beyond it.  On an empty table the counter is reset
 * to n even if that moves it back (openHalo 154ce7a, as MySQL does).
 */
void
mys_set_auto_increment(Oid relid, int64 next)
{
	Relation	rel = relation_open(relid, AccessExclusiveLock);
	TupleDesc	desc = RelationGetDescr(rel);
	Oid			seqoid = InvalidOid;
	bool		empty = false;

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AUTO_INCREMENT changes on partitioned tables are not supported")));

	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && att->attidentity)
		{
			seqoid = getIdentitySequence(rel, att->attnum, true);
			break;
		}
	}
	if (OidIsValid(seqoid))
		empty = table_is_empty(rel);
	relation_close(rel, NoLock);
	if (!OidIsValid(seqoid))
		return;

	if (empty)
	{
		check_sequence_write(seqoid);
		DirectFunctionCall3(setval3_oid, ObjectIdGetDatum(seqoid),
							Int64GetDatum(next), BoolGetDatum(false));
	}
	else
		sequence_advance(seqoid, next, false);
}

/* ----------------------------------------------------------------
 * Non-strict sql_mode: lenient conversion of INSERT / UPDATE values
 *
 * With STRICT_TRANS_TABLES / STRICT_ALL_TABLES off, MySQL stores the
 * closest valid value and reports a warning instead of failing:
 *	'12abc' -> 12, 'abc' -> 0, 300 into TINYINT -> 127, '1.5' -> 2,
 *	'toolong' into VARCHAR(3) -> 'too'.
 * mys_rewrite.c wraps literal values with these functions when the session
 * is not in strict mode.
 * ----------------------------------------------------------------
 */

/* longest numeric prefix of s (MySQL string-to-number conversion) */
static char *
numeric_prefix(const char *s, bool *complete)
{
	const char *p = s;
	const char *start;
	bool		digits = false;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	start = p;
	if (*p == '+' || *p == '-')
		p++;
	while (isdigit((unsigned char) *p))
		p++, digits = true;
	if (*p == '.')
	{
		p++;
		while (isdigit((unsigned char) *p))
			p++, digits = true;
	}
	if (digits && (*p == 'e' || *p == 'E'))
	{
		const char *e = p + 1;

		if (*e == '+' || *e == '-')
			e++;
		if (isdigit((unsigned char) *e))
		{
			while (isdigit((unsigned char) *e))
				e++;
			p = e;
		}
	}
	*complete = digits;
	if (digits)
	{
		const char *q = p;

		while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
			q++;
		*complete = (*q == '\0');
		return pnstrdup(start, p - start);
	}
	return pstrdup("0");
}

static Numeric
lenient_numeric_value(text *t)
{
	char	   *s = text_to_cstring(t);
	bool		complete;
	char	   *num = numeric_prefix(s, &complete);

	if (!complete)
		mys_add_warning(1265, "Data truncated during numeric conversion");
	return DatumGetNumeric(DirectFunctionCall3(numeric_in, CStringGetDatum(num),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1)));
}

/* mysql.lenient_int(text, lo, hi): rounded, clipped to [lo, hi] */
PG_FUNCTION_INFO_V1(mys_lenient_int);
Datum
mys_lenient_int(PG_FUNCTION_ARGS)
{
	Numeric		n = lenient_numeric_value(PG_GETARG_TEXT_PP(0));
	int64		lo = PG_GETARG_INT64(1);
	int64		hi = PG_GETARG_INT64(2);
	Datum		r = DirectFunctionCall2(numeric_round, NumericGetDatum(n), Int32GetDatum(0));
	Datum		lod = NumericGetDatum(int64_to_numeric(lo));
	Datum		hid = NumericGetDatum(int64_to_numeric(hi));

	if (DatumGetBool(DirectFunctionCall2(numeric_lt, r, lod)))
	{
		mys_add_warning(1264, "Out of range value for column");
		PG_RETURN_INT64(lo);
	}
	if (DatumGetBool(DirectFunctionCall2(numeric_gt, r, hid)))
	{
		mys_add_warning(1264, "Out of range value for column");
		PG_RETURN_INT64(hi);
	}
	PG_RETURN_DATUM(DirectFunctionCall1(numeric_int8, r));
}

/* mysql.lenient_numeric(text, typmod): rounded to the scale, clipped */
PG_FUNCTION_INFO_V1(mys_lenient_numeric);
Datum
mys_lenient_numeric(PG_FUNCTION_ARGS)
{
	Numeric		n = lenient_numeric_value(PG_GETARG_TEXT_PP(0));
	int32		typmod = PG_GETARG_INT32(1);
	int			precision,
				scale;
	Datum		r,
				max;
	char	   *maxstr;

	if (typmod < (int32) VARHDRSZ)
		PG_RETURN_NUMERIC(n);
	precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
	scale = (typmod - VARHDRSZ) & 0xffff;
	r = DirectFunctionCall2(numeric_round, NumericGetDatum(n), Int32GetDatum(scale));

	/* largest value: (precision - scale) nines, then scale nines after the point */
	maxstr = palloc(precision + 3);
	{
		int			k = 0;

		for (int i = 0; i < precision - scale; i++)
			maxstr[k++] = '9';
		if (precision - scale <= 0)
			maxstr[k++] = '0';
		if (scale > 0)
		{
			maxstr[k++] = '.';
			for (int i = 0; i < scale; i++)
				maxstr[k++] = '9';
		}
		maxstr[k] = '\0';
	}
	max = DirectFunctionCall3(numeric_in, CStringGetDatum(maxstr),
							  ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	if (DatumGetBool(DirectFunctionCall2(numeric_gt, r, max)))
	{
		mys_add_warning(1264, "Out of range value for column");
		PG_RETURN_DATUM(max);
	}
	if (DatumGetBool(DirectFunctionCall2(numeric_lt, r,
										 DirectFunctionCall1(numeric_uminus, max))))
	{
		mys_add_warning(1264, "Out of range value for column");
		PG_RETURN_DATUM(DirectFunctionCall1(numeric_uminus, max));
	}
	PG_RETURN_DATUM(r);
}

PG_FUNCTION_INFO_V1(mys_lenient_float8);
Datum
mys_lenient_float8(PG_FUNCTION_ARGS)
{
	Numeric		n = lenient_numeric_value(PG_GETARG_TEXT_PP(0));

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_float8, NumericGetDatum(n)));
}

/* mysql.lenient_varchar(text, n): at most n characters */
PG_FUNCTION_INFO_V1(mys_lenient_varchar);
Datum
mys_lenient_varchar(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	int32		maxchars = PG_GETARG_INT32(1);
	char	   *s = VARDATA_ANY(t);
	int			len = VARSIZE_ANY_EXHDR(t);
	int			cut;

	if (maxchars < 0)
		PG_RETURN_TEXT_P(t);
	cut = pg_mbcharcliplen(s, len, maxchars);
	if (cut < len)
	{
		mys_add_warning(1265, "Data truncated for column");
		PG_RETURN_TEXT_P(cstring_to_text_with_len(s, cut));
	}
	PG_RETURN_TEXT_P(t);
}

/* Catalog-derived metadata shared by SHOW COLUMNS and SHOW CREATE TABLE. */
/*
 * mysql.decimal_column_type(typmod): the MySQL column type of a numeric.
 * One MySQL cannot type (no typmod, scale < 0 or > 30 or above the
 * precision, precision > 65) is shown as DECIMAL(65,30), MySQL's widest,
 * with a warning: values with more than 35 integer or 30 fractional digits
 * would not survive a replay.
 */
PG_FUNCTION_INFO_V1(mys_decimal_column_type);

Datum
mys_decimal_column_type(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);
	int			precision = -1;
	int			scale = -1;

	if (typmod >= (int32) VARHDRSZ)
	{
		precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
		scale = ((((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024);
	}
	if (precision < 1 || precision > 65 || scale < 0 || scale > 30 || scale > precision)
	{
		mys_add_warning(1105, "numeric column without a MySQL precision shown as decimal(65,30)");
		precision = 65;
		scale = 30;
	}
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("decimal(%d,%d)", precision, scale)));
}

/*
 * mysql.index_key_parts(index): its key parts as MySQL shows them, in key
 * order: (seq, column_name, sub_part), sub_part the n of a prefix col(n)
 * aux_mysql indexes as left(col, n) / substr(col, 1, n).  Other expressions,
 * which MySQL 5.7 cannot show, are left out.
 */
PG_FUNCTION_INFO_V1(mys_index_key_parts);

Datum
mys_index_key_parts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	idx;
	List	   *exprs;
	ListCell   *expr;
	int			seq = 0;

	InitMaterializedSRF(fcinfo, 0);
	idx = try_relation_open(PG_GETARG_OID(0), AccessShareLock);
	if (idx == NULL)
		return (Datum) 0;
	if (idx->rd_index == NULL)
	{
		relation_close(idx, AccessShareLock);
		return (Datum) 0;
	}
	exprs = RelationGetIndexExpressions(idx);
	expr = list_head(exprs);
	for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
	{
		AttrNumber	attnum = idx->rd_index->indkey.values[i];
		int			length = -1;
		bool		bytes;
		bool		digest;
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		if (attnum == 0)
		{
			if (expr == NULL)
				break;
			if (!mys_index_prefix(lfirst(expr), &attnum, &length, &bytes, &digest))
				attnum = 0;
			expr = lnext(exprs, expr);
		}
		if (attnum <= 0)
			continue;
		values[0] = Int32GetDatum(++seq);
		values[1] = CStringGetTextDatum(get_attname(idx->rd_index->indrelid, attnum, false));
		values[2] = Int32GetDatum(length);
		nulls[2] = length < 0;
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(idx, AccessShareLock);
	return (Datum) 0;
}

/* mysql.index_key_name(index): the key's MySQL name (mys_rewrite.c) */
PG_FUNCTION_INFO_V1(mys_sql_index_key_name);

Datum
mys_sql_index_key_name(PG_FUNCTION_ARGS)
{
	char	   *name = mys_index_key_name(PG_GETARG_OID(0));

	if (name == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(name));
}

PG_FUNCTION_INFO_V1(mys_column_extra);
Datum
mys_column_extra(PG_FUNCTION_ARGS)
{
	Oid relid = PG_GETARG_OID(0);
	int attnum = PG_GETARG_INT32(1);
	Relation rel = relation_open(relid, AccessShareLock);
	StringInfoData result;

	initStringInfo(&result);
	if (attnum > 0 && attnum <= RelationGetDescr(rel)->natts)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);

		if (att->attidentity)
			appendStringInfoString(&result, "auto_increment");
		/* mysqldump leaves these columns out of its INSERTs */
		if (att->attgenerated)
			appendStringInfoString(&result,
								   att->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL ||
								   mys_generated_virtual(relid, attnum) ?
								   "VIRTUAL GENERATED" : "STORED GENERATED");
		if (rel->trigdesc)
			for (int i = 0; i < rel->trigdesc->numtriggers; i++)
			{
				Trigger *trigger = &rel->trigdesc->triggers[i];

				if (strcmp(trigger->tgname, "aux_mysql_on_update") != 0)
					continue;
				for (int k = 0; k < trigger->tgnargs; k++)
					if (atoi(trigger->tgargs[k]) == attnum)
					{
						if (result.len)
							appendStringInfoChar(&result, ' ');
						appendStringInfoString(&result, "on update CURRENT_TIMESTAMP");
						if (att->atttypmod > 0)
							appendStringInfo(&result, "(%d)", att->atttypmod);
					}
			}
	}
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

/*
 * MySQL text of a deparsed expression: an identifier PostgreSQL printed in
 * double quotes, which MySQL reads as a string, is a column name in
 * backquotes, or bare when PostgreSQL needs no quotes for it (function and
 * type names); a backslash in a string literal is doubled unless
 * NO_BACKSLASH_ESCAPES.
 */
static char *
mysql_expression_text(const char *s, TupleDesc desc, List *display_calls)
{
	StringInfoData buf;
	bool		backslash_escapes = !(mys_sqlMode & MYS_MODE_NO_BACKSLASH_ESCAPES);

	initStringInfo(&buf);
	while (*s)
	{
		/* Private deparse parameters stand for already-rendered MySQL calls.
		 * Quoted literals/identifiers below are consumed whole, so a user's
		 * '$1' or a column named "$1" is never substituted. */
		if (*s == '$' && display_calls && isdigit((unsigned char) s[1]))
		{
			char *end;
			long n = strtol(s + 1, &end, 10);

			if (n > 0 && n <= list_length(display_calls))
			{
				appendStringInfoString(&buf, list_nth(display_calls, n - 1));
				s = end;
				continue;
			}
			appendStringInfoChar(&buf, *s++);
		}
		else if (*s == '\'')
		{
			/* PG prints typed text/bytea constants with a postfix cast.  MySQL
			 * has text literals and X'hex'; retain explicit user typmods. */
			if (s[1] == '\\' && s[2] == 'x')
			{
				const char *end = s + 3;
				const char *after = NULL;

				while (isxdigit((unsigned char) *end)) end++;
				if (*end == '\'' && (end - (s + 3)) % 2 == 0)
				{
					if (strncmp(end + 1, "::bytea", 7) == 0) after = end + 8;
					else if (strncmp(end + 1, "::\"bytea\"", 9) == 0) after = end + 10;
				}
				if (after && !isalnum((unsigned char) *after) && *after != '_')
				{
					appendStringInfoString(&buf, "X'");
					appendBinaryStringInfo(&buf, s + 3, end - (s + 3));
					appendStringInfoChar(&buf, '\'');
					s = after;
					continue;
				}
			}
			appendStringInfoChar(&buf, *s++);
			for (; *s; s++)
			{
				if (*s == '\'' && s[1] != '\'')
					break;
				if (*s == '\'')
					appendStringInfoChar(&buf, *s++);
				else if (*s == '\\' && backslash_escapes)
					appendStringInfoChar(&buf, '\\');
				appendStringInfoChar(&buf, *s);
			}
			if (*s)
				appendStringInfoChar(&buf, *s++);
			if (strncmp(s, "::text", 6) == 0 &&
				!isalnum((unsigned char) s[6]) && s[6] != '_' && s[6] != '(')
				s += 6;
			else if (strncmp(s, "::\"text\"", 8) == 0 && s[8] != '(')
				s += 8;
		}
		else if (*s == '"')
		{
			StringInfoData name;
			bool		column = false;

			initStringInfo(&name);
			for (s++; *s; s++)
			{
				if (*s == '"' && s[1] != '"')
					break;
				if (*s == '"')
					s++;
				appendStringInfoChar(&name, *s);
			}
			if (*s)
				s++;
			for (int i = 0; desc && i < desc->natts && !column; i++)
				column = !TupleDescAttr(desc, i)->attisdropped &&
					strcmp(NameStr(TupleDescAttr(desc, i)->attname), name.data) == 0;
			if (desc && !column && strcmp(quote_identifier(name.data), name.data) == 0)
				appendStringInfoString(&buf, name.data);
			else
			{
				appendStringInfoChar(&buf, '`');
				for (char *p = name.data; *p; p++)
				{
					if (*p == '`')
						appendStringInfoChar(&buf, '`');
					appendStringInfoChar(&buf, *p);
				}
				appendStringInfoChar(&buf, '`');
			}
		}
		else
			appendStringInfoChar(&buf, *s++);
	}
	return buf.data;
}

/* Apply the current writer's mode, including writes from prepared plans and
 * ALTER rewrites.  Do not bake CREATE TABLE's sql_mode into the expression. */
static bool
generated_assignment_strict(void)
{
	MysStmtInfo *info;

	if (!mys_is_mysql_session()) return true;
	info = mys_current_stmt_info();
	return (mys_sqlMode & (MYS_MODE_STRICT_TRANS_TABLES | MYS_MODE_STRICT_ALL_TABLES)) &&
		!(info && info->ignore);
}

PG_FUNCTION_INFO_V1(mys_generated_varchar);
Datum
mys_generated_varchar(PG_FUNCTION_ARGS)
{
	text *value = PG_GETARG_TEXT_PP(0);
	int limit = PG_GETARG_INT32(1);
	bool bpchar = PG_GETARG_BOOL(2);
	char *data = VARDATA_ANY(value);
	int len = VARSIZE_ANY_EXHDR(value);
	int cut = pg_mbcharcliplen(data, len, limit);
	bool significant = false;

	for (int i = cut; i < len; i++)
		if (data[i] != ' ')
			significant = true;
	if (cut < len)
	{
		if ((!bpchar || significant) && generated_assignment_strict())
			ereport(ERROR, (errcode(MAKE_SQLSTATE('M','1','2','6','5')),
								errmsg("Data truncated for generated column")));
		/* MySQL CHAR silently discards trailing spaces; VARCHAR warns. */
		if (!bpchar || significant)
			mys_add_warning(1265, "Data truncated for generated column");
		PG_RETURN_TEXT_P(cstring_to_text_with_len(data, cut));
	}
	PG_RETURN_TEXT_P(value);
}

/* Generated DECIMAL(p,s) uses assignment range rules at execution time, even
 * for an immutable source expression. Round before checking the signed range. */
PG_FUNCTION_INFO_V1(mys_generated_numeric);
Datum
mys_generated_numeric(PG_FUNCTION_ARGS)
{
	Numeric value = PG_GETARG_NUMERIC(0);
	int32 typmod = PG_GETARG_INT32(1);
	int precision, scale, k = 0;
	Datum rounded, maximum, minimum, clipped;
	char *limit;

	if (typmod < (int32) VARHDRSZ) PG_RETURN_NUMERIC(value);
	precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
	scale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
	/* Native PG-only typmods keep their native conversion semantics. */
	if (precision < 1 || scale < 0 || scale > precision)
		PG_RETURN_DATUM(DirectFunctionCall2(numeric, NumericGetDatum(value), Int32GetDatum(typmod)));
	rounded = DirectFunctionCall2(numeric_round, NumericGetDatum(value), Int32GetDatum(scale));
	limit = palloc(precision + 3);
	for (int i = 0; i < precision - scale; i++) limit[k++] = '9';
	if (precision == scale) limit[k++] = '0';
	if (scale > 0)
	{
		limit[k++] = '.';
		for (int i = 0; i < scale; i++) limit[k++] = '9';
	}
	limit[k] = '\0';
	maximum = DirectFunctionCall3(numeric_in, CStringGetDatum(limit),
		ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	minimum = DirectFunctionCall1(numeric_uminus, maximum);
	if (DatumGetBool(DirectFunctionCall2(numeric_gt, rounded, maximum)))
		clipped = maximum;
	else if (DatumGetBool(DirectFunctionCall2(numeric_lt, rounded, minimum)))
		clipped = minimum;
	else
		PG_RETURN_DATUM(rounded);
	if (generated_assignment_strict())
		ereport(ERROR, (errcode(MAKE_SQLSTATE('M','1','2','6','4')),
						errmsg("Out of range value for generated column")));
	mys_add_warning(1264, "Out of range value for generated column");
	PG_RETURN_DATUM(clipped);
}

/* Only peel our conversion envelope.  User-written casts inside the marker
 * remain part of the expression, including when ALTER/LIKE reparses it. */
static Node *
generation_source(Node *node)
{
	Node *original = node;

	while (node)
	{
		if (IsA(node, FuncExpr))
		{
			FuncExpr *f = (FuncExpr *) node;
			char *name = get_func_name(f->funcid);
			bool ours = name &&
				strcmp(get_namespace_name(get_func_namespace(f->funcid)), "mysql") == 0;

			if (ours && strcmp(name, "generated_value") == 0)
				return linitial(f->args);
			if (f->funcformat == COERCE_EXPLICIT_CAST ||
				f->funcformat == COERCE_IMPLICIT_CAST ||
				(ours && (strcmp(name, "generated_varchar") == 0 ||
						  strcmp(name, "generated_numeric") == 0)))
				node = linitial(f->args);
			else
				break;
		}
		else if (IsA(node, CoerceToDomain))
			node = (Node *) ((CoerceToDomain *) node)->arg;
		else if (IsA(node, CoerceViaIO))
			node = (Node *) ((CoerceViaIO *) node)->arg;
		else if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		else
			break;
	}
	return original; /* a pre-1.9 generator has no conversion marker */
}

/* a generator's parameters are the arguments of its call */
static Node *
generator_arguments(Node *node, List *args)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN &&
		((Param *) node)->paramid >= 1 && ((Param *) node)->paramid <= list_length(args))
		return copyObject(list_nth(args, ((Param *) node)->paramid - 1));
	/* the COLLATE that keeps a parameter collated as its column */
	if (IsA(node, CollateExpr))
	{
		Node	   *arg = generator_arguments((Node *) ((CollateExpr *) node)->arg, args);

		if (exprCollation(arg) == ((CollateExpr *) node)->collOid)
			return arg;
	}
	return expression_tree_mutator(node, generator_arguments, args);
}

/* the expression a generator function (mys_utility.c) computes */
Node *
mys_unwrap_generator(Node *expr)
{
	Node	   *call = strip_implicit_coercions(expr);
	char	   *name;
	HeapTuple	tup;
	Datum		body;
	bool		isnull;
	Node	   *query;

	if (!IsA(call, FuncExpr) ||
		(name = get_func_name(((FuncExpr *) call)->funcid)) == NULL ||
		strncmp(name, MYS_GENERATOR_PREFIX, strlen(MYS_GENERATOR_PREFIX)) != 0)
		return expr;
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(((FuncExpr *) call)->funcid));
	if (!HeapTupleIsValid(tup))
		return expr;
	body = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
	query = isnull ? NULL : stringToNode(TextDatumGetCString(body));
	ReleaseSysCache(tup);
	if (query == NULL || !IsA(query, Query) || list_length(((Query *) query)->targetList) != 1)
		return expr;
	return generator_arguments(generation_source((Node *) linitial_node(TargetEntry, ((Query *) query)->targetList)->expr),
							   ((FuncExpr *) call)->args);
}

/* mysql.generated_column(name, expr) shows as the column it reads */
static Node *
generated_refs_to_columns(Node *node, void *context)
{
	Relation	rel = (Relation) context;

	if (node == NULL)
		return NULL;
	if (IsA(node, FuncExpr) && list_length(((FuncExpr *) node)->args) == 2 &&
		IsA(linitial(((FuncExpr *) node)->args), Const))
	{
		FuncExpr   *f = (FuncExpr *) node;
		Const	   *name = linitial_node(Const, f->args);
		char	   *func = get_func_name(f->funcid);
		AttrNumber	attnum;

		if (func && strcmp(func, MYS_GENERATED_REF) == 0 && !name->constisnull &&
			name->consttype == TEXTOID &&
			strcmp(get_namespace_name(get_func_namespace(f->funcid)), "mysql") == 0 &&
			(attnum = get_attnum(RelationGetRelid(rel),
								 TextDatumGetCString(name->constvalue))) > 0)
		{
			Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);

			return (Node *) makeVar(1, attnum, att->atttypid, att->atttypmod,
									att->attcollation, 0);
		}
	}
	return expression_tree_mutator(node, generated_refs_to_columns, context);
}

/*
 * mysql.column_generation(table, attnum): the expression of a generated
 * column as SHOW CREATE TABLE and information_schema.COLUMNS show it, NULL
 * for another column.  aux_mysql reads it back as the same expression.
 */
static Node *
generated_display_constants(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Const) &&
		mys_binary_base_type(((Const *) node)->consttype) == BYTEAOID)
	{
		Const *constant = copyObject((Const *) node);

		/* binary_n / varbinary_n share bytea's layout.  Emit their actual
		 * bytes as MySQL X'hex'. */
		constant->consttype = BYTEAOID;
		constant->consttypmod = -1;
		constant->constcollid = InvalidOid;
		return (Node *) constant;
	}
	return expression_tree_mutator(node, generated_display_constants, context);
}

static FuncExpr *
generated_helper(Node *node, const char *name, int nargs)
{
	FuncExpr *f;
	char *actual;

	if (node == NULL || !IsA(node, FuncExpr)) return NULL;
	f = (FuncExpr *) node;
	if (nargs >= 0 && list_length(f->args) != nargs) return NULL;
	actual = get_func_name(f->funcid);
	if (!actual || strcmp(actual, name) != 0 ||
		strcmp(get_namespace_name(get_func_namespace(f->funcid)), "mysql") != 0)
		return NULL;
	return f;
}

/* A concat_one boundary retains a nested user CONCAT as one argument. */
static bool
generated_concat_args(Node *node, List **args)
{
	FuncExpr *f = generated_helper(node, "concat_one", 1);

	if (f)
	{
		*args = lappend(*args, linitial(f->args));
		return true;
	}
	f = generated_helper(node, "concat_pair", 2);
	return f && generated_concat_args(linitial(f->args), args) &&
		generated_concat_args(lsecond(f->args), args);
}

/* Recognize only the CASE constructed by mys_binary_func_call for ELT. */
static List *
generated_elt_args(Node *node)
{
	CaseExpr *choice;
	FuncExpr *index;
	List *args;
	int ordinal = 0;

	if (!IsA(node, CaseExpr)) return NIL;
	choice = (CaseExpr *) node;
	index = generated_helper((Node *) choice->arg, "elt_index", 1);
	if (!index || !IsA(choice->defresult, Const) ||
		!((Const *) choice->defresult)->constisnull) return NIL;
	args = list_copy(index->args);
	foreach_node(CaseWhen, branch, choice->args)
	{
		OpExpr *test;
		Const *number;
		FuncExpr *value = generated_helper((Node *) branch->result, "raw_bytes", 1);

		if (!value || !IsA(branch->expr, OpExpr)) return NIL;
		test = (OpExpr *) branch->expr;
		if (list_length(test->args) != 2 || get_opcode(test->opno) != F_INT4EQ ||
			!IsA(linitial(test->args), CaseTestExpr) || !IsA(lsecond(test->args), Const)) return NIL;
		number = lsecond_node(Const, test->args);
		if (number->consttype != INT4OID || number->constisnull ||
			DatumGetInt32(number->constvalue) != ++ordinal) return NIL;
		args = lappend(args, linitial(value->args));
	}
	return ordinal ? args : NIL;
}

typedef struct GeneratedDisplay
{
	List *namespace;
	TupleDesc desc;
	List *calls;
} GeneratedDisplay;

/* Display only: these private Param nodes are never planned, stored, or used
 * by ALTER/LIKE. Keeping their original result types lets PG deparse the rest
 * of the expression, while MySQL function names need no PG overload/schema. */
static Node *
generated_display_calls(Node *node, void *context)
{
	GeneratedDisplay *display = context;
	const char *name = NULL;
	List *args = NIL;
	FuncExpr *f;

	if (node == NULL) return NULL;
	if (generated_concat_args(node, &args))
		name = "CONCAT";
	else if ((f = generated_helper(node, "string_result", 2)) != NULL)
	{
		FuncExpr *ws = generated_helper(lsecond(f->args), "concat_ws_bytes", -1);

		args = ws ? ws->args : generated_elt_args(lsecond(f->args));
		if (args) name = ws ? "CONCAT_WS" : "ELT";
	}
	if (name)
	{
		StringInfoData sql;
		Param *placeholder = makeNode(Param);
		bool first = true;

		initStringInfo(&sql);
		appendStringInfo(&sql, "%s(", name);
		foreach_ptr(Node, arg, args)
		{
			Node *shown = generated_display_calls(arg, display);
			char *text;

			if (!first) appendStringInfoString(&sql, ", ");
			/* A VARIADIC "any" argument can retain UNKNOWNOID. MySQL's bare
			 * NULL needs no PostgreSQL ::unknown cast. */
			text = IsA(shown, Const) && ((Const *) shown)->constisnull &&
				((Const *) shown)->consttype == UNKNOWNOID ? "NULL" :
				deparse_expression(shown, display->namespace, false, false);
			appendStringInfoString(&sql, mysql_expression_text(text, display->desc, display->calls));
			first = false;
		}
		appendStringInfoChar(&sql, ')');
		display->calls = lappend(display->calls, sql.data);
		placeholder->paramkind = PARAM_EXTERN;
		placeholder->paramid = list_length(display->calls);
		placeholder->paramtype = exprType(node);
		placeholder->paramtypmod = exprTypmod(node);
		placeholder->paramcollid = exprCollation(node);
		placeholder->location = -1;
		return (Node *) placeholder;
	}
	return expression_tree_mutator(node, generated_display_calls, context);
}

PG_FUNCTION_INFO_V1(mys_column_generation);

Datum
mys_column_generation(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int			attnum = PG_GETARG_INT32(1);
	Relation	rel = try_relation_open(relid, AccessShareLock);
	TupleConstr *constr;
	Node	   *expr = NULL;
	char	   *text;
	bool		save_quote = quote_all_identifiers;
	bool		save_strings = standard_conforming_strings;
	GeneratedDisplay display = {0};

	if (rel == NULL)
		PG_RETURN_NULL();
	constr = RelationGetDescr(rel)->constr;
	if (attnum > 0 && attnum <= RelationGetDescr(rel)->natts &&
		TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attgenerated && constr)
		for (int i = 0; i < constr->num_defval; i++)
			if (constr->defval[i].adnum == attnum)
				expr = mys_unwrap_generator(stringToNode(constr->defval[i].adbin));
	if (expr == NULL)
	{
		relation_close(rel, AccessShareLock);
		PG_RETURN_NULL();
	}

	expr = generated_refs_to_columns(expr, rel);
	expr = generated_display_constants(expr, NULL);

	/* every identifier delimited; literals with plain backslashes */
	quote_all_identifiers = true;
	standard_conforming_strings = true;
	PG_TRY();
	{
		display.namespace = deparse_context_for(RelationGetRelationName(rel), relid);
		display.desc = RelationGetDescr(rel);
		expr = generated_display_calls(expr, &display);
		text = deparse_expression(expr, display.namespace, false, false);
	}
	PG_FINALLY();
	{
		quote_all_identifiers = save_quote;
		standard_conforming_strings = save_strings;
	}
	PG_END_TRY();
	text = mysql_expression_text(text, RelationGetDescr(rel), display.calls);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(text));
}

/* Normalize the grammar's SELECT for SHOW CREATE after catalog access is safe
 * (in mys_rewrite_raw_stmts, never in the raw grammar).  Both SHOW spellings
 * must describe a view with the same four VARCHAR result columns. */
Node *
mys_rewrite_show_create(Node *stmt)
{
	SelectStmt *select;
	ResTarget *target;
	FuncCall *call;
	bool view;

	if (stmt == NULL || !IsA(stmt, SelectStmt))
		return stmt;
	select = (SelectStmt *) stmt;
	if (list_length(select->targetList) != 2 && list_length(select->targetList) != 4)
		return stmt;
	target = lsecond_node(ResTarget, select->targetList);
	if (!target->name || !IsA(target->val, FuncCall))
		return stmt;
	call = (FuncCall *) target->val;
	if (list_length(call->funcname) != 2 || list_length(call->args) != 2 ||
		strcmp(strVal(linitial(call->funcname)), "mysql") != 0)
		return stmt;
	view = strcmp(target->name, "Create View") == 0 &&
		strcmp(strVal(lsecond(call->funcname)), "show_create_view") == 0;
	if (!view && strcmp(target->name, "Create Table") == 0 &&
		strcmp(strVal(lsecond(call->funcname)), "show_create_table") == 0)
	{
		A_Const *schema;
		A_Const *name;
		Oid relid;

		if (!IsA(linitial(call->args), A_Const) || !IsA(lsecond(call->args), A_Const))
			return stmt;
		schema = linitial_node(A_Const, call->args);
		name = lsecond_node(A_Const, call->args);
		if (name->isnull || !IsA(&name->val, String) ||
			(!schema->isnull && !IsA(&schema->val, String)))
			return stmt;
		relid = RangeVarGetRelid(makeRangeVar(schema->isnull ? NULL : strVal(&schema->val),
											strVal(&name->val), -1), AccessShareLock, true);
		view = OidIsValid(relid) && get_rel_relkind(relid) == RELKIND_VIEW;
		if (view)
		{
			linitial_node(ResTarget, select->targetList)->name = pstrdup("View");
			target->name = pstrdup("Create View");
			for (int i = 0; i < 2; i++)
			{
				ResTarget *extra = makeNode(ResTarget);

				extra->name = pstrdup(i ? "collation_connection" : "character_set_client");
				extra->val = (Node *) makeStringConst(i ? "utf8mb4_general_ci" : "utf8mb4", -1);
				select->targetList = lappend(select->targetList, extra);
			}
		}
	}
	else if (!view)
		return stmt;
	foreach_node(ResTarget, field, select->targetList)
	{
		TypeCast *cast = makeNode(TypeCast);

		cast->arg = field->val;
		cast->typeName = makeTypeNameFromOid(VARCHAROID, -1);
		cast->location = -1;
		field->val = (Node *) cast;
	}
	return stmt;
}

/* Catalog default -> replayable MySQL literal.  Folding only immutable
 * constants is essential: SHOW must never advance a sequence or evaluate a
 * volatile default.  bytea_out's '\x...' is a PG literal, not a MySQL one. */
PG_FUNCTION_INFO_V1(mys_column_default);
Datum
mys_column_default(PG_FUNCTION_ARGS)
{
	Oid relid = PG_GETARG_OID(0);
	int attnum = PG_GETARG_INT32(1);
	Relation rel = try_relation_open(relid, AccessShareLock);
	Node *expr = NULL;
	char *result;
	bool save_strings = standard_conforming_strings;

	if (rel == NULL)
		PG_RETURN_NULL();
	if (attnum > 0 && attnum <= RelationGetDescr(rel)->natts &&
		!TupleDescAttr(RelationGetDescr(rel), attnum - 1)->attgenerated && rel->rd_att->constr)
		for (int i = 0; i < rel->rd_att->constr->num_defval; i++)
			if (rel->rd_att->constr->defval[i].adnum == attnum)
				expr = stringToNode(rel->rd_att->constr->defval[i].adbin);
	if (expr == NULL)
	{
		relation_close(rel, AccessShareLock);
		PG_RETURN_NULL();
	}
	/* A column default already has its destination's character/binary type.
	 * X'...' preserves NUL and non-UTF8 bytes across SHOW and replay. */
	if (mys_binary_base_type(exprType(expr)) == BYTEAOID)
	{
		Node *constant = strip_implicit_coercions(eval_const_expressions(NULL, expr));

		while (IsA(constant, CoerceToDomain))
			constant = strip_implicit_coercions((Node *) ((CoerceToDomain *) constant)->arg);
		if (IsA(constant, Const) && !((Const *) constant)->constisnull &&
			mys_binary_base_type(((Const *) constant)->consttype) == BYTEAOID)
		{
			bytea *value = DatumGetByteaPP(((Const *) constant)->constvalue);
			int len = VARSIZE_ANY_EXHDR(value);
			StringInfoData buf;
			static const char hex[] = "0123456789abcdef";
			unsigned char *bytes = (unsigned char *) VARDATA_ANY(value);

			initStringInfo(&buf);
			appendStringInfoString(&buf, "X'");
			for (int i = 0; i < len; i++)
			{
				appendStringInfoChar(&buf, hex[bytes[i] >> 4]);
				appendStringInfoChar(&buf, hex[bytes[i] & 15]);
			}
			appendStringInfoChar(&buf, '\'');
			relation_close(rel, AccessShareLock);
			PG_RETURN_TEXT_P(cstring_to_text(buf.data));
		}
	}
	standard_conforming_strings = true;
	PG_TRY();
	{
		result = deparse_expression(expr, deparse_context_for(RelationGetRelationName(rel), relid), false, false);
	}
	PG_FINALLY();
	{
		standard_conforming_strings = save_strings;
	}
	PG_END_TRY();
	result = mysql_expression_text(result, RelationGetDescr(rel), NIL);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/* Domain-to-bytea relabels are representation-only casts inserted by PG for
 * functions such as HEX(blob). A native dump replays them as explicit casts.
 * Strip these byte-preserving relabels from a display copy only; conversions
 * that change values and the stored view remain unchanged. */
static Node *
view_display_casts(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Query))
		return (Node *) query_tree_mutator((Query *) node, view_display_casts, context, 0);
	if (IsA(node, RelabelType))
	{
		RelabelType *cast = (RelabelType *) node;

		if (cast->resulttype == BYTEAOID &&
			mys_binary_base_type(exprType((Node *) cast->arg)) == BYTEAOID)
			return view_display_casts((Node *) cast->arg, context);
	}
	return expression_tree_mutator(node, view_display_casts, context);
}

/* Preserve the parsed query, including operators, commas, literals, aliases,
 * joins and predicates.  The old whitespace splitter destroyed all of these. */
PG_FUNCTION_INFO_V1(mys_show_create_view);
Datum
mys_show_create_view(PG_FUNCTION_ARGS)
{
	char *schema = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Oid relid = RangeVarGetRelid(makeRangeVar(schema, name, -1), AccessShareLock, false);
	HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	Form_pg_class cl = (Form_pg_class) GETSTRUCT(tuple);
	char *owner = GetUserNameFromId(cl->relowner, false);
	Relation view;
	Query *query;
	char *definition;
	StringInfoData buf;
	bool save_quote = quote_all_identifiers;
	bool save_strings = standard_conforming_strings;

	if (cl->relkind != RELKIND_VIEW)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("%s is not a view", name)));
	ReleaseSysCache(tuple);
	view = relation_open(relid, AccessShareLock);
	query = (Query *) view_display_casts((Node *) get_view_query(view), NULL);
	/* ALTER VIEW RENAME COLUMN changes the descriptor, not the stored target
	 * aliases. pg_get_querydef has no result-descriptor argument. */
	foreach_node(TargetEntry, target, query->targetList)
		if (!target->resjunk && target->resno > 0 &&
			target->resno <= RelationGetDescr(view)->natts)
			target->resname = pstrdup(NameStr(TupleDescAttr(RelationGetDescr(view),
														 target->resno - 1)->attname));
	quote_all_identifiers = true;
	standard_conforming_strings = true;
	PG_TRY();
	{
		definition = pg_get_querydef(query, false);
	}
	PG_FINALLY();
	{
		quote_all_identifiers = save_quote;
		standard_conforming_strings = save_strings;
	}
	PG_END_TRY();
	relation_close(view, AccessShareLock);
	initStringInfo(&buf);
	appendStringInfoString(&buf, "CREATE ALGORITHM=UNDEFINED DEFINER=`");
	for (char *p = owner; *p; p++)
	{
		if (*p == '`') appendStringInfoChar(&buf, '`');
		appendStringInfoChar(&buf, *p);
	}
	appendStringInfoString(&buf, "`@`%` SQL SECURITY DEFINER VIEW `");
	for (char *p = name; *p; p++)
	{
		if (*p == '`') appendStringInfoChar(&buf, '`');
		appendStringInfoChar(&buf, *p);
	}
	appendStringInfoString(&buf, "` AS ");
	if (*definition && definition[strlen(definition) - 1] == ';')
		definition[strlen(definition) - 1] = '\0';
	appendStringInfoString(&buf, mysql_expression_text(definition, NULL, NIL));
	PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
