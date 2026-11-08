/*-------------------------------------------------------------------------
 *
 * mys_stmt.c
 *	  MySQL prepared statements: COM_STMT_PREPARE / EXECUTE / CLOSE / RESET /
 *	  SEND_LONG_DATA and the binary result set protocol.
 *
 * Execution strategy.  A MySQL statement can expand into several PG
 * statements (mys_rewrite.c: ON DUPLICATE KEY, hidden RETURNING for
 * LAST_INSERT_ID(), DDL side statements ...) and MySQL parameters behave
 * like literals of the bound type rather than like typed PG parameters
 * (e.g. "? + 1" with '1.5').  So COM_STMT_EXECUTE renders every bound value
 * as an exact literal for the MySQL scanner, at the positions where the
 * scanner found the '?' markers, and runs the result through the regular
 * COM_QUERY path; only the row encoding switches to the binary protocol.
 * This is what client-side prepared statements (Connector/J default,
 * PyMySQL, ...) do, done once on the server with the server's own scanner.
 *
 * Literals are rendered so that no value can change the statement:
 *	strings		('...')  with ' doubled and \ escaped (the MySQL scanner always
 *				processes backslash escapes); values that are not valid in the
 *				database encoding or contain NUL go out as _binary X'hex'
 *	binary		(_binary X'hex')
 *	numbers		(digits) from the binary encoding, never from client text;
 *				DECIMAL text is accepted only if it is a well-formed number
 *	temporal	('YYYY-MM-DD hh:mm:ss.ffffff') built from the binary fields
 *
 * PREPARE describes the result by analyzing the statement with NULL in
 * place of each marker.  Errors that depend on the parameter types (e.g.
 * an ambiguous function) are not reported at PREPARE time, because the real
 * values decide them at EXECUTE time; syntax errors and unknown objects are,
 * as in MySQL.
 *
 * SQL PREPARE / EXECUTE / DEALLOCATE PREPARE (at the end of this file) keep
 * their statements by name and use the same text, marker and literal rules.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "pgstat.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "halo_mysql.h"
#include "mys_proto.h"
#include "mys_gramparse.h"
#include "mys_parsenodes.h"
#include "adapter/mysql/systemVar.h"
#include "commands/mysql/mys_uservar.h"

/* MySQL's default max_prepared_stmt_count, applied per session here */
#define MYS_MAX_PREPARED_STMTS	16382

typedef struct MysPrepStmt
{
	uint32		id;				/* hash key */
	MemoryContext cxt;
	char	   *query;			/* statement text with '?' markers */
	int			nparams;
	int		   *param_loc;		/* byte offset of each marker */
	uint16	   *param_types;	/* type | flags << 8, from the last bind */
	bool		types_bound;
	StringInfo *long_data;		/* COM_STMT_SEND_LONG_DATA, per parameter */
	Size		long_data_bytes;
	bool		long_data_error;
} MysPrepStmt;

static HTAB *stmt_hash = NULL;
static uint32 next_stmt_id = 1;

/* SQL PREPARE: statements by name (see the end of this file) */
static HTAB *sql_stmt_hash = NULL;
static void sql_stmt_drop_all(void);

bool		mys_binary_resultset = false;
MysTextMap *mys_text_map = NULL;

/* ----------------------------------------------------------------
 * Registry
 * ----------------------------------------------------------------
 */
static void
init_hash(void)
{
	HASHCTL		ctl;

	if (stmt_hash)
		return;
	ctl.keysize = sizeof(uint32);
	ctl.entrysize = sizeof(MysPrepStmt);
	ctl.hcxt = TopMemoryContext;
	stmt_hash = hash_create("aux_mysql prepared statements", 64, &ctl,
							HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static MysPrepStmt *
lookup_stmt(uint32 id, const char *where, bool missing_ok)
{
	MysPrepStmt *stmt;

	init_hash();
	stmt = hash_search(stmt_hash, &id, HASH_FIND, NULL);
	if (stmt == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("Unknown prepared statement handler (%u) given to %s",
						id, where)));
	return stmt;
}

static void
drop_stmt(MysPrepStmt *stmt)
{
	MemoryContext cxt = stmt->cxt;
	uint32		id = stmt->id;

	hash_search(stmt_hash, &id, HASH_REMOVE, NULL);
	MemoryContextDelete(cxt);
}

/* max_prepared_stmt_count counts protocol and SQL statements together */
static long
prepared_stmt_count(void)
{
	return (stmt_hash ? hash_get_num_entries(stmt_hash) : 0) +
		(sql_stmt_hash ? hash_get_num_entries(sql_stmt_hash) : 0);
}

/* COM_RESET_CONNECTION: all prepared statements, protocol and SQL */
void
mys_stmt_close_all(void)
{
	HASH_SEQ_STATUS seq;
	MysPrepStmt *stmt;
	List	   *ids = NIL;
	ListCell   *lc;

	sql_stmt_drop_all();
	if (stmt_hash == NULL)
		return;
	hash_seq_init(&seq, stmt_hash);
	while ((stmt = hash_seq_search(&seq)) != NULL)
		ids = lappend_int(ids, (int) stmt->id);
	foreach(lc, ids)
	{
		uint32		id = (uint32) lfirst_int(lc);

		stmt = hash_search(stmt_hash, &id, HASH_FIND, NULL);
		if (stmt)
			drop_stmt(stmt);
	}
	list_free(ids);
}

/* end of a COM_QUERY / COM_STMT_EXECUTE cycle */
void
mys_stmt_query_done(void)
{
	mys_binary_resultset = false;
	mys_text_map = NULL;
}

/* ----------------------------------------------------------------
 * Parameter markers
 * ----------------------------------------------------------------
 */

/* Byte offsets of the '?' markers, found with the MySQL scanner itself */
static int
find_markers(const char *query, int **locs)
{
	mys_yy_extra_type yyextra;
	core_yyscan_t yyscanner;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			tok;
	int			n = 0;
	int			cap = 8;
	int		   *loc = palloc(cap * sizeof(int));

	memset(&yyextra, 0, sizeof(yyextra));
	yyscanner = mys_scanner_init(query, &yyextra.core_yy_extra,
								 &MysScanKeywords, MysScanKeywordTokens);
	while ((tok = mys_core_yylex(&yylval, &yylloc, yyscanner)) != 0)
	{
		if (tok != MysParam)
			continue;
		if (n == cap)
		{
			cap *= 2;
			loc = repalloc(loc, cap * sizeof(int));
		}
		loc[n++] = yylloc;
	}
	mys_scanner_finish(yyscanner);
	*locs = loc;
	return n;
}

/* ----------------------------------------------------------------
 * Literal rendering
 * ----------------------------------------------------------------
 */
static void
append_hex_literal(StringInfo buf, const char *s, int len)
{
	static const char hex[] = "0123456789abcdef";

	appendStringInfoString(buf, "(_binary X'");
	enlargeStringInfo(buf, len * 2 + 3);
	for (int i = 0; i < len; i++)
	{
		appendStringInfoChar(buf, hex[((uint8) s[i]) >> 4]);
		appendStringInfoChar(buf, hex[((uint8) s[i]) & 0x0f]);
	}
	appendStringInfoString(buf, "')");
}

static void
append_string_literal(StringInfo buf, const char *s, int len, bool binary)
{
	if (binary || memchr(s, '\0', len) != NULL ||
		!pg_verify_mbstr(GetDatabaseEncoding(), s, len, true))
	{
		append_hex_literal(buf, s, len);
		return;
	}

	appendStringInfoString(buf, "('");
	for (int i = 0; i < len; i++)
	{
		if (s[i] == '\'')
			appendStringInfoString(buf, "''");
		else if (s[i] == '\\' && !(mys_sqlMode & MYS_MODE_NO_BACKSLASH_ESCAPES))
			appendStringInfoString(buf, "\\\\");
		else
			appendStringInfoChar(buf, s[i]);
	}
	appendStringInfoString(buf, "')");
}

/* [+-]digits[.digits][e[+-]digits], or .digits */
static bool
is_decimal_text(const char *s, int len)
{
	int			i = 0;
	int			digits = 0;

	if (i < len && (s[i] == '+' || s[i] == '-'))
		i++;
	while (i < len && isdigit((unsigned char) s[i]))
		i++, digits++;
	if (i < len && s[i] == '.')
	{
		i++;
		while (i < len && isdigit((unsigned char) s[i]))
			i++, digits++;
	}
	if (digits == 0)
		return false;
	if (i < len && (s[i] == 'e' || s[i] == 'E'))
	{
		int			edigits = 0;

		i++;
		if (i < len && (s[i] == '+' || s[i] == '-'))
			i++;
		while (i < len && isdigit((unsigned char) s[i]))
			i++, edigits++;
		if (edigits == 0)
			return false;
	}
	return i == len;
}

static pg_noreturn void
malformed(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROTOCOL_VIOLATION),
			 errmsg("Malformed communication packet.")));
}

static uint64
need_uint(StringInfo b, int n)
{
	uint64		v;

	if (!get_uint(b, n, &v))
		malformed();
	return v;
}

static void
append_temporal(StringInfo buf, StringInfo pkt, uint8 type)
{
	int			len = (int) need_uint(pkt, 1);
	char		out[64];

	if (type == MYSQL_TYPE_TIME)
	{
		int			neg = 0,
					hh = 0,
					mi = 0,
					ss = 0;
		long		days = 0,
					usec = 0;

		if (len != 0 && len != 8 && len != 12)
			malformed();
		if (len >= 8)
		{
			neg = (int) need_uint(pkt, 1);
			days = (long) need_uint(pkt, 4);
			hh = (int) need_uint(pkt, 1);
			mi = (int) need_uint(pkt, 1);
			ss = (int) need_uint(pkt, 1);
		}
		if (len == 12)
			usec = (long) need_uint(pkt, 4);
		snprintf(out, sizeof(out), "('%s%02ld:%02d:%02d.%06ld')",
				 neg ? "-" : "", days * 24 + hh, mi, ss, usec);
	}
	else
	{
		int			yy = 0,
					mo = 0,
					dd = 0,
					hh = 0,
					mi = 0,
					ss = 0;
		long		usec = 0;

		if (len != 0 && len != 4 && len != 7 && len != 11)
			malformed();
		if (len >= 4)
		{
			yy = (int) need_uint(pkt, 2);
			mo = (int) need_uint(pkt, 1);
			dd = (int) need_uint(pkt, 1);
		}
		if (len >= 7)
		{
			hh = (int) need_uint(pkt, 1);
			mi = (int) need_uint(pkt, 1);
			ss = (int) need_uint(pkt, 1);
		}
		if (len == 11)
			usec = (long) need_uint(pkt, 4);
		if (type == MYSQL_TYPE_DATE || type == MYSQL_TYPE_NEWDATE)
			snprintf(out, sizeof(out), "('%04d-%02d-%02d')", yy, mo, dd);
		else
			snprintf(out, sizeof(out), "('%04d-%02d-%02d %02d:%02d:%02d.%06ld')",
					 yy, mo, dd, hh, mi, ss, usec);
	}
	appendStringInfoString(buf, out);
}

static void
append_float_literal(StringInfo buf, double v, int digits)
{
	if (!isfinite(v))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Incorrect DOUBLE value: '%g'", v)));
	appendStringInfo(buf, "(%.*g)", digits, v);
}

/* Decode one binary-protocol parameter value and append it as a literal */
static void
append_param(StringInfo buf, StringInfo pkt, uint16 ptype)
{
	uint8		type = ptype & 0xff;
	bool		is_unsigned = (ptype & 0x8000) != 0;
	uint64		v;
	const char *s;

	switch (type)
	{
		case MYSQL_TYPE_NULL:
			appendStringInfoString(buf, "NULL");
			break;
		case MYSQL_TYPE_TINY:
			v = need_uint(pkt, 1);
			appendStringInfo(buf, "(%d)", is_unsigned ? (int) (uint8) v : (int) (int8) v);
			break;
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_YEAR:
			v = need_uint(pkt, 2);
			appendStringInfo(buf, "(%d)", is_unsigned ? (int) (uint16) v : (int) (int16) v);
			break;
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_INT24:
			v = need_uint(pkt, 4);
			if (is_unsigned)
				appendStringInfo(buf, "(%u)", (uint32) v);
			else
				appendStringInfo(buf, "(%d)", (int32) v);
			break;
		case MYSQL_TYPE_LONGLONG:
			v = need_uint(pkt, 8);
			if (is_unsigned)
				appendStringInfo(buf, "(" UINT64_FORMAT ")", v);
			else
				appendStringInfo(buf, "(" INT64_FORMAT ")", (int64) v);
			break;
		case MYSQL_TYPE_FLOAT:
			{
				uint32		u = (uint32) need_uint(pkt, 4);
				float		f;

				memcpy(&f, &u, sizeof(f));
				append_float_literal(buf, f, 9);
				break;
			}
		case MYSQL_TYPE_DOUBLE:
			{
				double		d;

				v = need_uint(pkt, 8);
				memcpy(&d, &v, sizeof(d));
				append_float_literal(buf, d, 17);
				break;
			}
		case MYSQL_TYPE_DATE:
		case MYSQL_TYPE_NEWDATE:
		case MYSQL_TYPE_DATETIME:
		case MYSQL_TYPE_TIMESTAMP:
		case MYSQL_TYPE_TIME:
			append_temporal(buf, pkt, type);
			break;
		case MYSQL_TYPE_DECIMAL:
		case MYSQL_TYPE_NEWDECIMAL:
			if (!get_lenenc_int(pkt, &v) || !get_bytes(pkt, (int) v, &s))
				malformed();
			if (is_decimal_text(s, (int) v))
			{
				appendStringInfoChar(buf, '(');
				appendBinaryStringInfo(buf, s, (int) v);
				appendStringInfoChar(buf, ')');
			}
			else
				append_string_literal(buf, s, (int) v, false);
			break;
		case MYSQL_TYPE_VARCHAR:
		case MYSQL_TYPE_VAR_STRING:
		case MYSQL_TYPE_STRING:
		case MYSQL_TYPE_ENUM:
		case MYSQL_TYPE_SET:
		case MYSQL_TYPE_JSON:
			if (!get_lenenc_int(pkt, &v) || !get_bytes(pkt, (int) v, &s))
				malformed();
			append_string_literal(buf, s, (int) v, false);
			break;
		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB:
		case MYSQL_TYPE_BIT:
		case MYSQL_TYPE_GEOMETRY:
			/* binary character set, as in MySQL */
			if (!get_lenenc_int(pkt, &v) || !get_bytes(pkt, (int) v, &s))
				malformed();
			append_string_literal(buf, s, (int) v, true);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported parameter type %u", type)));
	}
}

static bool
param_type_is_binary(uint16 ptype)
{
	switch (ptype & 0xff)
	{
		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB:
		case MYSQL_TYPE_BIT:
		case MYSQL_TYPE_GEOMETRY:
			return true;
		default:
			return false;
	}
}

/* ----------------------------------------------------------------
 * COM_STMT_PREPARE
 * ----------------------------------------------------------------
 */

static const char *ps_marker_name(Node *stmt);

/* errors that MySQL also reports at PREPARE time */
static bool
prepare_time_error(int sqlerrcode)
{
	switch (sqlerrcode)
	{
		case ERRCODE_SYNTAX_ERROR:
		case ERRCODE_UNDEFINED_TABLE:
		case ERRCODE_UNDEFINED_COLUMN:
		case ERRCODE_AMBIGUOUS_COLUMN:
		case ERRCODE_INVALID_SCHEMA_NAME:
		case ERRCODE_INSUFFICIENT_PRIVILEGE:
		case ERRCODE_FEATURE_NOT_SUPPORTED:
		case ERRCODE_INVALID_PSTATEMENT_DEFINITION:
			return true;
		default:
			return false;
	}
}

/*
 * Analyze the statement with NULL for every marker to find the shape of its
 * result.  Returns false if the statement returns no rows (or the shape
 * cannot be known before the parameter values are).  An error that MySQL
 * would report at PREPARE time is returned in *error; the analysis runs in
 * a subtransaction, so the client's transaction is not affected.
 */
static bool
describe_statement(const char *query, int nparams, const int *locs,
				   TupleDesc *tupdesc, List **tlist, ErrorData **error)
{
	StringInfoData text;
	MysTextMap	map;
	int		   *new_loc = palloc(sizeof(int) * Max(nparams, 1));
	int		   *new_len = palloc(sizeof(int) * Max(nparams, 1));
	int			prev = 0;
	MemoryContext oldcxt = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	MysTextMap *saved_map = mys_text_map;
	bool		result = false;

	initStringInfo(&text);
	for (int i = 0; i < nparams; i++)
	{
		appendBinaryStringInfo(&text, query + prev, locs[i] - prev);
		new_loc[i] = text.len;
		appendStringInfoString(&text, "NULL");
		new_len[i] = 4;
		prev = locs[i] + 1;
	}
	appendStringInfoString(&text, query + prev);

	map.orig = query;
	map.nparams = nparams;
	map.orig_loc = locs;
	map.new_loc = new_loc;
	map.new_len = new_len;

	*tupdesc = NULL;
	*tlist = NIL;
	*error = NULL;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcxt);
	PG_TRY();
	{
		List	   *raw;
		List	   *infos;
		RawStmt    *last = NULL;
		MysStmtInfo *last_info = NULL;
		int			visible = 0;
		ListCell   *lc,
				   *lc2;

		mys_text_map = &map;
		raw = mys_rewrite_raw_stmts(mysql_raw_parser(text.data), text.data, &infos);
		/* EXECUTE rewrites it again and reports them */
		mys_discard_rewrite_warnings();
		forboth(lc, raw, lc2, infos)
		{
			MysStmtInfo *info = (MysStmtInfo *) lfirst(lc2);

			if (ps_marker_name(lfirst_node(RawStmt, lc)->stmt))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PSTATEMENT_DEFINITION),
						 errmsg("This command is not supported in the prepared statement protocol yet")));
			if (info->suppress_response)
				continue;
			visible++;
			last = lfirst_node(RawStmt, lc);
			last_info = info;
		}
		if (visible > 1)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("You have an error in your SQL syntax: a prepared statement cannot contain multiple statements")));

		if (last && !last_info->silent && !last_info->capture_insert_id)
		{
			List	   *qlist = pg_analyze_and_rewrite_fixedparams(last, text.data,
																   NULL, 0, NULL);
			Query	   *q = NULL;

			foreach(lc, qlist)
			{
				if (lfirst_node(Query, lc)->canSetTag)
					q = lfirst_node(Query, lc);
			}
			switch (ChoosePortalStrategy(qlist))
			{
				case PORTAL_ONE_SELECT:
					*tlist = q->targetList;
					*tupdesc = ExecCleanTypeFromTL(q->targetList);
					break;
				case PORTAL_ONE_RETURNING:
				case PORTAL_ONE_MOD_WITH:
					/* ON DUPLICATE KEY UPDATE / REPLACE: hidden RETURNING */
					if (last_info->upsert != MYS_UPSERT_NONE)
						break;
					*tlist = q->returningList;
					*tupdesc = ExecCleanTypeFromTL(q->returningList);
					break;
				case PORTAL_UTIL_SELECT:
					*tupdesc = UtilityTupleDescriptor(q->utilityStmt);
					break;
				case PORTAL_MULTI_QUERY:
					break;
			}
			result = (*tupdesc != NULL);
		}
		mys_text_map = saved_map;

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		mys_text_map = saved_map;
		mys_discard_rewrite_warnings();
		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;

		*tupdesc = NULL;
		*tlist = NIL;
		result = false;
		if (prepare_time_error(edata->sqlerrcode))
			*error = edata;		/* reported by the caller */
		else
			FreeErrorData(edata);	/* depends on the values: EXECUTE time */
	}
	PG_END_TRY();

	return result;
}

static void
send_param_def(void)
{
	StringInfoData b;

	initStringInfo(&b);
	put_lenenc_str(&b, "def", 3);
	put_lenenc_str(&b, "", 0);
	put_lenenc_str(&b, "", 0);
	put_lenenc_str(&b, "", 0);
	put_lenenc_str(&b, "?", 1);
	put_lenenc_str(&b, "", 0);
	put_lenenc_int(&b, 0x0c);
	put_int2(&b, MYS_CHARSET_BINARY);
	put_int4(&b, 0);
	put_int1(&b, MYSQL_TYPE_VAR_STRING);
	put_int2(&b, 0);
	put_int1(&b, 0);
	put_int2(&b, 0);
	mys_send_packet(b.data, b.len);
	pfree(b.data);
}

void
mys_stmt_prepare(StringInfo pkt)
{
	char	   *volatile query = pnstrdup(pkt->data + 1, pkt->len - 1);
	int		   *volatile locs = NULL;
	volatile int nparams = 0;
	TupleDesc	tupdesc = NULL;
	List	   *tlist = NIL;
	int			ncols = 0;
	MysPrepStmt *stmt;
	MemoryContext cxt;
	StringInfoData b;
	uint32		id;
	bool		found;
	ErrorData  *volatile error = NULL;

	init_hash();
	if (prepared_stmt_count() >= MYS_MAX_PREPARED_STMTS)
	{
		mys_send_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
					   psprintf("Can't create more than max_prepared_stmt_count statements (current value: %d)",
								MYS_MAX_PREPARED_STMTS));
		return;
	}

	debug_query_string = query;
	pgstat_report_activity(STATE_RUNNING, query);
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	/* as exec_simple_query: work in MessageContext, not the transaction */
	MemoryContextSwitchTo(MessageContext);
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));
	PushActiveSnapshot(GetTransactionSnapshot());

	/* scanner errors (e.g. unterminated string) are syntax errors */
	PG_TRY();
	{
		int		   *found_locs;

		/* as for COM_QUERY, string literals that are not text become X'hex' */
		query = mys_normalize_query(pkt->data + 1, pkt->len - 1);
		nparams = find_markers(query, &found_locs);
		locs = found_locs;
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(MessageContext);
		error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();

	if (error == NULL)
	{
		ErrorData  *describe_error;

		if (describe_statement(query, nparams, locs, &tupdesc, &tlist, &describe_error))
			ncols = tupdesc->natts;
		error = describe_error;
	}

	if (error)
	{
		/* MySQL: a failed PREPARE leaves the transaction alone */
		PopActiveSnapshot();
		CommitTransactionCommand();
		debug_query_string = NULL;
		mys_send_error(error->sqlerrcode, error->message);
		return;
	}

	/* register */
	do
	{
		id = next_stmt_id++;
		if (next_stmt_id == 0)
			next_stmt_id = 1;
	} while (hash_search(stmt_hash, &id, HASH_FIND, NULL) != NULL);

	cxt = AllocSetContextCreate(TopMemoryContext, "aux_mysql prepared statement",
								ALLOCSET_SMALL_SIZES);
	stmt = hash_search(stmt_hash, &id, HASH_ENTER, &found);
	stmt->id = id;
	stmt->cxt = cxt;
	stmt->query = MemoryContextStrdup(cxt, query);
	stmt->nparams = nparams;
	stmt->param_loc = MemoryContextAlloc(cxt, sizeof(int) * Max(nparams, 1));
	memcpy(stmt->param_loc, locs, sizeof(int) * nparams);
	stmt->param_types = MemoryContextAllocZero(cxt, sizeof(uint16) * Max(nparams, 1));
	stmt->types_bound = false;
	stmt->long_data = MemoryContextAllocZero(cxt, sizeof(StringInfo) * Max(nparams, 1));
	stmt->long_data_bytes = 0;
	stmt->long_data_error = false;

	/* COM_STMT_PREPARE_OK */
	initStringInfo(&b);
	put_int1(&b, 0x00);
	put_int4(&b, id);
	put_int2(&b, (uint16) ncols);
	put_int2(&b, (uint16) nparams);
	put_int1(&b, 0);
	put_int2(&b, mys_warning_count);
	mys_send_packet(b.data, b.len);
	pfree(b.data);

	for (int i = 0; i < nparams; i++)
		send_param_def();
	if (nparams > 0 && !(mys_client_caps & CLIENT_DEPRECATE_EOF))
		mys_send_eof(false);

	if (ncols > 0)
	{
		ListCell   *tlc = list_head(tlist);

		for (int i = 0; i < ncols; i++)
		{
			TargetEntry *tle = NULL;

			while (tlc && lfirst_node(TargetEntry, tlc)->resjunk)
				tlc = lnext(tlist, tlc);
			if (tlc)
			{
				tle = lfirst_node(TargetEntry, tlc);
				tlc = lnext(tlist, tlc);
			}
			mys_send_coldef(TupleDescAttr(tupdesc, i), tle);
		}
		if (!(mys_client_caps & CLIENT_DEPRECATE_EOF))
			mys_send_eof(false);
	}

	PopActiveSnapshot();
	CommitTransactionCommand();
	debug_query_string = NULL;
}

/* ----------------------------------------------------------------
 * COM_STMT_EXECUTE: rewrite the packet into the query to run
 * ----------------------------------------------------------------
 */
void
mys_stmt_execute(StringInfo inBuf)
{
	MysPrepStmt *stmt;
	const char *nullmap = NULL;
	StringInfoData q;
	MysTextMap *map;
	int		   *new_loc;
	int		   *new_len;
	int			prev = 0;
	uint32		id;

	inBuf->cursor = 1;			/* command byte */
	id = (uint32) need_uint(inBuf, 4);
	stmt = lookup_stmt(id, "mysqld_stmt_execute", false);
	if (stmt->long_data_error)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("long data exceeds mysql.max_allowed_packet; reset the statement")));
	(void) need_uint(inBuf, 1); /* flags: cursors are not supported, the
								 * whole result is sent */
	(void) need_uint(inBuf, 4); /* iteration count, always 1 */

	if (stmt->nparams > 0)
	{
		if (!get_bytes(inBuf, (stmt->nparams + 7) / 8, &nullmap))
			malformed();
		if (need_uint(inBuf, 1))
		{
			for (int i = 0; i < stmt->nparams; i++)
				stmt->param_types[i] = (uint16) need_uint(inBuf, 2);
			stmt->types_bound = true;
		}
		else if (!stmt->types_bound)
			malformed();
	}

	new_loc = palloc(sizeof(int) * Max(stmt->nparams, 1));
	new_len = palloc(sizeof(int) * Max(stmt->nparams, 1));
	initStringInfo(&q);
	for (int i = 0; i < stmt->nparams; i++)
	{
		appendBinaryStringInfo(&q, stmt->query + prev, stmt->param_loc[i] - prev);
		new_loc[i] = q.len;
		if (stmt->long_data[i])
			append_string_literal(&q, stmt->long_data[i]->data, stmt->long_data[i]->len,
								  param_type_is_binary(stmt->param_types[i]));
		else if (nullmap[i / 8] & (1 << (i % 8)))
			appendStringInfoString(&q, "NULL");
		else
			append_param(&q, inBuf, stmt->param_types[i]);
		new_len[i] = q.len - new_loc[i];
		prev = stmt->param_loc[i] + 1;
	}
	appendStringInfoString(&q, stmt->query + prev);

	/* long data is consumed by the execution, as in MySQL */
	for (int i = 0; i < stmt->nparams; i++)
	{
		if (stmt->long_data[i])
		{
			pfree(stmt->long_data[i]->data);
			pfree(stmt->long_data[i]);
			stmt->long_data[i] = NULL;
		}
	}

	/* result columns are named after the text with '?' (MySQL's naming) */
	stmt->long_data_bytes = 0;
	map = palloc(sizeof(MysTextMap));
	map->orig = stmt->query;
	map->nparams = stmt->nparams;
	map->orig_loc = stmt->param_loc;
	map->new_loc = new_loc;
	map->new_len = new_len;

	/* hand the query to the regular simple-query path */
	resetStringInfo(inBuf);
	appendBinaryStringInfo(inBuf, q.data, q.len + 1);	/* with the NUL */
	inBuf->cursor = 0;
	pfree(q.data);

	mys_text_map = map;
	mys_binary_resultset = true;
}

/* ----------------------------------------------------------------
 * COM_STMT_CLOSE / RESET / SEND_LONG_DATA
 * ----------------------------------------------------------------
 */
void
mys_stmt_close(StringInfo pkt)
{
	MysPrepStmt *stmt;
	uint64		id;

	pkt->cursor = 1;
	/* no response, also not for an unknown id */
	if (!get_uint(pkt, 4, &id))
		return;
	stmt = lookup_stmt((uint32) id, "mysqld_stmt_close", true);
	if (stmt)
		drop_stmt(stmt);
}

void
mys_stmt_reset(StringInfo pkt)
{
	MysPrepStmt *stmt = NULL;
	uint64		id = 0;

	pkt->cursor = 1;
	if (get_uint(pkt, 4, &id))
		stmt = lookup_stmt((uint32) id, "mysqld_stmt_reset", true);
	if (stmt == NULL)
	{
		mys_send_error(ERRCODE_UNDEFINED_PSTATEMENT,
					   psprintf("Unknown prepared statement handler (%u) given to mysqld_stmt_reset",
								(uint32) id));
		return;
	}
	for (int i = 0; i < stmt->nparams; i++)
	{
		if (stmt->long_data[i])
		{
			pfree(stmt->long_data[i]->data);
			pfree(stmt->long_data[i]);
			stmt->long_data[i] = NULL;
		}
	}
	stmt->long_data_bytes = 0;
	stmt->long_data_error = false;
	mys_send_ok(0, 0, false, false);
}

void
mys_stmt_send_long_data(StringInfo pkt)
{
	MysPrepStmt *stmt;
	uint64		id;
	uint64		param;
	MemoryContext oldcxt;

	pkt->cursor = 1;
	/* no response; MySQL reports problems at EXECUTE time */
	if (!get_uint(pkt, 4, &id) || !get_uint(pkt, 2, &param))
		return;
	stmt = lookup_stmt((uint32) id, "mysqld_stmt_send_long_data", true);
	if (stmt == NULL || param >= (uint64) stmt->nparams)
		return;
	if (stmt->long_data_error ||
		stmt->long_data_bytes + pkt->len - pkt->cursor > (Size) halo_mysql_max_allowed_packet)
	{
		stmt->long_data_error = true;
		return;
	}
	stmt->long_data_bytes += pkt->len - pkt->cursor;

	oldcxt = MemoryContextSwitchTo(stmt->cxt);
	if (stmt->long_data[param] == NULL)
		stmt->long_data[param] = makeStringInfo();
	appendBinaryStringInfo(stmt->long_data[param], pkt->data + pkt->cursor,
						   pkt->len - pkt->cursor);
	MemoryContextSwitchTo(oldcxt);
}

/* ----------------------------------------------------------------
 * SQL PREPARE / EXECUTE / DEALLOCATE PREPARE
 *
 * The grammar builds PostgreSQL's nodes for them, but PostgreSQL's prepared
 * statements hold analyzed queries with typed parameters, while MySQL's hold
 * text whose '?' markers take the values of user variables like literals.
 * So mys_rewrite.c turns them into marker statements that run here.  PREPARE
 * checks the text as COM_STMT_PREPARE does and keeps it by name; EXECUTE
 * renders the values into it, as COM_STMT_EXECUTE does, and has
 * mysql_proto.c run the result as the next query (followed by the rest of
 * the packet), so the client gets the statement's own result.
 * ----------------------------------------------------------------
 */
#define PS_PREPARE_MARKER		"aux_mysql_prepare"
#define PS_EXECUTE_MARKER		"aux_mysql_execute"
#define PS_DEALLOCATE_MARKER	"aux_mysql_deallocate"

typedef struct MysSqlStmt
{
	char		name[NAMEDATALEN];	/* hash key, in lower case */
	MemoryContext cxt;
	char	   *query;			/* text with '?' markers */
	int			nparams;
	int		   *param_loc;		/* byte offset of each marker */
} MysSqlStmt;

/* statement names are case-insensitive in MySQL */
static void
sql_stmt_key(const char *name, char *key)
{
	int			i;

	for (i = 0; name[i] != '\0' && i < NAMEDATALEN - 1; i++)
		key[i] = pg_ascii_tolower((unsigned char) name[i]);
	key[i] = '\0';
}

static MysSqlStmt *
sql_stmt_lookup(const char *name)
{
	char		key[NAMEDATALEN];

	if (sql_stmt_hash == NULL)
		return NULL;
	sql_stmt_key(name, key);
	return hash_search(sql_stmt_hash, key, HASH_FIND, NULL);
}

static bool
sql_stmt_drop(const char *name)
{
	MysSqlStmt *stmt = sql_stmt_lookup(name);
	char		key[NAMEDATALEN];
	MemoryContext cxt;

	if (stmt == NULL)
		return false;
	cxt = stmt->cxt;
	strlcpy(key, stmt->name, NAMEDATALEN);
	hash_search(sql_stmt_hash, key, HASH_REMOVE, NULL);
	MemoryContextDelete(cxt);
	return true;
}

static void
sql_stmt_drop_all(void)
{
	HASH_SEQ_STATUS seq;
	MysSqlStmt *stmt;

	if (sql_stmt_hash == NULL)
		return;
	hash_seq_init(&seq, sql_stmt_hash);
	while ((stmt = hash_seq_search(&seq)) != NULL)
		MemoryContextDelete(stmt->cxt);
	hash_destroy(sql_stmt_hash);
	sql_stmt_hash = NULL;
}

static void
sql_stmt_store(const char *name, const char *query, int nparams, const int *locs)
{
	char		key[NAMEDATALEN];
	MemoryContext cxt;
	char	   *text;
	int		   *loc;
	MysSqlStmt *stmt;
	bool		found;

	if (sql_stmt_hash == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = sizeof(MysSqlStmt);
		ctl.hcxt = TopMemoryContext;
		sql_stmt_hash = hash_create("aux_mysql SQL prepared statements", 16, &ctl,
									HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
	}
	sql_stmt_key(name, key);

	cxt = AllocSetContextCreate(TopMemoryContext, "aux_mysql SQL prepared statement",
								ALLOCSET_SMALL_SIZES);
	text = MemoryContextStrdup(cxt, query);
	loc = MemoryContextAlloc(cxt, sizeof(int) * Max(nparams, 1));
	memcpy(loc, locs, sizeof(int) * nparams);

	stmt = hash_search(sql_stmt_hash, key, HASH_ENTER, &found);
	if (found)
		MemoryContextDelete(stmt->cxt);
	stmt->cxt = cxt;
	stmt->query = text;
	stmt->nparams = nparams;
	stmt->param_loc = loc;
}

/* the marker statement's kind, or NULL if it is no such marker */
static const char *
ps_marker_name(Node *stmt)
{
	const char *name;

	if (!IsA(stmt, DoStmt) || ((DoStmt *) stmt)->args == NIL)
		return NULL;
	name = linitial_node(DefElem, ((DoStmt *) stmt)->args)->defname;
	if (strcmp(name, PS_PREPARE_MARKER) == 0 || strcmp(name, PS_EXECUTE_MARKER) == 0 ||
		strcmp(name, PS_DEALLOCATE_MARKER) == 0)
		return name;
	return NULL;
}

/*
 * The marker statement for a PrepareStmt, ExecuteStmt or DeallocateStmt of
 * the MySQL grammar (other statements are returned as they are).  rest is
 * the text of the packet after an EXECUTE, or NULL.
 */
Node *
mys_sql_ps_marker(Node *stmt, const char *rest)
{
	DoStmt	   *d = makeNode(DoStmt);

	switch ((int) nodeTag(stmt))
	{
		case T_PrepareStmt:
			{
				PrepareStmt *p = (PrepareStmt *) stmt;
				DefElem    *src;

				/* FROM 'text', or FROM @var: its value when PREPARE runs */
				if (IsA(p->query, String))
					src = makeDefElem("text", p->query, -1);
				else if ((int) nodeTag(p->query) == T_UserVarRef)
					src = makeDefElem("uservar",
									  (Node *) makeString(((UserVarRef *) p->query)->userVarName), -1);
				else
					return stmt;
				d->args = list_make2(makeDefElem(PS_PREPARE_MARKER,
												 (Node *) makeString(p->name), -1),
									 src);
				break;
			}
		case T_ExecuteStmt:
			{
				ExecuteStmt *e = (ExecuteStmt *) stmt;

				d->args = list_make2(makeDefElem(PS_EXECUTE_MARKER,
												 (Node *) makeString(e->name), -1),
									 makeDefElem("using", (Node *) e->params, -1));
				if (rest)
					d->args = lappend(d->args,
									  makeDefElem("rest", (Node *) makeString(pstrdup(rest)), -1));
				break;
			}
		case T_DeallocateStmt:
			{
				DeallocateStmt *x = (DeallocateStmt *) stmt;

				/* no name: DEALLOCATE ALL (PostgreSQL syntax) */
				d->args = list_make1(makeDefElem(PS_DEALLOCATE_MARKER,
												 x->name ? (Node *) makeString(x->name) : NULL,
												 -1));
				break;
			}
		default:
			return stmt;
	}
	return (Node *) d;
}

static void
sql_prepare(DoStmt *d)
{
	const char *name = strVal(linitial_node(DefElem, d->args)->arg);
	DefElem    *src = lsecond_node(DefElem, d->args);
	const char *text;
	int			len;
	char	   *query;
	int		   *locs;
	int			nparams;
	TupleDesc	tupdesc;
	List	   *tlist;
	ErrorData  *error;

	if (strcmp(src->defname, "uservar") == 0)
	{
		bytea	   *value = mysGetUserVarValueInternal(strVal(src->arg));

		/* MySQL takes an unset variable for the text NULL: a syntax error */
		text = value ? VARDATA_ANY(value) : "NULL";
		len = value ? VARSIZE_ANY_EXHDR(value) : 4;
	}
	else
	{
		text = strVal(src->arg);
		len = strlen(text);
	}

	/* as in MySQL, a statement of that name is gone even if this one fails */
	sql_stmt_drop(name);
	if (prepared_stmt_count() >= MYS_MAX_PREPARED_STMTS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("Can't create more than max_prepared_stmt_count statements (current value: %d)",
						MYS_MAX_PREPARED_STMTS)));

	query = mys_normalize_query(text, len);
	nparams = find_markers(query, &locs);
	(void) describe_statement(query, nparams, locs, &tupdesc, &tlist, &error);
	if (error)
		ReThrowError(error);
	sql_stmt_store(name, query, nparams, locs);
}

/* the name of an @variable as the grammar emits it, mysql.get_user_var('name') */
static const char *
user_var_name(Node *n)
{
	FuncCall   *fc = (FuncCall *) n;
	A_Const    *arg;

	if (!IsA(n, FuncCall) || list_length(fc->funcname) != 2 || list_length(fc->args) != 1 ||
		strcmp(strVal(linitial(fc->funcname)), "mysql") != 0 ||
		strcmp(strVal(lsecond(fc->funcname)), "get_user_var") != 0 ||
		!IsA(linitial(fc->args), A_Const))
		return NULL;
	arg = linitial_node(A_Const, fc->args);
	if (arg->isnull || !IsA(&arg->val, String))
		return NULL;
	return strVal(&arg->val);
}

static void
append_number_or_string(StringInfo buf, const char *s, int len)
{
	if (is_decimal_text(s, len))
	{
		appendStringInfoChar(buf, '(');
		appendBinaryStringInfo(buf, s, len);
		appendStringInfoChar(buf, ')');
	}
	else
		append_string_literal(buf, s, len, false);
}

/*
 * One EXECUTE ... USING value as a literal.  An @variable keeps its kind,
 * number or string, as in MySQL.  MySQL accepts nothing else; any other
 * expression is evaluated and rendered by its type.
 */
static void
append_using_value(StringInfo buf, Node *arg, ParseState *pstate)
{
	const char *var = user_var_name(arg);
	Node	   *expr;
	Oid			type;
	EState	   *estate;
	ExprState  *exprstate;
	Datum		value;
	bool		isnull;

	if (var)
	{
		bytea	   *v = mysGetUserVarValueInternal((char *) var);

		if (v == NULL)
			appendStringInfoString(buf, "NULL");
		else if (varValueIsDigit(PointerGetDatum(v), BYTEAOID))
			append_number_or_string(buf, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
		else
			append_string_literal(buf, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), false);
		return;
	}

	expr = transformExpr(pstate, arg, EXPR_KIND_EXECUTE_PARAMETER);
	assign_expr_collations(pstate, expr);
	type = getBaseType(exprType(expr));
	estate = CreateExecutorState();
	exprstate = ExecPrepareExpr((Expr *) expr, estate);
	value = ExecEvalExprSwitchContext(exprstate, GetPerTupleExprContext(estate), &isnull);
	if (isnull)
		appendStringInfoString(buf, "NULL");
	else if (type == BYTEAOID)
	{
		bytea	   *b = DatumGetByteaPP(value);

		append_string_literal(buf, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), true);
	}
	else if (type == BOOLOID)
		appendStringInfoString(buf, DatumGetBool(value) ? "(1)" : "(0)");
	else
	{
		Oid			output;
		bool		varlena;
		char		category;
		bool		preferred;
		char	   *s;

		getTypeOutputInfo(exprType(expr), &output, &varlena);
		s = OidOutputFunctionCall(output, value);
		get_type_category_preferred(type, &category, &preferred);
		if (category == TYPCATEGORY_NUMERIC)
			append_number_or_string(buf, s, strlen(s));
		else
			append_string_literal(buf, s, strlen(s), false);
	}
	FreeExecutorState(estate);
}

static void
sql_execute(DoStmt *d, const char *queryString)
{
	const char *name = strVal(linitial_node(DefElem, d->args)->arg);
	List	   *args = (List *) lsecond_node(DefElem, d->args)->arg;
	DefElem    *rest = list_length(d->args) > 2 ? lthird_node(DefElem, d->args) : NULL;
	MysSqlStmt *stmt = sql_stmt_lookup(name);
	ParseState *pstate;
	StringInfoData q;
	MysTextMap	map;
	int		   *new_loc;
	int		   *new_len;
	int			prev = 0;

	if (stmt == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("Unknown prepared statement handler (%s) given to EXECUTE", name)));
	if (list_length(args) != stmt->nparams)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PARAMETER),
				 errmsg("Incorrect arguments to EXECUTE")));

	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	new_loc = palloc(sizeof(int) * Max(stmt->nparams, 1));
	new_len = palloc(sizeof(int) * Max(stmt->nparams, 1));
	initStringInfo(&q);
	for (int i = 0; i < stmt->nparams; i++)
	{
		appendBinaryStringInfo(&q, stmt->query + prev, stmt->param_loc[i] - prev);
		new_loc[i] = q.len;
		append_using_value(&q, list_nth(args, i), pstate);
		new_len[i] = q.len - new_loc[i];
		prev = stmt->param_loc[i] + 1;
	}
	appendStringInfoString(&q, stmt->query + prev);
	free_parsestate(pstate);

	/* result columns are named after the text with '?', as in MySQL */
	map.orig = stmt->query;
	map.nparams = stmt->nparams;
	map.orig_loc = stmt->param_loc;
	map.new_loc = new_loc;
	map.new_len = new_len;
	mys_queue_query(q.data, &map, rest ? strVal(rest->arg) : NULL);
}

static void
sql_deallocate(DoStmt *d)
{
	Node	   *name = linitial_node(DefElem, d->args)->arg;

	if (name == NULL)
		sql_stmt_drop_all();
	else if (!sql_stmt_drop(strVal(name)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_PSTATEMENT),
				 errmsg("Unknown prepared statement handler (%s) given to DEALLOCATE PREPARE",
						strVal(name))));
}

/* ProcessUtility: run a marker statement of mys_sql_ps_marker() */
bool
mys_sql_ps_utility(Node *parsetree, const char *queryString)
{
	const char *marker = ps_marker_name(parsetree);

	if (marker == NULL)
		return false;
	if (strcmp(marker, PS_PREPARE_MARKER) == 0)
		sql_prepare((DoStmt *) parsetree);
	else if (strcmp(marker, PS_EXECUTE_MARKER) == 0)
		sql_execute((DoStmt *) parsetree, queryString);
	else
		sql_deallocate((DoStmt *) parsetree);
	return true;
}
