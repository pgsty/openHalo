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
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
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

/* Atomic with nextval; uses the core sequence page lock, ACLs and WAL. */
static void
sequence_advance(Oid seqoid, int64 value, bool is_called)
{
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
	if (isnull)
		return PointerGetDatum(tuple);
	switch (getBaseType(cache->typid))
	{
		case INT2OID:
			value = DatumGetInt16(d);
			break;
		case INT4OID:
			value = DatumGetInt32(d);
			break;
		case INT8OID:
			value = DatumGetInt64(d);
			break;
		default:
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
		DirectFunctionCall3(setval3_oid, ObjectIdGetDatum(seqoid),
							Int64GetDatum(next), BoolGetDatum(false));
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
