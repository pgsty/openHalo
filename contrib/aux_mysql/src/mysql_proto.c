/*-------------------------------------------------------------------------
 *
 * mysql_proto.c
 *	  MySQL client/server protocol on top of the PostgreSQL 18 protocol
 *	  extension hooks.
 *
 * Connection life cycle:
 *
 *	postmaster accepts on mysql.port
 *	  -> mys_start()           greeting + HandshakeResponse41 (pre-auth)
 *	  -> mys_authenticate()    pg_hba.conf + password check (InitPostgres)
 *	  -> first ReadyForQuery   select initial database, send OK
 *	  -> mys_read_command()    COM_QUERY becomes a regular simple query,
 *	                           other COM_* are handled by mys_process_command
 *	  -> parse_query hook      MySQL (or PG) raw parser
 *	  -> DestReceiver          text protocol result sets
 *	  -> end_command hook      OK / EOF packets
 *	  -> send_message hook     ERR packets
 *
 * Packet layout and type mapping follow openHalo's adapter (GPL-3.0),
 * rewritten for the PG18 hook interface.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/printtup.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/discard.h"
#include "funcapi.h"
#include "common/string.h"
#include "libpq/auth.h"
#include "libpq/crypt.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parser.h"
#include "parser/scansup.h"
#include "postmaster/postmaster.h"
#include "postmaster/protocol_extension.h"
#include "storage/ipc.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#include "halo_mysql.h"
#include "mys_fk.h"
#include "mys_proto.h"
#include "adapter/mysql/adapter.h"
#include "adapter/mysql/systemVar.h"
#include "commands/mysql/mys_uservar.h"

/* ----------------------------------------------------------------
 * Per-connection state
 * ----------------------------------------------------------------
 */
static StringInfoData outbuf;		/* pending output, TopMemoryContext */
static uint8 mys_seq = 0;			/* next sequence id to send */
uint32		mys_client_caps = 0;
static char scramble[MYS_SCRAMBLE_LEN + 1];
static char *initial_db = NULL;		/* database from HandshakeResponse */
static char *client_plugin = NULL;
static char *client_auth_resp = NULL;
static int	client_auth_resp_len = 0;
static bool handshake_done = false;
static bool resultset_active = false;
uint16		mys_warning_count = 0;

#define MYS_MAX_DIAGNOSTICS 64
static struct
{
	bool error;
	uint16 code;
	char *message;
} diagnostics[MYS_MAX_DIAGNOSTICS];
static int ndiagnostics = 0;
static MemoryContext diagnostics_context = NULL;
static bool diagnostics_pending = false;

static void
mys_clear_diagnostics(void)
{
	if (diagnostics_context)
		MemoryContextReset(diagnostics_context);
	ndiagnostics = 0;
	mys_warning_count = 0;
	diagnostics_pending = false;
}

static void
mys_record_diagnostic(bool error, uint16 code, const char *message)
{
	if (diagnostics_pending)
		mys_clear_diagnostics();
	if (!diagnostics_context)
		diagnostics_context = AllocSetContextCreate(TopMemoryContext,
												   "MySQL diagnostics", ALLOCSET_SMALL_SIZES);
	if (mys_warning_count < PG_UINT16_MAX)
		mys_warning_count++;
	if (ndiagnostics < MYS_MAX_DIAGNOSTICS)
	{
		diagnostics[ndiagnostics].error = error;
		diagnostics[ndiagnostics].code = code;
		diagnostics[ndiagnostics].message = MemoryContextStrdup(diagnostics_context, message);
		ndiagnostics++;
	}
}

PG_FUNCTION_INFO_V1(mys_show_diagnostics);
Datum
mys_show_diagnostics(PG_FUNCTION_ARGS)
{
	bool errors_only = PG_GETARG_BOOL(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	for (int i = 0; i < ndiagnostics; i++)
	{
		Datum values[3];
		bool nulls[3] = {false, false, false};

		if (errors_only && !diagnostics[i].error)
			continue;
		values[0] = CStringGetTextDatum(diagnostics[i].error ? "Error" : "Warning");
		values[1] = Int32GetDatum(diagnostics[i].code);
		values[2] = CStringGetTextDatum(diagnostics[i].message);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

#ifdef HALO_BABELFISH_API
static List *bf_receivers = NIL;	/* result receiver state (MysDR) */
#endif

/* statements of the current COM_QUERY (see mys_rewrite.c) */
static List *stmt_infos = NIL;
static int	stmt_index = 0;
static uint64 cur_insert_id = 0;	/* last_insert_id for the next OK packet */
static bool com_query = false;		/* the command is COM_QUERY */
static bool trailing_comment_ok = false;	/* an OK follows the last statement */
static bool trailing_ok_deferred = false;	/* ... of the last queued part */
static bool last_queued_part = false;	/* running the last queued query */
static uint64 upsert_deleted;
static uint64 upsert_rows[MYS_ROW_UNCHANGED + 1];	/* per MysUpsertRow */

static MysStmtInfo *
current_stmt_info(void)
{
	static MysStmtInfo plain = {false, false, false};

	if (stmt_index < list_length(stmt_infos))
		return (MysStmtInfo *) list_nth(stmt_infos, stmt_index);
	return &plain;
}

/*
 * The MySQL statement being executed, or NULL (native PG connection, or
 * outside a COM_QUERY).
 */
MysStmtInfo *
mys_current_stmt_info(void)
{
	if (!mys_is_mysql_session() ||
		stmt_index >= list_length(stmt_infos))
		return NULL;
	return (MysStmtInfo *) list_nth(stmt_infos, stmt_index);
}

/* a warning that is counted in the OK / EOF packet, without a log line */
void
mys_add_warning(uint16 code, const char *message)
{
	if (mys_is_mysql_session())
		mys_record_diagnostic(false, code, message);
}

/*
 * Queries that run before the next command is read, as parts of the current
 * one: the statement of an EXECUTE, then the rest of its packet (mys_stmt.c).
 * An error ends the packet and drops them.
 */
typedef struct MysQueuedQuery
{
	char	   *query;
	MysTextMap *map;			/* EXECUTE: names columns after the text with '?' */
} MysQueuedQuery;

static List *queued_queries = NIL;
static MemoryContext queue_context = NULL;

static MysTextMap *
copy_text_map(const MysTextMap *map)
{
	MysTextMap *copy = palloc(sizeof(MysTextMap));
	int		   *locs = palloc(sizeof(int) * 3 * Max(map->nparams, 1));

	memcpy(locs, map->orig_loc, sizeof(int) * map->nparams);
	memcpy(locs + map->nparams, map->new_loc, sizeof(int) * map->nparams);
	memcpy(locs + 2 * map->nparams, map->new_len, sizeof(int) * map->nparams);
	copy->orig = pstrdup(map->orig);
	copy->nparams = map->nparams;
	copy->orig_loc = locs;
	copy->new_loc = locs + map->nparams;
	copy->new_len = locs + 2 * map->nparams;
	return copy;
}

/* run query (then rest, if any) before anything queued so far */
void
mys_queue_query(const char *query, const MysTextMap *map, const char *rest)
{
	MemoryContext oldcxt;
	MysQueuedQuery *q;
	List	   *add;

	if (queue_context == NULL)
		queue_context = AllocSetContextCreate(TopMemoryContext, "aux_mysql queued queries",
											  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(queue_context);
	q = palloc0(sizeof(MysQueuedQuery));
	q->query = pstrdup(query);
	if (map)
		q->map = copy_text_map(map);
	add = list_make1(q);
	if (rest)
	{
		q = palloc0(sizeof(MysQueuedQuery));
		q->query = pstrdup(rest);
		add = lappend(add, q);
	}
	queued_queries = list_concat(add, queued_queries);
	MemoryContextSwitchTo(oldcxt);
}

static void
discard_queued_queries(void)
{
	queued_queries = NIL;
	trailing_ok_deferred = false;
	if (queue_context)
		MemoryContextReset(queue_context);
}

/* does a later statement of this COM_QUERY send its own response? */
static bool
more_visible_results(void)
{
	for (int i = stmt_index + 1; i < list_length(stmt_infos); i++)
	{
		if (!((MysStmtInfo *) list_nth(stmt_infos, i))->suppress_response)
			return true;
	}
	return trailing_comment_ok || queued_queries != NIL;
}

/*
 * MySQL first strips trailing ';' and spaces from a COM_QUERY.  When what
 * then follows the last ';' is only comments ("select 1; -- done"), it runs
 * that as an empty statement and answers it with an OK of its own, if the
 * client enabled multiple statements.  The grammar has dropped the empty
 * statements, so look at the text after the last statement: the parse left
 * only ';', space and comments there.
 */
static bool
has_trailing_comment(List *raw, const char *query_string)
{
	RawStmt    *last;
	const char *p;

	if (!com_query || !(mys_client_caps & CLIENT_MULTI_STATEMENTS) || raw == NIL)
		return false;
	last = llast_node(RawStmt, raw);
	/* stmt_len 0: the statement runs to the end of the text */
	if (last->stmt_len <= 0)
		return false;
	p = query_string + last->stmt_location + last->stmt_len;
	if (*p != ';')
		return false;
	for (; *p; p++)
	{
		if (*p != ';' && !scanner_isspace(*p))
			return true;
	}
	return false;
}

/* ----------------------------------------------------------------
 * Low-level packet I/O
 * ----------------------------------------------------------------
 */
void
mys_flush(void)
{
	char	   *p = outbuf.data;
	int			left = outbuf.len;

	while (left > 0)
	{
		ssize_t		r = secure_write(MyProcPort, p, left);

		if (r <= 0)
		{
			if (errno == EINTR)
				continue;
			/* same treatment as internal_flush() */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("could not send data to client: %m")));
			ClientConnectionLost = 1;
			InterruptPending = 1;
			break;
		}
		p += r;
		left -= r;
	}
	resetStringInfo(&outbuf);
}

/* Append one logical packet (payload) to the output buffer */
void
mys_send_packet(const char *payload, int len)
{
	do
	{
		int			chunk = Min(len, MYS_MAX_PACKET);
		char		hdr[4];

		hdr[0] = chunk & 0xff;
		hdr[1] = (chunk >> 8) & 0xff;
		hdr[2] = (chunk >> 16) & 0xff;
		hdr[3] = mys_seq++;
		appendBinaryStringInfo(&outbuf, hdr, 4);
		appendBinaryStringInfo(&outbuf, payload, chunk);
		payload += chunk;
		len -= chunk;
		/* a payload of exactly N*0xffffff bytes ends with an empty packet */
		if (chunk == MYS_MAX_PACKET && len == 0)
		{
			hdr[0] = hdr[1] = hdr[2] = 0;
			hdr[3] = mys_seq++;
			appendBinaryStringInfo(&outbuf, hdr, 4);
		}
	} while (len > 0);

	if (outbuf.len > 65536)
		mys_flush();
}

/* Read one logical packet into buf.  Returns 0, or EOF on disconnect. */
static int
mys_read_packet(StringInfo buf)
{
	unsigned char hdr[4];
	int			len;

	resetStringInfo(buf);
	pq_startmsgread();
	for (;;)
	{
		if (pq_getbytes((char *) hdr, 4) == EOF)
			return EOF;
		len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16);
		mys_seq = hdr[3] + 1;
		if ((Size) buf->len + len > (Size) halo_mysql_max_allowed_packet ||
			(Size) buf->len + len >= MaxAllocSize - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("MySQL packet exceeds mysql.max_allowed_packet")));
		enlargeStringInfo(buf, len);
		if (len > 0 && pq_getbytes(buf->data + buf->len, len) == EOF)
			return EOF;
		buf->len += len;
		buf->data[buf->len] = '\0';
		if (len < MYS_MAX_PACKET)
			break;
	}
	pq_endmsgread();
	buf->cursor = 0;
	return 0;
}

/* ----------------------------------------------------------------
 * Payload builders
 * ----------------------------------------------------------------
 */
/* sysvar_core.c: @@autocommit, 0 = off */
extern int	autoCommit;

static uint16
mys_status_flags(bool more_results)
{
	uint16		status = autoCommit ? SERVER_STATUS_AUTOCOMMIT : 0;

	if (mys_transaction_active())
	{
		status |= SERVER_STATUS_IN_TRANS;
		if (XactReadOnly)
			status |= SERVER_STATUS_IN_TRANS_READONLY;
	}
	if (more_results)
		status |= SERVER_MORE_RESULTS_EXISTS;
	if (mys_sqlMode & MYS_MODE_NO_BACKSLASH_ESCAPES)
		status |= SERVER_STATUS_NO_BACKSLASH_ESCAPES;
	return status;
}

void
mys_send_ok(uint64 affected, uint64 last_insert_id, bool eof_marker,
			bool more_results)
{
	StringInfoData b;

	initStringInfo(&b);
	put_int1(&b, eof_marker ? 0xfe : 0x00);
	put_lenenc_int(&b, affected);
	put_lenenc_int(&b, last_insert_id);
	put_int2(&b, mys_status_flags(more_results));
	put_int2(&b, mys_warning_count);
	mys_send_packet(b.data, b.len);
	pfree(b.data);
}

void
mys_send_eof(bool more_results)
{
	StringInfoData b;

	initStringInfo(&b);
	put_int1(&b, 0xfe);
	put_int2(&b, mys_warning_count);
	put_int2(&b, mys_status_flags(more_results));
	mys_send_packet(b.data, b.len);
	pfree(b.data);
}

/* End of a result set: EOF packet, or OK-with-0xFE when EOF is deprecated */
void
mys_send_resultset_end(bool more_results)
{
	if (mys_client_caps & CLIENT_DEPRECATE_EOF)
		mys_send_ok(0, 0, true, more_results);
	else
		mys_send_eof(more_results);
}

void
mys_send_err(uint16 code, const char *sqlstate, const char *msg)
{
	StringInfoData b;

	mys_record_diagnostic(true, code, msg);
	/* nothing of the command runs after its error, as in MySQL */
	discard_queued_queries();

	initStringInfo(&b);
	put_int1(&b, 0xff);
	put_int2(&b, code);
	if (mys_client_caps & CLIENT_PROTOCOL_41 || !handshake_done)
	{
		put_int1(&b, '#');
		appendBinaryStringInfo(&b, sqlstate, 5);
	}
	appendStringInfoString(&b, msg);
	mys_send_packet(b.data, b.len);
	pfree(b.data);
}

/* ----------------------------------------------------------------
 * SQLSTATE -> MySQL error number
 * ----------------------------------------------------------------
 */
uint16
mys_errno(int sqlerrcode)
{
	/* a MySQL error number of its own (halo_mysql.h) */
	if (PGUNSIXBIT(sqlerrcode) == 'M')
		return (uint16) atoi(unpack_sql_state(sqlerrcode) + 1);

	switch (sqlerrcode)
	{
		case ERRCODE_SYNTAX_ERROR:
			return 1064;		/* ER_PARSE_ERROR */
		case ERRCODE_UNDEFINED_TABLE:
			return 1146;		/* ER_NO_SUCH_TABLE */
		case ERRCODE_UNDEFINED_COLUMN:
			return 1054;		/* ER_BAD_FIELD_ERROR */
		case ERRCODE_AMBIGUOUS_COLUMN:
			return 1052;		/* ER_NON_UNIQ_ERROR */
		case ERRCODE_DUPLICATE_TABLE:
			return 1050;		/* ER_TABLE_EXISTS_ERROR */
		case ERRCODE_DUPLICATE_COLUMN:
			return 1060;		/* ER_DUP_FIELDNAME */
		case ERRCODE_DUPLICATE_SCHEMA:
			return 1007;		/* ER_DB_CREATE_EXISTS */
		case ERRCODE_INVALID_SCHEMA_NAME:
		case ERRCODE_INVALID_CATALOG_NAME:
			return 1049;		/* ER_BAD_DB_ERROR */
		case ERRCODE_UNIQUE_VIOLATION:
			return 1062;		/* ER_DUP_ENTRY */
		case ERRCODE_NOT_NULL_VIOLATION:
			return 1048;		/* ER_BAD_NULL_ERROR */
		case ERRCODE_FOREIGN_KEY_VIOLATION:
			return 1452;		/* ER_NO_REFERENCED_ROW_2 */
		case ERRCODE_RESTRICT_VIOLATION:
			return 1451;		/* ER_ROW_IS_REFERENCED_2: ON DELETE/UPDATE RESTRICT */
		case ERRCODE_CHECK_VIOLATION:
			return 3819;		/* ER_CHECK_CONSTRAINT_VIOLATED */
		case ERRCODE_STRING_DATA_RIGHT_TRUNCATION:
			return 1406;		/* ER_DATA_TOO_LONG */
		case ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE:
			return 1264;		/* ER_WARN_DATA_OUT_OF_RANGE */
		case ERRCODE_INVALID_TEXT_REPRESENTATION:
		case ERRCODE_CHARACTER_NOT_IN_REPERTOIRE:	/* bytes that are not valid text */
			return 1366;		/* ER_TRUNCATED_WRONG_VALUE_FOR_FIELD */
		case ERRCODE_INVALID_DATETIME_FORMAT:
		case ERRCODE_DATETIME_FIELD_OVERFLOW:
			return 1292;		/* ER_TRUNCATED_WRONG_VALUE */
		case ERRCODE_DIVISION_BY_ZERO:
			return 1365;		/* ER_DIVISION_BY_ZERO */
		case ERRCODE_UNDEFINED_FUNCTION:
			return 1305;		/* ER_SP_DOES_NOT_EXIST */
		case ERRCODE_INSUFFICIENT_PRIVILEGE:
			return 1142;		/* ER_TABLEACCESS_DENIED_ERROR */
		case ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION:
		case ERRCODE_INVALID_PASSWORD:
			return 1045;		/* ER_ACCESS_DENIED_ERROR */
		case ERRCODE_T_R_DEADLOCK_DETECTED:
		case ERRCODE_T_R_SERIALIZATION_FAILURE:
			return 1213;		/* ER_LOCK_DEADLOCK, SQLSTATE 40001: retry */
		case ERRCODE_READ_ONLY_SQL_TRANSACTION:
			return mys_server_read_only ?
				1290 :			/* ER_OPTION_PREVENTS_STATEMENT (--read-only) */
				1792;			/* ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION */
		case ERRCODE_LOCK_NOT_AVAILABLE:
			return 1205;		/* ER_LOCK_WAIT_TIMEOUT */
		case ERRCODE_QUERY_CANCELED:
			return 1317;		/* ER_QUERY_INTERRUPTED */
		case ERRCODE_FEATURE_NOT_SUPPORTED:
			return 1235;		/* ER_NOT_SUPPORTED_YET */
		case ERRCODE_GENERATED_ALWAYS:
			return 3105;		/* ER_NON_DEFAULT_VALUE_FOR_GENERATED_COLUMN */
		case ERRCODE_TOO_MANY_CONNECTIONS:
			return 1040;		/* ER_CON_COUNT_ERROR */
		case ERRCODE_UNDEFINED_OBJECT:
			return 1091;		/* ER_CANT_DROP_FIELD_OR_KEY */
		case ERRCODE_UNDEFINED_PSTATEMENT:
			return 1243;		/* ER_UNKNOWN_STMT_HANDLER */
		case ERRCODE_UNDEFINED_PARAMETER:
			return 1210;		/* ER_WRONG_ARGUMENTS: EXECUTE ... USING */
		case ERRCODE_INVALID_PSTATEMENT_DEFINITION:
			return 1295;		/* ER_UNSUPPORTED_PS */
		case ERRCODE_PROTOCOL_VIOLATION:
			return 1835;		/* ER_MALFORMED_PACKET */
		case ERRCODE_INVALID_CURSOR_STATE:
			return 1421;		/* ER_STMT_HAS_NO_OPEN_CURSOR */
		case ERRCODE_MYS_USER_LOCK_WRONG_NAME:
			return 3057;		/* ER_USER_LOCK_WRONG_NAME */
		case ERRCODE_MYS_USER_LOCK_DEADLOCK:
			return 3058;		/* ER_USER_LOCK_DEADLOCK */
		default:
			return 1105;		/* ER_UNKNOWN_ERROR */
	}
}

/*
 * SQLSTATE of the ERR packet: MySQL's own for the aux_mysql-only codes, 42000
 * for a MySQL error number (halo_mysql.h)
 */
static const char *
mys_sqlstate(int sqlerrcode)
{
	switch (sqlerrcode)
	{
		case ERRCODE_MYS_USER_LOCK_WRONG_NAME:
			return "42000";
		case ERRCODE_MYS_USER_LOCK_DEADLOCK:
			return "40001";
		case MYS_ERRCODE_TXN_CHARACTERISTICS:
			return "25001";
		default:
			return PGUNSIXBIT(sqlerrcode) == 'M' ? "42000" : unpack_sql_state(sqlerrcode);
	}
}

/*
 * ERR packet for an error that is not an ereport(ERROR): protocol-level
 * failures must not abort the client's transaction (MySQL keeps it).
 */
void
mys_send_error(int sqlerrcode, const char *msg)
{
	mys_send_err(mys_errno(sqlerrcode), mys_sqlstate(sqlerrcode), msg);
}

#ifdef HALO_BABELFISH_API
static int	mys_bf_listen(const char *host, unsigned short port);
#endif

/* ----------------------------------------------------------------
 * Postmaster: listen sockets
 * ----------------------------------------------------------------
 */
void
mysql_listen_init(void)
{
	const char *addrs;
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *lc;
	int			success = 0;

	addrs = halo_mysql_listen_addresses ? halo_mysql_listen_addresses : ListenAddresses;
	if (!halo_mysql_listener_on || halo_mysql_port == 0 || addrs == NULL || addrs[0] == '\0')
		return;

	/* RSA key pair for caching_sha2_password over non-TLS connections */
	mys_auth_postmaster_init();

	rawstring = pstrdup(addrs);
	if (!SplitGUCList(rawstring, ',', &elemlist))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax in parameter \"%s\"",
						"mysql.listen_addresses")));

	foreach(lc, elemlist)
	{
		char	   *host = (char *) lfirst(lc);
		int			status;

#ifdef HALO_BABELFISH_API
		status = mys_bf_listen(strcmp(host, "*") == 0 ? NULL : host,
							   (unsigned short) halo_mysql_port);
#else
		status = ListenServerPortForProtocol(halo_mysql_protocol_id, AF_UNSPEC,
											 strcmp(host, "*") == 0 ? NULL : host,
											 (unsigned short) halo_mysql_port,
											 NULL);
#endif
		if (status == STATUS_OK)
			success++;
		else
			ereport(WARNING,
					(errmsg("aux_mysql: could not create listen socket for \"%s\"",
							host)));
	}

	if (success > 0)
		ereport(LOG,
				(errmsg("aux_mysql: MySQL protocol listening on port %d",
						halo_mysql_port)));
	list_free(elemlist);
	pfree(rawstring);
}

/* ----------------------------------------------------------------
 * Handshake (before InitProcess, no shared memory yet)
 * ----------------------------------------------------------------
 */
static void
mys_make_scramble(void)
{
	uint8		raw[MYS_SCRAMBLE_LEN];

	if (!pg_strong_random(raw, sizeof(raw)))
		ereport(FATAL,
				(errmsg("could not generate random scramble")));
	/* printable, non-NUL, like the MySQL server does */
	for (int i = 0; i < MYS_SCRAMBLE_LEN; i++)
		scramble[i] = (char) ((raw[i] % 94) + 33);
	scramble[MYS_SCRAMBLE_LEN] = '\0';
}

/* Can this connection be upgraded to TLS?  (PG's ssl = on) */
static bool
mys_ssl_available(Port *port)
{
#ifdef USE_SSL
	return LoadedSSL && port->laddr.addr.ss_family != AF_UNIX;
#else
	return false;
#endif
}

static void
mys_send_greeting(Port *port)
{
	StringInfoData b;
	uint32		caps = SERVER_CAPABILITIES;

	if (mys_ssl_available(port))
		caps |= CLIENT_SSL;

	initStringInfo(&b);
	put_int1(&b, 10);			/* protocol version */
	appendStringInfoString(&b, halo_mysql_version);
	put_int1(&b, 0);
	put_int4(&b, (uint32) MyProcPid);	/* connection id = backend pid */
	appendBinaryStringInfo(&b, scramble, 8);
	put_int1(&b, 0);			/* filler */
	put_int2(&b, caps & 0xffff);
	/* server default collation follows the reported version (openHalo b223ca4) */
	put_int1(&b, strncmp(halo_mysql_version, "8.0", 3) < 0 ?
			 MYS_CHARSET_UTF8MB4_GENERAL : MYS_CHARSET_UTF8MB4);
	put_int2(&b, SERVER_STATUS_AUTOCOMMIT);
	put_int2(&b, (caps >> 16) & 0xffff);
	put_int1(&b, MYS_SCRAMBLE_LEN + 1);
	appendBinaryStringInfo(&b, "\0\0\0\0\0\0\0\0\0\0", 10);
	appendBinaryStringInfo(&b, scramble + 8, MYS_SCRAMBLE_LEN - 8);
	put_int1(&b, 0);
	appendStringInfoString(&b, halo_mysql_auth_plugin == MYS_AUTH_CACHING_SHA2 ?
						   AUTH_SHA2 : AUTH_NATIVE);
	put_int1(&b, 0);

	mys_seq = 0;
	mys_send_packet(b.data, b.len);
	mys_flush();
	pfree(b.data);
}

static char *
get_cstring(StringInfo b)
{
	char	   *s = b->data + b->cursor;
	int			n = strnlen(s, b->len - b->cursor);

	if (b->cursor + n >= b->len && b->data[b->len] != '\0')
		return NULL;
	b->cursor += Min(n + 1, b->len - b->cursor);
	return pnstrdup(s, n);
}

static void
mys_parse_connect_attrs(StringInfo b, Port *port)
{
	uint64		total;
	int			end;

	if (!get_lenenc_int(b, &total))
		return;
	end = Min(b->cursor + (int) total, b->len);
	while (b->cursor < end)
	{
		uint64		klen,
					vlen;
		const char *k,
				   *v;

		if (!get_lenenc_int(b, &klen) || !get_bytes(b, (int) klen, &k) ||
			!get_lenenc_int(b, &vlen) || !get_bytes(b, (int) vlen, &v))
			break;
		if (klen == 12 && strncmp(k, "program_name", 12) == 0)
		{
			char	   *appname = pnstrdup(v, Min(vlen, NAMEDATALEN - 1));

			pg_clean_ascii(appname, 0);
			port->application_name = appname;
		}
	}
}

static int
mys_start(Port *port)
{
	MemoryContext oldcxt;
	StringInfoData pkt;
	uint64		v;
	const char *dummy;
	char	   *user;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	initStringInfo(&outbuf);

	/* we are not speaking the PG protocol, but pretend v3 for core code */
	port->proto = FrontendProtocol = PG_PROTOCOL_LATEST;

	mys_make_scramble();
	mys_send_greeting(port);

	initStringInfo(&pkt);
	if (mys_read_packet(&pkt) == EOF)
	{
		MemoryContextSwitchTo(oldcxt);
		return STATUS_ERROR;	/* client went away, e.g. port probe */
	}

	if (!get_uint(&pkt, 4, &v))
		goto bad_packet;
	mys_client_caps = (uint32) v;

	if (!(mys_client_caps & CLIENT_PROTOCOL_41))
	{
		mys_send_err(1251, "08004",
					 "Client does not support authentication protocol requested by server; consider upgrading MySQL client");
		mys_flush();
		MemoryContextSwitchTo(oldcxt);
		return STATUS_ERROR;
	}

	if (mys_client_caps & CLIENT_SSL)
	{
		/*
		 * SSLRequest (caps, max packet, charset, filler: 32 bytes), then the
		 * TLS handshake right away.  PG's own TLS setup does the work and
		 * takes care of handshake bytes that were already buffered.
		 */
		if (!mys_ssl_available(port) || pkt.len != 32)
			goto bad_packet;
		if (secure_open_server(port) != 0)
		{
			MemoryContextSwitchTo(oldcxt);
			return STATUS_ERROR;	/* TLS alert already sent */
		}

		/* the real HandshakeResponse41 follows, encrypted */
		if (mys_read_packet(&pkt) == EOF || !get_uint(&pkt, 4, &v))
		{
			MemoryContextSwitchTo(oldcxt);
			return STATUS_ERROR;
		}
		mys_client_caps = (uint32) v;
	}

	/* max packet size(4), charset(1), filler(23) */
	if (!get_bytes(&pkt, 4 + 1 + 23, &dummy))
		goto bad_packet;
	user = get_cstring(&pkt);
	if (user == NULL)
		goto bad_packet;

	/* auth response */
	if (mys_client_caps & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
	{
		if (!get_lenenc_int(&pkt, &v) || !get_bytes(&pkt, (int) v, &dummy))
			goto bad_packet;
	}
	else if (mys_client_caps & CLIENT_SECURE_CONNECTION)
	{
		if (!get_uint(&pkt, 1, &v) || !get_bytes(&pkt, (int) v, &dummy))
			goto bad_packet;
	}
	else
	{
		char	   *s = get_cstring(&pkt);

		if (s == NULL)
			goto bad_packet;
		v = strlen(s);
		dummy = s;
	}
	client_auth_resp = palloc(v + 1);
	memcpy(client_auth_resp, dummy, v);
	client_auth_resp[v] = '\0';
	client_auth_resp_len = (int) v;

	if ((mys_client_caps & CLIENT_CONNECT_WITH_DB) && pkt.cursor < pkt.len)
		initial_db = get_cstring(&pkt);
	if ((mys_client_caps & CLIENT_PLUGIN_AUTH) && pkt.cursor < pkt.len)
		client_plugin = get_cstring(&pkt);
	if ((mys_client_caps & CLIENT_CONNECT_ATTRS) && pkt.cursor < pkt.len)
		mys_parse_connect_attrs(&pkt, port);

	if (user[0] == '\0')
	{
		mys_send_err(1045, "28000", "Access denied: empty user name");
		mys_flush();
		MemoryContextSwitchTo(oldcxt);
		return STATUS_ERROR;
	}

	port->user_name = user;
	port->database_name = pstrdup(halo_mysql_database);
	port->guc_options = list_make4(pstrdup("client_encoding"), pstrdup("UTF8"),
								   pstrdup("DateStyle"), pstrdup("ISO, YMD"));
	if (port->application_name)
		port->guc_options = lappend(lappend(port->guc_options,
											pstrdup("application_name")),
									pstrdup(port->application_name));

	MemoryContextSwitchTo(oldcxt);
	return STATUS_OK;

bad_packet:
	mys_send_err(1043, "08S01", "Bad handshake");
	mys_flush();
	MemoryContextSwitchTo(oldcxt);
	return STATUS_ERROR;
}

/* ----------------------------------------------------------------
 * Authentication (inside InitPostgres, shared catalogs available)
 * ----------------------------------------------------------------
 */
static void
mys_read_auth_packet(StringInfo buf)
{
	if (mys_read_packet(buf) == EOF)
		ereport(FATAL,
				(errcode(ERRCODE_CONNECTION_FAILURE),
				 errmsg("client disconnected during authentication")));
}

/* AuthMoreData: 0x01 followed by plugin data */
static void
mys_send_auth_more_data(const char *data, int len)
{
	StringInfoData b;

	initStringInfo(&b);
	put_int1(&b, 0x01);
	appendBinaryStringInfo(&b, data, len);
	mys_send_packet(b.data, b.len);
	mys_flush();
	pfree(b.data);
}

/*
 * AuthSwitchRequest: make the client restart authentication with another
 * plugin; its answer replaces client_auth_resp.
 */
static void
mys_auth_switch(const char *plugin)
{
	StringInfoData b;
	StringInfoData resp;

	initStringInfo(&b);
	put_int1(&b, 0xfe);
	appendStringInfoString(&b, plugin);
	put_int1(&b, 0);
	if (strcmp(plugin, AUTH_CLEAR) != 0)
	{
		appendBinaryStringInfo(&b, scramble, MYS_SCRAMBLE_LEN);
		put_int1(&b, 0);
	}
	mys_send_packet(b.data, b.len);
	mys_flush();
	pfree(b.data);

	initStringInfo(&resp);
	mys_read_auth_packet(&resp);
	client_plugin = pstrdup(plugin);
	client_auth_resp = resp.data;
	client_auth_resp_len = resp.len;
}

static void
mys_auth_failed(Port *port, bool with_password, const char *logdetail)
{
	ereport(FATAL,
			(errcode(ERRCODE_INVALID_PASSWORD),
			 errmsg("Access denied for user '%s'@'%s' (using password: %s)",
					port->user_name, port->remote_host,
					with_password ? "YES" : "NO"),
			 logdetail ? errdetail_log("%s", logdetail) : 0));
}

/*
 * caching_sha2_password full authentication: the client sends the password
 * in clear text over TLS, or RSA-OAEP encrypted (after fetching our public
 * key if it does not have it).
 */
static char *
mys_sha2_full_auth(Port *port)
{
	StringInfoData pkt;
	char	   *passwd;

	initStringInfo(&pkt);
	mys_read_auth_packet(&pkt);

	if (port->ssl_in_use)
		return pnstrdup(pkt.data, strnlen(pkt.data, pkt.len));

	if (pkt.len == 1 && (uint8) pkt.data[0] == SHA2_REQUEST_PUBLIC_KEY)
	{
		int			len;
		const char *pem = mys_rsa_public_key(&len);

		if (pem == NULL)
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("Authentication requires secure connection."),
					 errdetail_log("No RSA key pair is available (mysql.rsa_private_key_file) and the connection does not use TLS.")));
		mys_send_auth_more_data(pem, len);
		mys_read_auth_packet(&pkt);
	}

	passwd = mys_rsa_decrypt_password((uint8 *) pkt.data, pkt.len,
									  scramble, MYS_SCRAMBLE_LEN);
	if (passwd == NULL)
		mys_auth_failed(port, true, "could not decrypt the RSA encrypted password");
	return passwd;
}

/*
 * Password authentication for pg_hba.conf methods password / md5 /
 * scram-sha-256.  The password is always checked against the role's
 * regular PostgreSQL verifier, so one password works for both protocols.
 */
static void
mys_password_auth(Port *port)
{
	const char *logdetail = NULL;
	char	   *shadow_pass;
	char	   *passwd;

	shadow_pass = get_role_password(port->user_name, &logdetail);

	/* as in PG, scram-sha-256 does not accept MD5 verifiers */
	if (shadow_pass && port->hba->auth_method == uaSCRAM &&
		get_password_type(shadow_pass) != PASSWORD_TYPE_SCRAM_SHA_256)
	{
		logdetail = psprintf("User \"%s\" does not have a valid SCRAM secret.",
							 port->user_name);
		shadow_pass = NULL;
	}

	if ((client_plugin && strcmp(client_plugin, AUTH_CLEAR) == 0) ||
		halo_mysql_auth_plugin == MYS_AUTH_CLEARTEXT)
	{
		/* the client chose clear text, or the server is in legacy mode */
		if (client_plugin == NULL || strcmp(client_plugin, AUTH_CLEAR) != 0)
			mys_auth_switch(AUTH_CLEAR);
		passwd = pnstrdup(client_auth_resp,
						  strnlen(client_auth_resp, client_auth_resp_len));
	}
	else
	{
		static const char fast_ok = SHA2_FAST_AUTH_SUCCESS;
		static const char full_auth = SHA2_PERFORM_FULL_AUTH;

		/* e.g. a client configured with default-auth=mysql_native_password */
		if (client_plugin == NULL || strcmp(client_plugin, AUTH_SHA2) != 0)
			mys_auth_switch(AUTH_SHA2);

		if (client_auth_resp_len == 0)
			mys_auth_failed(port, false, logdetail);

		if (mys_sha2_fast_auth(port->user_name, shadow_pass,
							   scramble, MYS_SCRAMBLE_LEN,
							   (uint8 *) client_auth_resp, client_auth_resp_len))
		{
			mys_send_auth_more_data(&fast_ok, 1);
			return;
		}

		/*
		 * Cache miss (or wrong password): ask for the password itself.  Also
		 * done for unknown roles, so they cannot be told apart.
		 */
		mys_send_auth_more_data(&full_auth, 1);
		passwd = mys_sha2_full_auth(port);
	}

	if (passwd[0] == '\0')
		mys_auth_failed(port, false, logdetail);
	if (shadow_pass == NULL ||
		plain_crypt_verify(port->user_name, shadow_pass, passwd,
						   &logdetail) != STATUS_OK)
		mys_auth_failed(port, true, logdetail);

	mys_sha2_cache_store(port->user_name, shadow_pass, passwd);
	explicit_bzero(passwd, strlen(passwd));
}

static void
mys_authenticate(Port *port)
{
	hba_getauthmethod(port);

	switch (port->hba->auth_method)
	{
		case uaReject:
		case uaImplicitReject:
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("Access denied for user '%s'@'%s' (rejected by pg_hba.conf for database \"%s\")",
							port->user_name, port->remote_host,
							port->database_name)));
			break;

		case uaTrust:
			break;

		case uaPassword:
		case uaMD5:
		case uaSCRAM:
			mys_password_auth(port);
			break;

		default:
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
					 errmsg("authentication method \"%s\" is not supported by the MySQL protocol",
							hba_authname(port->hba->auth_method))));
	}

	set_authn_id(port, port->user_name);
}

/* ----------------------------------------------------------------
 * Database (= schema) selection
 * ----------------------------------------------------------------
 */
static bool
mys_use_database(const char *db, bool raise)
{
	bool		found;
	StringInfoData path;

	StartTransactionCommand();
	found = OidIsValid(get_namespace_oid(db, true));
	CommitTransactionCommand();

	if (!found)
	{
		if (raise)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_SCHEMA_NAME),
					 errmsg("Unknown database '%s'", db)));
		return false;
	}

	/*
	 * Same order as openHalo: MySQL-semantics functions and operators in
	 * schema "mysql" must win over the pg_catalog built-ins.
	 */
	initStringInfo(&path);
	appendStringInfo(&path, "%s, mysql, pg_catalog, \"$user\", public", quote_identifier(db));
	SetConfigOption("search_path", path.data, PGC_USERSET, PGC_S_SESSION);
	pfree(path.data);
	return true;
}

/* ----------------------------------------------------------------
 * Main loop hooks
 * ----------------------------------------------------------------
 */
static void
mys_send_ready_for_query(CommandDest dest)
{
	/* after an error inside a transaction: statement-level rollback */
	mys_txn_recover();

	if (!handshake_done)
	{
		/* An installation error must close the connection, never send OK. */
		PG_TRY();
		{
			halo_mysql_ensure_extension();
			/* GLOBAL defaults (transaction_read_only ...) reach the first statement */
			StartTransactionCommand();
			mys_sync_session_state();
			mys_binary_session_init();
			CommitTransactionCommand();
		}
		PG_CATCH();
		{
			ErrorData *edata;

			MemoryContextSwitchTo(TopMemoryContext);
			edata = CopyErrorData();
			FlushErrorState();
			ereport(FATAL,
					(errcode(edata->sqlerrcode),
					 errmsg("could not initialize MySQL session: %s", edata->message)));
		}
		PG_END_TRY();
		handshake_done = true;
		if (initial_db && initial_db[0] != '\0')
		{
			if (!mys_use_database(initial_db, false))
			{
				StringInfoData msg;

				initStringInfo(&msg);
				appendStringInfo(&msg, "Unknown database '%s'", initial_db);
				mys_send_err(1049, "42000", msg.data);
				mys_flush();
				proc_exit(0);
			}
		}
		else
			SetConfigOption("search_path", "mysql, pg_catalog, \"$user\", public",
							PGC_USERSET, PGC_S_SESSION);
		mys_send_ok(0, 0, false, false);
	}
	resultset_active = false;
	stmt_infos = NIL;
	stmt_index = 0;
	trailing_comment_ok = false;
	mys_stmt_query_done();
	mys_flush();
}

/*
 * An error raised while decoding a command inside read_command is reported
 * from process_command, i.e. from the main loop proper.
 */
#define MYS_DEFERRED_ERROR		0xff
static ErrorData *deferred_error = NULL;

static int
mys_read_command(StringInfo inBuf)
{
	uint8		cmd;

	deferred_error = NULL;
	if (queued_queries != NIL)
	{
		/* the next part of the current command (see mys_queue_query) */
		MysQueuedQuery *q = linitial(queued_queries);

		resetStringInfo(inBuf);
		appendBinaryStringInfo(inBuf, q->query, strlen(q->query) + 1);	/* with the NUL */
		if (q->map)
			mys_text_map = copy_text_map(q->map);
		queued_queries = list_delete_first(queued_queries);
		if (queued_queries == NIL)
			MemoryContextReset(queue_context);
		last_queued_part = queued_queries == NIL;
		diagnostics_pending = true;
		stmt_infos = NIL;
		stmt_index = 0;
		return PqMsg_Query;
	}
	if (mys_read_packet(inBuf) == EOF)
		return EOF;

	diagnostics_pending = true;
	stmt_infos = NIL;
	stmt_index = 0;
	trailing_comment_ok = false;
	trailing_ok_deferred = last_queued_part = false;
	if (inBuf->len == 0)
		ereport(FATAL,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("empty MySQL command packet")));

	cmd = (uint8) inBuf->data[0];
	com_query = (cmd == COM_QUERY);
	switch (cmd)
	{
		case COM_QUIT:
			return PqMsg_Terminate;

		case COM_QUERY:
		case COM_STMT_EXECUTE:
			{
				/* becomes a regular query with the parameters rendered in */
				MemoryContext cxt = CurrentMemoryContext;
				volatile bool ok = true;

				PG_TRY();
				{
					if (cmd == COM_STMT_EXECUTE)
						mys_stmt_execute(inBuf);
					else
					{
						char *query = mys_normalize_query(inBuf->data + 1, inBuf->len - 1);

						resetStringInfo(inBuf);
						appendBinaryStringInfo(inBuf, query, strlen(query) + 1);
						pfree(query);
					}
				}
				PG_CATCH();
				{
					MemoryContextSwitchTo(cxt);
					deferred_error = CopyErrorData();
					FlushErrorState();
					mys_stmt_query_done();
					ok = false;
				}
				PG_END_TRY();
				return ok ? PqMsg_Query : MYS_MSG_BASE + MYS_DEFERRED_ERROR;
			}

		default:
			return MYS_MSG_BASE + cmd;
	}
}

static bool
mys_process_command(int firstchar, StringInfo inBuf)
{
	uint8		cmd;

	if (firstchar < MYS_MSG_BASE)
		return false;
	cmd = (uint8) (firstchar - MYS_MSG_BASE);

	switch (cmd)
	{
		case COM_INIT_DB:
			{
				char	   *db = pnstrdup(inBuf->data + 1, inBuf->len - 1);

				if (mys_use_database(db, false))
					mys_send_ok(0, 0, false, false);
				else
					mys_send_error(ERRCODE_INVALID_SCHEMA_NAME,
								   psprintf("Unknown database '%s'", db));
				break;
			}
		case COM_PING:
			mys_send_ok(0, 0, false, false);
			break;
		case COM_RESET_CONNECTION:
			{
				DiscardStmt discard = {0};
				MemoryContext cxt = CurrentMemoryContext;
				/* USE is a search_path assignment, including SQL USE statements. */
				char *path = pstrdup(GetConfigOption("search_path", false, false));

				AbortOutOfAnyTransaction();
				MemoryContextSwitchTo(cxt);
				mys_txn_recover();
				mys_stmt_close_all();
				mys_stmt_query_done();
				clearUserVars();
				StartTransactionCommand();
				PushActiveSnapshot(GetTransactionSnapshot());
				/* GET_LOCK() locks (DISCARD ALL drops every advisory lock) */
				(void) mys_release_user_locks();
				discard.type = T_DiscardStmt;
				discard.target = DISCARD_ALL;
				DiscardCommand(&discard, true);
				resetSessionSystemVariables();
				PopActiveSnapshot();
				CommitTransactionCommand();
				SetConfigOption("search_path", path, PGC_USERSET, PGC_S_SESSION);
				affectedRows = 0;
				lastInsertID = cur_insert_id = 0;
				mys_clear_diagnostics();
				mys_send_ok(0, 0, false, false);
				break;
			}
		case COM_STMT_PREPARE:
			mys_stmt_prepare(inBuf);
			break;
		case COM_STMT_CLOSE:
			mys_stmt_close(inBuf);	/* no response */
			break;
		case COM_STMT_RESET:
			mys_stmt_reset(inBuf);
			break;
		case COM_STMT_SEND_LONG_DATA:
			mys_stmt_send_long_data(inBuf); /* no response */
			break;
		case COM_STMT_FETCH:
			/* COM_STMT_EXECUTE never opens a cursor */
			mys_send_error(ERRCODE_INVALID_CURSOR_STATE,
						   "The statement has no open cursor.");
			break;
		case MYS_DEFERRED_ERROR:
			/* COM_STMT_EXECUTE could not be decoded: nothing ran */
			mys_send_error(deferred_error->sqlerrcode, deferred_error->message);
			deferred_error = NULL;
			break;
		case COM_FIELD_LIST:
			/* column list for tab completion: answer with an empty list */
			mys_send_resultset_end(false);
			break;
		case COM_SET_OPTION:
			mys_send_eof(false);
			break;
		case COM_STATISTICS:
			{
				const char *s = "Uptime: 1  Threads: 1  Questions: 0  Slow queries: 0  Opens: 0  Flush tables: 0  Open tables: 0";

				mys_send_packet(s, strlen(s));
				break;
			}
		default:
			{
				char		msg[64];

				snprintf(msg, sizeof(msg), "Unsupported MySQL command 0x%02x", cmd);
				mys_send_err(1047, "08S01", msg);
				break;
			}
	}
	return true;
}

static List *
mys_parse_query(const char *query_string)
{
	List	   *stmts;

	/* session variables carried by GUCs; SET TRANSACTION; GLOBAL read_only */
	if (!IsAbortedTransactionBlockState())
		mys_sync_session_state();

	if (halo_mysql_parser == MYS_PARSER_POSTGRES)
	{
		stmts = raw_parser(query_string, RAW_PARSE_DEFAULT);
		mys_discard_rewrite_warnings();
		stmt_infos = NIL;
		for (int i = 0; i < list_length(stmts); i++)
			stmt_infos = lappend(stmt_infos, palloc0(sizeof(MysStmtInfo)));
	}
	else
	{
		List	   *raw = NIL;

		if (halo_mysql_statement_rollback && IsTransactionBlock() &&
			!IsAbortedTransactionBlockState())
		{
			/*
			 * A syntax error inside a transaction must not abort it either:
			 * re-raise it from a statement that runs under the hidden
			 * savepoint.  Only the grammar runs here, which allocates
			 * nothing but memory, so it is safe to catch its errors.
			 */
			MemoryContext cxt = CurrentMemoryContext;
			ErrorData  *volatile perr = NULL;

			PG_TRY();
			{
				raw = mysql_raw_parser(query_string);
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(cxt);
				perr = CopyErrorData();
				FlushErrorState();
			}
			PG_END_TRY();
			if (perr)
				raw = mys_txn_deferred_error(perr);
			else
				trailing_comment_ok = has_trailing_comment(raw, query_string);
		}
		else
		{
			raw = mysql_raw_parser(query_string);
			trailing_comment_ok = has_trailing_comment(raw, query_string);
		}
		/* the trailing comment of a packet an EXECUTE split (mys_end_command) */
		if (last_queued_part && trailing_ok_deferred)
		{
			trailing_comment_ok = true;
			trailing_ok_deferred = false;
		}

		stmts = mys_rewrite_raw_stmts(raw, query_string, &stmt_infos);
		/* autocommit / implicit commit rules */
		stmts = mys_txn_apply(stmts, &stmt_infos);
	}
	stmt_index = 0;
	cur_insert_id = 0;
	memset(upsert_rows, 0, sizeof(upsert_rows));
	upsert_deleted = 0;
	foreach_ptr(MysStmtInfo, info, stmt_infos)
	{
		if (info->suppress_response)
			continue;
		if (!info->diagnostic)
			mys_clear_diagnostics();
		else
			diagnostics_pending = false;
		break;
	}

	/* a statement transaction starts now, a START TRANSACTION at execution */
	if (!IsAbortedTransactionBlockState() &&
		!(stmts != NIL && IsA(linitial_node(RawStmt, stmts)->stmt, TransactionStmt) &&
		  (((TransactionStmt *) linitial_node(RawStmt, stmts)->stmt)->kind == TRANS_STMT_START ||
		   ((TransactionStmt *) linitial_node(RawStmt, stmts)->stmt)->kind == TRANS_STMT_BEGIN)))
		mys_start_implicit_transaction(stmt_infos != NIL &&
									   ((MysStmtInfo *) linitial(stmt_infos))->txn_kind ==
									   MYS_TXN_IMPLICIT_COMMIT);
	return stmts;
}

/* Count at execution time: RETURNING can be buffered before its receiver
 * is started.  Only scalar counts live across that boundary. */
static void
mys_record_upsert_row(TupleTableSlot *oldslot, TupleTableSlot *newslot, uint64 deleted)
{
	uint64 row;

	if (current_stmt_info()->upsert == MYS_UPSERT_NONE)
		return;
	row = mys_upsert_row(oldslot, newslot, deleted);
	if (row <= MYS_ROW_UNCHANGED)
		upsert_rows[row]++;
	else
	{
		upsert_rows[MYS_ROW_INSERTED]++;
		upsert_deleted += row - MYS_ROW_UNCHANGED;
	}
}

/*
 * MySQL's affected rows of INSERT ... ON DUPLICATE KEY UPDATE / REPLACE: 1
 * per inserted row, 2 per updated or replaced row; a row ON DUPLICATE KEY
 * UPDATE leaves unchanged counts 0, 1 for CLIENT_FOUND_ROWS clients; an
 * identical row REPLACE leaves alone counts 1, 2 when it deletes and
 * inserts.  nprocessed is the fallback for statements outside this execution path.
 */
static uint64
upsert_affected(MysUpsertKind kind, uint64 nprocessed)
{
	uint64		inserted = upsert_rows[MYS_ROW_INSERTED];
	uint64		changed = upsert_rows[MYS_ROW_CHANGED];
	uint64		unchanged = upsert_rows[MYS_ROW_UNCHANGED];
	uint64		deleted = upsert_deleted;

	memset(upsert_rows, 0, sizeof(upsert_rows));
	upsert_deleted = 0;
	if (inserted + changed + unchanged != nprocessed)
		return nprocessed;
	switch (kind)
	{
		case MYS_UPSERT_UPDATE:
			if (!(mys_client_caps & CLIENT_FOUND_ROWS))
				unchanged = 0;
			break;
		case MYS_UPSERT_REPLACE_DELETE:
			unchanged *= 2;
			break;
		default:
			break;
	}
	return inserted + 2 * changed + unchanged + deleted;
}

static void
end_statement(const QueryCompletion *qc)
{
	MysStmtInfo *info = current_stmt_info();
	MysStmtInfo *following;
	bool		more = more_visible_results();

	mys_flush_rewrite_warnings(info->rewrite_warnings);
	info->rewrite_warnings = NIL;
	stmt_index++;
	following = stmt_index < list_length(stmt_infos) ?
		(MysStmtInfo *) list_nth(stmt_infos, stmt_index) : NULL;
	mys_txn_stmt_done(info);
	/* SET TRANSACTION's one-shot values; the next transaction's defaults */
	mys_statement_done(info->txn_kind == MYS_TXN_END && !info->txn_control,
					   following != NULL && following->txn_kind == MYS_TXN_IMPLICIT_COMMIT);
	if (info->suppress_response || info->continued)
	{
		resultset_active = false;
		return;
	}

	/*
	 * COMMIT of a transaction that PostgreSQL had to abort: MySQL clients
	 * must not take this for a successful commit.
	 */
	if (info->is_commit && qc->commandTag == CMDTAG_ROLLBACK)
	{
		mys_send_err(1180, "40000",
					 "Transaction was rolled back, not committed: an earlier statement failed");
		affectedRows = -1;
		trailing_comment_ok = false;
		return;
	}

	if (resultset_active)
	{
		mys_send_resultset_end(more);
		resultset_active = false;
		affectedRows = -1;
	}
	else if (info->silent)
		mys_send_ok(0, 0, false, more);
	else
	{
		uint64		affected = 0;

		switch (qc->commandTag)
		{
			case CMDTAG_INSERT:
				affected = info->upsert == MYS_UPSERT_NONE ? qc->nprocessed
					: upsert_affected(info->upsert, qc->nprocessed);
				break;
			case CMDTAG_UPDATE:
			case CMDTAG_DELETE:
			case CMDTAG_MERGE:
			case CMDTAG_SELECT:
			case CMDTAG_COPY:
				affected = qc->nprocessed;
				break;
			default:
				break;
		}
		mys_send_ok(affected, cur_insert_id, false, more);
		cur_insert_id = 0;
		/* ROW_COUNT(): rows changed by the previous DML statement */
		affectedRows = (qc->commandTag == CMDTAG_SELECT) ? -1 : (long long) affected;
	}
	/* A subsequent SHOW in the same packet reads the preceding statement. */
	for (int i = stmt_index; i < list_length(stmt_infos); i++)
	{
		MysStmtInfo *next = list_nth(stmt_infos, i);

		if (next->suppress_response)
			continue;
		if (!next->diagnostic)
			mys_clear_diagnostics();
		break;
	}
}

static void
mys_end_command(const QueryCompletion *qc, CommandDest dest)
{
	end_statement(qc);

	/*
	 * after the last statement, the empty one of a trailing comment; when an
	 * EXECUTE queued its statement (and the rest of the packet), after those
	 */
	if (trailing_comment_ok && stmt_index >= list_length(stmt_infos) && queued_queries != NIL)
	{
		trailing_comment_ok = false;
		trailing_ok_deferred = true;
	}
	else if (trailing_comment_ok && stmt_index >= list_length(stmt_infos))
	{
		trailing_comment_ok = false;
		/* a statement of its own: it clears the warnings, as in MySQL */
		mys_clear_diagnostics();
		mys_send_ok(0, 0, false, false);
	}
}

#ifndef HALO_BABELFISH_API
static void
mys_null_command(CommandDest dest)
{
	mys_send_ok(0, 0, false, false);
}
#endif

/* NO ACTION uses the same PostgreSQL SQLSTATE on either side of an FK.
 * message_id is the untranslated primary template, so lc_messages does not
 * affect the distinction between a missing parent and a referenced parent. */
static uint16
mys_error_number(ErrorData *edata)
{
	if (edata->sqlerrcode == ERRCODE_FOREIGN_KEY_VIOLATION &&
		edata->message_id &&
		strncmp(edata->message_id, "update or delete on table", 24) == 0)
		return 1451;
	return mys_errno(edata->sqlerrcode);
}

static void
mys_send_message(ErrorData *edata)
{
	char	   *msg;

	if (edata->elevel < ERROR)
	{
		/* not the warnings of hidden BEGIN / COMMIT statements */
		if (edata->elevel >= WARNING && !current_stmt_info()->txn_control)
			mys_record_diagnostic(false, mys_error_number(edata), edata->message);
		return;
	}

	msg = edata->message ? edata->message : "missing error text";
	/* a key (a unique violation, ...): its MySQL name, not the PostgreSQL one */
	if (edata->constraint_name)
	{
		char	   *pgname = psprintf("\"%s\"", edata->constraint_name);
		char	   *key = mys_key_name(edata->constraint_name, false);
		const char *at = strstr(msg, pgname);

		if (at && strcmp(key, edata->constraint_name) != 0)
			msg = psprintf("%.*s\"%s\"%s", (int) (at - msg), msg, key, at + strlen(pgname));
	}
	mys_send_err(mys_error_number(edata),
				 mys_sqlstate(edata->sqlerrcode), msg);
	mys_flush();
	resultset_active = false;
	stmt_infos = NIL;
	stmt_index = 0;
	trailing_comment_ok = false;
}

static void
mys_comm_reset(void)
{
	memset(upsert_rows, 0, sizeof(upsert_rows));
	upsert_deleted = 0;
	resultset_active = false;
	discard_queued_queries();
#ifdef HALO_BABELFISH_API
	/* receivers of aborted portals are gone */
	list_free_deep(bf_receivers);
	bf_receivers = NIL;
#endif
}

/* ----------------------------------------------------------------
 * DestReceiver: text protocol result sets
 * ----------------------------------------------------------------
 */
typedef struct MysColumn
{
	Oid			typid;
	uint8		mtype;
	FmgrInfo	finfo;			/* output function */
	bool		typisvarlena;
} MysColumn;

typedef struct
{
	DestReceiver pub;			/* must stay first: layout-compatible with */
	Portal		portal;			/* DR_printtup for SetRemoteDestReceiverParams */
	/* private */
	bool		discard;		/* silent statement or hidden RETURNING */
	bool		capture;		/* remember first value as LAST_INSERT_ID */
	bool		captured;
	bool		upsert;			/* count the last column's MysUpsertRow */
	int			natts;
	MysColumn  *cols;
	StringInfoData row;
	MemoryContext tmpcontext;
	DestReceiver *owner;		/* Babelfish: the DR_printtup it belongs to */
} MysDR;

#ifdef HALO_BABELFISH_API
/*
 * Babelfish allocates its own DR_printtup and only swaps in our callbacks,
 * so the per-receiver state lives in a side list.  Like DR_printtup, both
 * start with {DestReceiver, Portal}: SetRemoteDestReceiverParams() fills in
 * the portal before the startup callback runs.
 */
typedef struct
{
	DestReceiver pub;
	Portal		portal;
} MysDRHead;

static MysDR *
mys_dr(DestReceiver *self)
{
	MysDR	   *dr;

	foreach_ptr(MysDR, d, bf_receivers)
	{
		if (d->owner == self)
			return d;
	}
	dr = MemoryContextAllocZero(TopMemoryContext, sizeof(MysDR));
	dr->owner = self;
	dr->portal = ((MysDRHead *) self)->portal;
	dr->pub = *self;
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		bf_receivers = lappend(bf_receivers, dr);
		MemoryContextSwitchTo(old);
	}
	return dr;
}

static void
mys_dr_forget(DestReceiver *self)
{
	foreach_ptr(MysDR, d, bf_receivers)
	{
		if (d->owner == self)
		{
			bf_receivers = list_delete_ptr(bf_receivers, d);
			pfree(d);
			return;
		}
	}
}
#else
#define mys_dr(self)	((MysDR *) (self))
#endif

void
mys_type_info(Oid typid, int32 typmod, uint8 *mtype, uint32 *len,
			  uint16 *flags, uint8 *decimals, uint16 *charset)
{
	*flags = 0;
	*decimals = 0;
	*charset = MYS_CHARSET_BINARY;

	/* aux_mysql models tinyint, datetime, ... as domains */
	typid = getBaseTypeAndTypmod(typid, &typmod);
	if (mys_is_text_string(typid))
	{
		*mtype = MYSQL_TYPE_BLOB;
		*len = typmod >= 0 ? (uint32) typmod : 0xffffffff;
		*flags = BLOB_FLAG;
		*charset = MYS_CHARSET_UTF8MB4;
		return;
	}
	if (mys_binary_base_type(typid) != typid)
	{
		*mtype = mys_is_fixed_binary(typid) && typmod >= 0 ?
			MYSQL_TYPE_STRING : MYSQL_TYPE_VAR_STRING;
		*len = typmod >= 0 ? (uint32) typmod : 65535;
		*flags = BINARY_FLAG;
		return;
	}

	switch (typid)
	{
		case BOOLOID:
			*mtype = MYSQL_TYPE_TINY;
			*len = 1;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case INT2OID:
			*mtype = MYSQL_TYPE_SHORT;
			*len = 6;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case INT4OID:
		case OIDOID:
			*mtype = MYSQL_TYPE_LONG;
			*len = 11;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case INT8OID:
			*mtype = MYSQL_TYPE_LONGLONG;
			*len = 20;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case FLOAT4OID:
			*mtype = MYSQL_TYPE_FLOAT;
			*len = 12;
			*decimals = 31;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case FLOAT8OID:
			*mtype = MYSQL_TYPE_DOUBLE;
			*len = 22;
			*decimals = 31;
			*flags = NUM_FLAG | BINARY_FLAG;
			break;
		case NUMERICOID:
			*mtype = MYSQL_TYPE_NEWDECIMAL;
			*flags = NUM_FLAG | BINARY_FLAG;
			if (typmod >= (int32) VARHDRSZ)
			{
				int32		tm = typmod - VARHDRSZ;

				*len = ((tm >> 16) & 0xffff) + 2;
				*decimals = tm & 0xffff;
			}
			else
			{
				*len = 66;
				*decimals = 30;
			}
			break;
		case DATEOID:
			*mtype = MYSQL_TYPE_DATE;
			*len = 10;
			*flags = BINARY_FLAG;
			break;
		case TIMEOID:
		case TIMETZOID:
			*mtype = MYSQL_TYPE_TIME;
			*len = 17;
			*decimals = typmod >= 0 && typmod <= 6 ? typmod : 6;
			*flags = BINARY_FLAG;
			break;
		case TIMESTAMPOID:
			*mtype = MYSQL_TYPE_DATETIME;
			*len = 26;
			*decimals = typmod >= 0 && typmod <= 6 ? typmod : 6;
			*flags = BINARY_FLAG;
			break;
		case TIMESTAMPTZOID:
			*mtype = MYSQL_TYPE_TIMESTAMP;
			*len = 26;
			*decimals = typmod >= 0 && typmod <= 6 ? typmod : 6;
			*flags = BINARY_FLAG;
			break;
		case BYTEAOID:
			*mtype = MYSQL_TYPE_BLOB;
			*len = 65535;
			*flags = BLOB_FLAG | BINARY_FLAG;
			break;
		case JSONOID:
		case JSONBOID:
			*mtype = MYSQL_TYPE_JSON;
			*len = 0xffffffff;
			*flags = BLOB_FLAG;
			*charset = MYS_CHARSET_UTF8MB4;
			break;
		case TEXTOID:
			*mtype = MYSQL_TYPE_BLOB;
			*len = 0xffffffff;
			*flags = BLOB_FLAG;
			*charset = MYS_CHARSET_UTF8MB4;
			break;
		case BPCHAROID:
			*mtype = MYSQL_TYPE_STRING;
			*len = typmod >= (int32) VARHDRSZ ? (typmod - VARHDRSZ) * 4 : 4;
			*charset = MYS_CHARSET_UTF8MB4;
			break;
		case UNKNOWNOID:
		case VARCHAROID:
		default:
			*mtype = MYSQL_TYPE_VAR_STRING;
			*len = typmod >= (int32) VARHDRSZ ? (typmod - VARHDRSZ) * 4 : 1024;
			*charset = MYS_CHARSET_UTF8MB4;
			break;
	}
}

void
mys_send_coldef(Form_pg_attribute attr, TargetEntry *tle)
{
	StringInfoData b;
	uint8		mtype;
	uint32		len;
	uint16		flags;
	uint8		decimals;
	uint16		charset;
	char	   *schema = "";
	char	   *table = "";
	char	   *orgcol = "";

	if (tle && OidIsValid(tle->resorigtbl))
	{
		char	   *s = get_namespace_name(get_rel_namespace(tle->resorigtbl));
		char	   *t = get_rel_name(tle->resorigtbl);

		if (s)
			schema = s;
		if (t)
			table = t;
		if (tle->resorigcol > 0)
		{
			char	   *c = get_attname(tle->resorigtbl, tle->resorigcol, true);

			if (c)
				orgcol = c;
		}
	}

	mys_type_info(attr->atttypid, attr->atttypmod, &mtype, &len, &flags,
				  &decimals, &charset);
	if (attr->attnotnull)
		flags |= NOT_NULL_FLAG;

	initStringInfo(&b);
	put_lenenc_str(&b, "def", 3);
	put_lenenc_str(&b, schema, -1);
	put_lenenc_str(&b, table, -1);
	put_lenenc_str(&b, table, -1);
	put_lenenc_str(&b, NameStr(attr->attname), -1);
	put_lenenc_str(&b, orgcol, -1);
	put_lenenc_int(&b, 0x0c);
	put_int2(&b, charset);
	put_int4(&b, len);
	put_int1(&b, mtype);
	put_int2(&b, flags);
	put_int1(&b, decimals);
	put_int2(&b, 0);
	mys_send_packet(b.data, b.len);
	pfree(b.data);
}

static void
mys_dr_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	MysDR	   *dr = mys_dr(self);
	List	   *tlist = NIL;
	ListCell   *tlc;
	StringInfoData b;

	dr->natts = typeinfo->natts;
	dr->cols = MemoryContextAllocZero(CurrentMemoryContext,
									  sizeof(MysColumn) * Max(dr->natts, 1));
	dr->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
										   "aux_mysql row",
										   ALLOCSET_DEFAULT_SIZES);
	initStringInfo(&dr->row);

	{
		MysStmtInfo *info = current_stmt_info();

		dr->discard = info->silent || info->capture_insert_id ||
			info->upsert != MYS_UPSERT_NONE || info->suppress_response;
		dr->capture = info->capture_insert_id;
		dr->captured = false;
		dr->upsert = info->upsert != MYS_UPSERT_NONE;

	}

	if (dr->portal && !dr->discard)
		tlist = FetchPortalTargetList(dr->portal);

	/* column count */
	if (!dr->discard)
	{
		initStringInfo(&b);
		put_lenenc_int(&b, dr->natts);
		mys_send_packet(b.data, b.len);
		pfree(b.data);
	}

	tlc = list_head(tlist);
	for (int i = 0; i < dr->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);
		TargetEntry *tle = NULL;
		MysColumn  *col = &dr->cols[i];
		Oid			typoutput;
		uint32		len;
		uint16		flags,
					charset;
		uint8		decimals;

		/* skip junk entries */
		while (tlc && ((TargetEntry *) lfirst(tlc))->resjunk)
			tlc = lnext(tlist, tlc);
		if (tlc)
		{
			tle = (TargetEntry *) lfirst(tlc);
			tlc = lnext(tlist, tlc);
		}

		col->typid = mys_binary_base_type(attr->atttypid);
		mys_type_info(attr->atttypid, attr->atttypmod, &col->mtype, &len,
					  &flags, &decimals, &charset);
		getTypeOutputInfo(attr->atttypid, &typoutput, &col->typisvarlena);
		fmgr_info(typoutput, &col->finfo);

		if (!dr->discard)
			mys_send_coldef(attr, tle);
	}

	if (dr->discard)
		return;

	if (!(mys_client_caps & CLIENT_DEPRECATE_EOF))
		mys_send_eof(false);

	resultset_active = true;
}

static void
mys_append_value(StringInfo row, MysColumn *col, Datum value)
{
	switch (col->typid)
	{
		case BOOLOID:
			put_lenenc_str(row, DatumGetBool(value) ? "1" : "0", 1);
			return;

		case BYTEAOID:
			{
				bytea	   *b = DatumGetByteaPP(value);

				put_lenenc_str(row, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
				return;
			}
		case BPCHAROID:
			{
				BpChar *b = DatumGetBpCharPP(value);
				int len = VARSIZE_ANY_EXHDR(b);
				char *s;

				if (!(mys_sqlMode & MYS_MODE_PAD_CHAR_TO_FULL_LENGTH))
					len = bpchartruelen(VARDATA_ANY(b), len);
				s = pg_server_to_client(VARDATA_ANY(b), len);
				put_lenenc_str(row, s, s == VARDATA_ANY(b) ? len : strlen(s));
				return;
			}

		case TIMESTAMPTZOID:
			{
				TimestampTz ts = DatumGetTimestampTz(value);
				char		buf[MAXDATELEN + 1];
				struct pg_tm tm;
				fsec_t		fsec;
				int			tz;

				if (TIMESTAMP_NOT_FINITE(ts))
					EncodeSpecialTimestamp(ts, buf);
				else if (timestamp2tm(ts, &tz, &tm, &fsec, NULL, NULL) == 0)
					EncodeDateTime(&tm, fsec, false, 0, NULL, USE_ISO_DATES, buf);
				else
					strcpy(buf, "0000-00-00 00:00:00");
				put_lenenc_str(row, buf, -1);
				return;
			}

		default:
			{
				char	   *s = OutputFunctionCall(&col->finfo, value);
				int			len = strlen(s);
				char	   *conv = pg_server_to_client(s, len);

				if (conv != s)
					len = strlen(conv);
				put_lenenc_str(row, conv, len);
				return;
			}
	}
}

/* DATE / DATETIME / TIMESTAMP in the binary protocol: length + fields */
static void
put_binary_datetime(StringInfo row, struct pg_tm *tm, fsec_t fsec, bool with_time)
{
	bool		has_time = with_time && (tm->tm_hour || tm->tm_min || tm->tm_sec || fsec);
	int			len = !with_time ? 4 : fsec ? 11 : has_time ? 7 : 4;

	put_int1(row, len);
	put_int2(row, (uint16) tm->tm_year);
	put_int1(row, tm->tm_mon);
	put_int1(row, tm->tm_mday);
	if (len >= 7)
	{
		put_int1(row, tm->tm_hour);
		put_int1(row, tm->tm_min);
		put_int1(row, tm->tm_sec);
	}
	if (len == 11)
		put_int4(row, (uint32) fsec);
}

/* TIME in the binary protocol */
static void
put_binary_time(StringInfo row, TimeADT t)
{
	bool		neg = t < 0;
	uint64		us = neg ? -t : t;
	uint32		micro = us % USECS_PER_SEC;
	uint64		secs = us / USECS_PER_SEC;
	int			len = micro ? 12 : secs ? 8 : 0;

	put_int1(row, len);
	if (len == 0)
		return;
	put_int1(row, neg ? 1 : 0);
	put_int4(row, (uint32) (secs / 86400));
	put_int1(row, (secs % 86400) / 3600);
	put_int1(row, (secs % 3600) / 60);
	put_int1(row, secs % 60);
	if (len == 12)
		put_int4(row, micro);
}

/*
 * Binary protocol value; the encoding must match the column type that
 * mys_type_info() announced for the column.
 */
static void
mys_append_binary(StringInfo row, MysColumn *col, Datum value)
{
	switch (col->typid)
	{
		case BOOLOID:
			put_int1(row, DatumGetBool(value) ? 1 : 0);
			return;
		case INT2OID:
			put_int2(row, (uint16) DatumGetInt16(value));
			return;
		case INT4OID:
			put_int4(row, (uint32) DatumGetInt32(value));
			return;
		case OIDOID:
			put_int4(row, (uint32) DatumGetObjectId(value));
			return;
		case INT8OID:
			{
				uint64		v = (uint64) DatumGetInt64(value);

				put_int4(row, (uint32) (v & 0xffffffff));
				put_int4(row, (uint32) (v >> 32));
				return;
			}
		case FLOAT4OID:
			{
				float4		f = DatumGetFloat4(value);
				uint32		u;

				memcpy(&u, &f, sizeof(u));
				put_int4(row, u);
				return;
			}
		case FLOAT8OID:
			{
				float8		f = DatumGetFloat8(value);
				uint64		u;

				memcpy(&u, &f, sizeof(u));
				put_int4(row, (uint32) (u & 0xffffffff));
				put_int4(row, (uint32) (u >> 32));
				return;
			}
		case DATEOID:
			{
				DateADT		d = DatumGetDateADT(value);
				struct pg_tm tm;

				if (DATE_NOT_FINITE(d))
				{
					put_int1(row, 0);
					return;
				}
				j2date(d + POSTGRES_EPOCH_JDATE, &tm.tm_year, &tm.tm_mon, &tm.tm_mday);
				put_binary_datetime(row, &tm, 0, false);
				return;
			}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				Timestamp	ts = DatumGetTimestamp(value);
				struct pg_tm tm;
				fsec_t		fsec;
				int			tz;

				if (TIMESTAMP_NOT_FINITE(ts) ||
					timestamp2tm(ts, col->typid == TIMESTAMPTZOID ? &tz : NULL,
								 &tm, &fsec, NULL, NULL) != 0)
				{
					put_int1(row, 0);
					return;
				}
				put_binary_datetime(row, &tm, fsec, true);
				return;
			}
		case TIMEOID:
			put_binary_time(row, DatumGetTimeADT(value));
			return;
		case TIMETZOID:
			put_binary_time(row, DatumGetTimeTzADTP(value)->time);
			return;
		default:
			/* DECIMAL, strings, BLOB, JSON: same as the text protocol */
			mys_append_value(row, col, value);
			return;
	}
}

static bool
mys_dr_receive(TupleTableSlot *slot, DestReceiver *self)
{
	MysDR	   *dr = mys_dr(self);
	MemoryContext oldcxt;

	slot_getallattrs(slot);

	if (dr->discard)
	{
		if (dr->capture && !dr->captured && dr->natts > 0 && !slot->tts_isnull[0])
		{
			char	   *s = OutputFunctionCall(&dr->cols[0].finfo, slot->tts_values[0]);

			cur_insert_id = strtou64(s, NULL, 10);
			lastInsertID = (unsigned long) cur_insert_id;
			dr->captured = true;
		}

		return true;
	}

	oldcxt = MemoryContextSwitchTo(dr->tmpcontext);
	resetStringInfo(&dr->row);

	if (mys_binary_resultset)
	{
		/* binary protocol row: 0x00, NULL bitmap (offset 2), values */
		int			nbytes = (dr->natts + 7 + 2) / 8;
		int			bitmap;

		put_int1(&dr->row, 0x00);
		bitmap = dr->row.len;
		for (int i = 0; i < nbytes; i++)
			put_int1(&dr->row, 0);
		for (int i = 0; i < dr->natts; i++)
		{
			if (slot->tts_isnull[i])
				dr->row.data[bitmap + (i + 2) / 8] |= (char) (1 << ((i + 2) % 8));
			else
				mys_append_binary(&dr->row, &dr->cols[i], slot->tts_values[i]);
		}
	}
	else
	{
		for (int i = 0; i < dr->natts; i++)
		{
			if (slot->tts_isnull[i])
				put_int1(&dr->row, 0xfb);
			else
				mys_append_value(&dr->row, &dr->cols[i], slot->tts_values[i]);
		}
	}

	mys_send_packet(dr->row.data, dr->row.len);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(dr->tmpcontext);
	return true;
}

static void
mys_dr_shutdown(DestReceiver *self)
{
	MysDR	   *dr = mys_dr(self);

	if (dr->tmpcontext)
		MemoryContextDelete(dr->tmpcontext);
	dr->tmpcontext = NULL;
	if (dr->cols)
		pfree(dr->cols);
	dr->cols = NULL;
}

static void
mys_dr_destroy(DestReceiver *self)
{
#ifdef HALO_BABELFISH_API
	mys_dr_forget(self);
#endif
	pfree(self);
}

#ifndef HALO_BABELFISH_API
static DestReceiver *
mys_create_dest_receiver(CommandDest dest)
{
	MysDR	   *dr = palloc0(sizeof(MysDR));

	dr->pub.receiveSlot = mys_dr_receive;
	dr->pub.rStartup = mys_dr_startup;
	dr->pub.rShutdown = mys_dr_shutdown;
	dr->pub.rDestroy = mys_dr_destroy;
	dr->pub.mydest = dest;
	return (DestReceiver *) dr;
}
#endif

/* ----------------------------------------------------------------
 * Registration
 * ----------------------------------------------------------------
 */
#ifndef HALO_BABELFISH_API

const ProtocolExtensionConfig mysql_protocol_config = {
	.name = "mysql",
	.listen_init = mysql_listen_init,
	.start = mys_start,
	.authenticate = mys_authenticate,
	.send_message = mys_send_message,
	.send_ready_for_query = mys_send_ready_for_query,
	.read_command = mys_read_command,
	.process_command = mys_process_command,
	.parse_query = mys_parse_query,
	.create_dest_receiver = mys_create_dest_receiver,
	.end_command = mys_end_command,
	.null_command = mys_null_command,
	.report_param_status = NULL,
	.comm_reset = mys_comm_reset,
	.foreign_key_checks = mys_foreign_key_checks,
	.foreign_key_insert = mys_pending_fk_check_insert,
	.sequence_write = mys_sequence_write,
	.on_conflict_row = mys_record_upsert_row,
};

bool
mys_is_mysql_session(void)
{
	return MyProtocolExtension == &mysql_protocol_config;
}

#else							/* HALO_BABELFISH_API */

/* ----------------------------------------------------------------
 * Adapter for Babelfish's protocol extension API
 *
 * postgresql_modified_for_babelfish has its own ProtocolExtensionConfig
 * (libpq-be.h): sockets are added with listen_add_socket() from
 * listen_init_hook, every backend hook goes through
 * MyProcPort->protocol_config.  With patches/babelfish-6.2-mysql-delta.patch
 * it also has fn_parse_query and lets fn_process_command hand a message
 * back to the regular main loop.
 * ----------------------------------------------------------------
 */
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/ip.h"
#include "postmaster/postmaster.h"
#include "tcop/backend_startup.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"

static ProtocolExtensionConfig mys_bf_config;
static listen_init_hook_type prev_listen_init_hook = NULL;
static StringInfo bf_inbuf = NULL;
static int	bf_firstchar = 0;

bool
mys_is_mysql_session(void)
{
	return MyProcPort != NULL && MyProcPort->protocol_config == &mys_bf_config;
}

/*
 * Babelfish's ListenServerPort() registers every socket it opens with the
 * default (libpq) protocol, so a protocol extension opens its own sockets,
 * as babelfishpg_tds does.  Same options as ListenServerPort().
 */
static int
mys_bf_listen(const char *host, unsigned short port)
{
	struct addrinfo hint;
	struct addrinfo *addrs = NULL;
	char		service[32];
	int			added = 0;
	int			ret;

	MemSet(&hint, 0, sizeof(hint));
	hint.ai_family = AF_UNSPEC;
	hint.ai_flags = AI_PASSIVE;
	hint.ai_socktype = SOCK_STREAM;
	snprintf(service, sizeof(service), "%d", port);
	ret = pg_getaddrinfo_all(host, service, &hint, &addrs);
	if (ret || addrs == NULL)
	{
		ereport(LOG,
				(errmsg("aux_mysql: could not translate host name \"%s\", service \"%s\" to address: %s",
						host ? host : "*", service, gai_strerror(ret))));
		if (addrs)
			pg_freeaddrinfo_all(hint.ai_family, addrs);
		return STATUS_ERROR;
	}

	for (struct addrinfo *ai = addrs; ai; ai = ai->ai_next)
	{
		pgsocket	fd;
		int			one = 1;
		int			backlog = MaxConnections * 2;	/* as ListenServerPort() */

		if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
			continue;
		if (!listen_have_free_slot())
			break;
		fd = socket(ai->ai_family, SOCK_STREAM, 0);
		if (fd == PGINVALID_SOCKET)
		{
			ereport(LOG, (errcode_for_socket_access(),
						  errmsg("aux_mysql: could not create socket: %m")));
			continue;
		}
		(void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one));
		if (ai->ai_family == AF_INET6)
			(void) setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &one, sizeof(one));
		if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || listen(fd, backlog) < 0)
		{
			ereport(LOG, (errcode_for_socket_access(),
						  errmsg("aux_mysql: could not listen on port %d: %m", port)));
			closesocket(fd);
			continue;
		}
		ereport(LOG,
				(errmsg("aux_mysql: listening on %s address, port %d",
						ai->ai_family == AF_INET6 ? "IPv6" : "IPv4", port)));
		listen_add_socket(fd, &mys_bf_config);
		added++;
	}
	pg_freeaddrinfo_all(hint.ai_family, addrs);
	return added > 0 ? STATUS_OK : STATUS_ERROR;
}

static void
mys_bf_listen_init(void)
{
	if (prev_listen_init_hook)
		prev_listen_init_hook();
	mysql_listen_init();
}

/* what the kernel patch does after the handshake */
static int
mys_bf_start(Port *port)
{
	int			status = mys_start(port);

	if (status != STATUS_OK)
		return status;
	if (port->database_name == NULL || port->database_name[0] == '\0' ||
		port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name or database name specified")));
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';
	/* ProcessStartupPacket() would have set this */
	MyBackendType = B_BACKEND;
	return STATUS_OK;
}

/* Babelfish calls this instead of PerformAuthentication(): do its part too */
static void
mys_bf_authenticate(Port *port, const char **username)
{
	ClientAuthInProgress = true;
	enable_timeout_after(STATEMENT_TIMEOUT, AuthenticationTimeout * 1000);
	set_ps_display("authentication");

	mys_authenticate(port);

	disable_timeout(STATEMENT_TIMEOUT, false);
	if (log_connections & LOG_CONNECTION_AUTHORIZATION)
		ereport(LOG,
				(errmsg("connection authorized: user=%s database=%s (MySQL protocol%s)",
						port->user_name, port->database_name,
						port->ssl_in_use ? ", SSL" : "")));
	set_ps_display("startup");
	ClientAuthInProgress = false;
}

static int
mys_bf_read_command(StringInfo inBuf)
{
	bf_inbuf = inBuf;
	bf_firstchar = mys_read_command(inBuf);
	return bf_firstchar;
}

/* MySQL commands are ours; COM_QUERY etc. go back to the main loop */
static int
mys_bf_process_command(void)
{
	if (bf_firstchar < MYS_MSG_BASE)
		return PROTOCOL_EXTENSION_FALLTHROUGH;
	mys_process_command(bf_firstchar, bf_inbuf);
	return 0;
}

static void
mys_bf_end_command(QueryCompletion *qc, CommandDest dest)
{
	mys_end_command(qc, dest);
}

/* no NullCommand hook in Babelfish: answer an empty query as MySQL does */
static List *
mys_bf_parse_query(const char *query_string)
{
	List	   *stmts = mys_parse_query(query_string);

	if (stmts == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("Query was empty")));
	return stmts;
}

/* the MySQL protocol negotiates TLS after its greeting */
static int
mys_bf_direct_ssl_handshake(Port *port)
{
	return STATUS_OK;
}

void
mysql_babelfish_register(void)
{
	mys_bf_config.fn_accept = libpq_accept;
	mys_bf_config.fn_close = libpq_close;
	mys_bf_config.fn_init = libpq_init;
	mys_bf_config.fn_start = mys_bf_start;
	mys_bf_config.fn_authenticate = mys_bf_authenticate;
	mys_bf_config.fn_mainfunc = libpq_mainfunc;
	mys_bf_config.fn_send_message = mys_send_message;
	mys_bf_config.fn_send_cancel_key = NULL;
	mys_bf_config.fn_comm_reset = mys_comm_reset;
	mys_bf_config.fn_is_reading_msg = libpq_is_reading_msg;
	mys_bf_config.fn_send_ready_for_query = mys_send_ready_for_query;
	mys_bf_config.fn_read_command = mys_bf_read_command;
	mys_bf_config.fn_end_command = mys_bf_end_command;
	mys_bf_config.fn_printtup = mys_dr_receive;
	mys_bf_config.fn_printtup_startup = mys_dr_startup;
	mys_bf_config.fn_printtup_shutdown = mys_dr_shutdown;
	mys_bf_config.fn_printtup_destroy = mys_dr_destroy;
	mys_bf_config.fn_process_command = mys_bf_process_command;
	mys_bf_config.fn_report_param_status = NULL;
	mys_bf_config.fn_direct_ssl_handshake = mys_bf_direct_ssl_handshake;
	mys_bf_config.fn_parse_query = mys_bf_parse_query;

	prev_listen_init_hook = listen_init_hook;
	listen_init_hook = mys_bf_listen_init;
}

#endif							/* HALO_BABELFISH_API */
