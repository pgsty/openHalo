/*-------------------------------------------------------------------------
 *
 * mys_proto.h
 *	  MySQL client/server protocol: constants and packet helpers shared by
 *	  mysql_proto.c (connection, text protocol) and mys_stmt.c (prepared
 *	  statements, binary protocol).
 *
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PROTO_H
#define MYS_PROTO_H

#include "access/tupdesc.h"
#include "lib/stringinfo.h"
#include "nodes/primnodes.h"

/* ----------------------------------------------------------------
 * Protocol constants
 * ----------------------------------------------------------------
 */
#define CLIENT_LONG_PASSWORD					0x00000001
#define CLIENT_FOUND_ROWS						0x00000002
#define CLIENT_LONG_FLAG						0x00000004
#define CLIENT_CONNECT_WITH_DB					0x00000008
#define CLIENT_PROTOCOL_41						0x00000200
#define CLIENT_SSL								0x00000800
#define CLIENT_TRANSACTIONS						0x00002000
#define CLIENT_SECURE_CONNECTION				0x00008000
#define CLIENT_MULTI_STATEMENTS					0x00010000
#define CLIENT_MULTI_RESULTS					0x00020000
#define CLIENT_PS_MULTI_RESULTS					0x00040000
#define CLIENT_PLUGIN_AUTH						0x00080000
#define CLIENT_CONNECT_ATTRS					0x00100000
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA	0x00200000
#define CLIENT_DEPRECATE_EOF					0x01000000

#define SERVER_CAPABILITIES \
	(CLIENT_LONG_PASSWORD | CLIENT_FOUND_ROWS | CLIENT_LONG_FLAG | \
	 CLIENT_CONNECT_WITH_DB | CLIENT_PROTOCOL_41 | CLIENT_TRANSACTIONS | \
	 CLIENT_SECURE_CONNECTION | CLIENT_MULTI_STATEMENTS | \
	 CLIENT_MULTI_RESULTS | CLIENT_PS_MULTI_RESULTS | CLIENT_PLUGIN_AUTH | \
	 CLIENT_CONNECT_ATTRS | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | \
	 CLIENT_DEPRECATE_EOF)

#define SERVER_STATUS_IN_TRANS			0x0001
#define SERVER_STATUS_AUTOCOMMIT		0x0002
#define SERVER_MORE_RESULTS_EXISTS		0x0008
#define SERVER_STATUS_NO_BACKSLASH_ESCAPES 0x0200
#define SERVER_STATUS_IN_TRANS_READONLY	0x2000

#define COM_QUIT				0x01
#define COM_INIT_DB				0x02
#define COM_QUERY				0x03
#define COM_FIELD_LIST			0x04
#define COM_STATISTICS			0x09
#define COM_PING				0x0e
#define COM_SET_OPTION			0x1b
#define COM_RESET_CONNECTION	0x1f

/* firstchar values handed to PostgresMain for non-query commands */
#define MYS_MSG_BASE			0x100

/* MySQL column types */
#define MYSQL_TYPE_DECIMAL		0x00
#define MYSQL_TYPE_TINY			0x01
#define MYSQL_TYPE_SHORT		0x02
#define MYSQL_TYPE_LONG			0x03
#define MYSQL_TYPE_FLOAT		0x04
#define MYSQL_TYPE_DOUBLE		0x05
#define MYSQL_TYPE_NULL			0x06
#define MYSQL_TYPE_TIMESTAMP	0x07
#define MYSQL_TYPE_LONGLONG		0x08
#define MYSQL_TYPE_DATE			0x0a
#define MYSQL_TYPE_TIME			0x0b
#define MYSQL_TYPE_DATETIME		0x0c
#define MYSQL_TYPE_JSON			0xf5
#define MYSQL_TYPE_NEWDECIMAL	0xf6
#define MYSQL_TYPE_BLOB			0xfc
#define MYSQL_TYPE_VAR_STRING	0xfd
#define MYSQL_TYPE_STRING		0xfe

/* MySQL column flags */
#define NOT_NULL_FLAG			0x0001
#define BLOB_FLAG				0x0010
#define BINARY_FLAG				0x0080
#define NUM_FLAG				0x8000

#define MYS_CHARSET_UTF8MB4		255		/* utf8mb4_0900_ai_ci */
#define MYS_CHARSET_UTF8MB4_GENERAL	45	/* utf8mb4_general_ci: MySQL 5.x default */
#define MYS_CHARSET_BINARY		63

#define MYS_MAX_PACKET			0xffffff
#define MYS_SCRAMBLE_LEN		20

#define AUTH_NATIVE				"mysql_native_password"
#define AUTH_SHA2				"caching_sha2_password"
#define AUTH_CLEAR				"mysql_clear_password"

/* caching_sha2_password AuthMoreData payloads */
#define SHA2_REQUEST_PUBLIC_KEY		0x02
#define SHA2_FAST_AUTH_SUCCESS		0x03
#define SHA2_PERFORM_FULL_AUTH		0x04

#define COM_STMT_PREPARE		0x16
#define COM_STMT_EXECUTE		0x17
#define COM_STMT_SEND_LONG_DATA	0x18
#define COM_STMT_CLOSE			0x19
#define COM_STMT_RESET			0x1a
#define COM_STMT_FETCH			0x1c

/* more column types (binary protocol parameters) */
#define MYSQL_TYPE_INT24		0x09
#define MYSQL_TYPE_YEAR			0x0d
#define MYSQL_TYPE_NEWDATE		0x0e
#define MYSQL_TYPE_VARCHAR		0x0f
#define MYSQL_TYPE_BIT			0x10
#define MYSQL_TYPE_ENUM			0xf7
#define MYSQL_TYPE_SET			0xf8
#define MYSQL_TYPE_TINY_BLOB	0xf9
#define MYSQL_TYPE_MEDIUM_BLOB	0xfa
#define MYSQL_TYPE_LONG_BLOB	0xfb
#define MYSQL_TYPE_GEOMETRY		0xff

#define UNSIGNED_FLAG			0x0020

/* per-session protocol state (mysql_proto.c) */
extern uint32 mys_client_caps;
extern uint16 mys_warning_count;

/* ----------------------------------------------------------------
 * Payload builders
 * ----------------------------------------------------------------
 */
static inline void
put_int1(StringInfo b, uint8 v)
{
	appendStringInfoCharMacro(b, (char) v);
}

static inline void
put_int2(StringInfo b, uint16 v)
{
	put_int1(b, v & 0xff);
	put_int1(b, (v >> 8) & 0xff);
}

static inline void
put_int3(StringInfo b, uint32 v)
{
	put_int2(b, v & 0xffff);
	put_int1(b, (v >> 16) & 0xff);
}

static inline void
put_int4(StringInfo b, uint32 v)
{
	put_int2(b, v & 0xffff);
	put_int2(b, (v >> 16) & 0xffff);
}

static inline void
put_lenenc_int(StringInfo b, uint64 v)
{
	if (v < 251)
		put_int1(b, (uint8) v);
	else if (v < 65536)
	{
		put_int1(b, 0xfc);
		put_int2(b, (uint16) v);
	}
	else if (v < 16777216)
	{
		put_int1(b, 0xfd);
		put_int3(b, (uint32) v);
	}
	else
	{
		put_int1(b, 0xfe);
		put_int4(b, (uint32) (v & 0xffffffff));
		put_int4(b, (uint32) (v >> 32));
	}
}

static inline void
put_lenenc_str(StringInfo b, const char *s, int len)
{
	if (s == NULL)
	{
		s = "";
		len = 0;
	}
	if (len < 0)
		len = strlen(s);
	put_lenenc_int(b, len);
	appendBinaryStringInfo(b, s, len);
}

/* ----------------------------------------------------------------
 * Payload readers
 * ----------------------------------------------------------------
 */
/* Read helpers over a StringInfo with bounds checking */
static inline bool
get_bytes(StringInfo b, int n, const char **out)
{
	if (b->cursor + n > b->len)
		return false;
	*out = b->data + b->cursor;
	b->cursor += n;
	return true;
}

static inline bool
get_uint(StringInfo b, int n, uint64 *v)
{
	const char *p;

	if (!get_bytes(b, n, &p))
		return false;
	*v = 0;
	for (int i = n - 1; i >= 0; i--)
		*v = (*v << 8) | (uint8) p[i];
	return true;
}

static inline bool
get_lenenc_int(StringInfo b, uint64 *v)
{
	uint64		first;

	if (!get_uint(b, 1, &first))
		return false;
	if (first < 251)
	{
		*v = first;
		return true;
	}
	if (first == 0xfc)
		return get_uint(b, 2, v);
	if (first == 0xfd)
		return get_uint(b, 3, v);
	if (first == 0xfe)
		return get_uint(b, 8, v);
	return false;
}

/* mysql_proto.c */
extern void mys_flush(void);
extern void mys_send_packet(const char *payload, int len);
extern void mys_send_ok(uint64 affected, uint64 last_insert_id, bool eof_marker,
						bool more_results);
extern void mys_send_eof(bool more_results);
extern void mys_send_resultset_end(bool more_results);
extern void mys_send_err(uint16 code, const char *sqlstate, const char *msg);
extern void mys_send_error(int sqlerrcode, const char *msg);
extern uint16 mys_errno(int sqlerrcode);
extern void mys_type_info(Oid typid, int32 typmod, uint8 *mtype, uint32 *len,
						  uint16 *flags, uint8 *decimals, uint16 *charset);
extern void mys_send_coldef(Form_pg_attribute attr, TargetEntry *tle);

/* mys_stmt.c: prepared statements */
extern bool mys_binary_resultset;
extern void mys_stmt_prepare(StringInfo pkt);
extern void mys_stmt_execute(StringInfo inBuf);
extern void mys_stmt_close(StringInfo pkt);
extern void mys_stmt_reset(StringInfo pkt);
extern void mys_stmt_send_long_data(StringInfo pkt);
extern void mys_stmt_close_all(void);
extern void mys_stmt_query_done(void);

#endif							/* MYS_PROTO_H */
