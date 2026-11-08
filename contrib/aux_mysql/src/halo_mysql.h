/*-------------------------------------------------------------------------
 *
 * halo_mysql.h
 *	  MySQL compatibility extension for (patched) PostgreSQL 18.
 *
 * The wire protocol and SQL dialect support are derived from openHalo
 * (https://github.com/pgsty/openhalo), GPL-3.0.
 *
 *-------------------------------------------------------------------------
 */
#ifndef HALO_MYSQL_H
#define HALO_MYSQL_H

#include "nodes/pg_list.h"
#include "mys_binary.h"
#ifndef HALO_BABELFISH_API
#include "postmaster/protocol_extension.h"
#endif

/* GUC variables */
extern int	halo_mysql_port;
extern int	halo_mysql_max_allowed_packet;
extern char *halo_mysql_listen_addresses;
extern char *halo_mysql_database;
extern bool halo_mysql_listener_on;
extern char *halo_mysql_version;	/* mysql.halo_mysql_version (mys_compat.c) */
extern int	halo_mysql_parser;

typedef enum
{
	MYS_PARSER_MYSQL = 0,
	MYS_PARSER_POSTGRES
} MysParserMode;

/* protocol id assigned by RegisterProtocolExtension() */
extern int	halo_mysql_protocol_id;

/* mysql_proto.c */
#ifdef HALO_BABELFISH_API
extern void mysql_babelfish_register(void);	/* Babelfish's protocol API */
#else
extern const ProtocolExtensionConfig mysql_protocol_config;
#endif
extern void mysql_listen_init(void);
extern bool mys_is_mysql_session(void);
extern void halo_mysql_ensure_extension(void);
extern void halo_mysql_lock_setup(void);
extern bool mys_foreign_key_checks(void);
extern void mys_sync_foreign_key_checks(void);
extern void mys_sync_session_state(void);
extern void mys_start_implicit_transaction(bool ddl);
extern const char *mys_transaction_start(int explicit_read_only, bool *snapshot, bool *read_only,
										 bool explicit_begin);
extern bool mys_transaction_active(void);
extern bool mys_transaction_has_block(void);
extern void mys_statement_done(bool end, bool next_ddl);
extern void mys_statement_touched_tables(void);
extern bool mys_server_read_only;
extern bool mys_server_read_only_applies(void);
extern void mys_flush_rewrite_warnings(List *warnings);
extern void mys_discard_rewrite_warnings(void);
extern bool mys_index_prefix(Node *expr, AttrNumber *attnum, int *length, bool *bytes,
							 bool *digest);

/* mys_rewrite.c: MySQL key names, per table (see there) */
extern char *mys_key_name(const char *relname, bool primary);
extern char *mys_index_key_name(Oid indexoid);
extern Oid	mys_find_key(Oid relid, const char *name);
extern char *mys_key_pg_name(const char *relname, const char *key, Oid nspid, List *reserved);
extern Node *mys_name_keys(Node *stmt);

/*
 * SQLSTATEs for MySQL errors that no PostgreSQL SQLSTATE maps to: "M" and the
 * MySQL error number (mys_errno()).  The client gets SQLSTATE 42000, which
 * MySQL sends for each of them.
 */
#define MYS_ERRCODE_DUP_KEYNAME			MAKE_SQLSTATE('M','1','0','6','1')
#define MYS_ERRCODE_KEY_DOES_NOT_EXIST	MAKE_SQLSTATE('M','1','1','7','6')
#define MYS_ERRCODE_WRONG_NAME_FOR_INDEX	MAKE_SQLSTATE('M','1','2','8','0')
#define MYS_ERRCODE_TXN_CHARACTERISTICS	MAKE_SQLSTATE('M','1','5','6','8')

/* how a MySQL statement interacts with transactions (mys_txn.c) */
typedef enum MysTxnKind
{
	MYS_TXN_REGULAR = 0,		/* starts a transaction when autocommit = 0 */
	MYS_TXN_NEUTRAL,			/* SET, SHOW, COMMIT AND CHAIN ... */
	MYS_TXN_BEGIN,				/* BEGIN / START TRANSACTION */
	MYS_TXN_END,				/* COMMIT / ROLLBACK */
	MYS_TXN_IMPLICIT_COMMIT,	/* DDL and friends */
	MYS_TXN_SET_AUTOCOMMIT,		/* SET autocommit = ... */
	MYS_TXN_SAVEPOINT			/* SAVEPOINT / RELEASE / ROLLBACK TO */
} MysTxnKind;

/* hidden statements inserted by mys_txn.c */
typedef enum MysTxnOp
{
	MYS_TXNOP_NONE = 0,
	MYS_TXNOP_BEGIN,
	MYS_TXNOP_COMMIT,
	MYS_TXNOP_COMMIT_IDLE,	/* finish an unused autocommit=0 block */
	MYS_TXNOP_SAVEPOINT,		/* statement-level rollback point */
	MYS_TXNOP_RELEASE
} MysTxnOp;

/*
 * INSERT ... ON DUPLICATE KEY UPDATE / REPLACE: the last column of the
 * hidden RETURNING tells each row's MysUpsertRow, and the affected rows
 * count as in MySQL (2 for an updated or replaced row).
 */
typedef enum MysUpsertKind
{
	MYS_UPSERT_NONE = 0,
	MYS_UPSERT_UPDATE,			/* ON DUPLICATE KEY UPDATE */
	MYS_UPSERT_REPLACE,			/* REPLACE: an identical row counts 1 */
	MYS_UPSERT_REPLACE_DELETE	/* REPLACE that MySQL runs as DELETE + INSERT */
} MysUpsertKind;

typedef enum MysUpsertRow
{
	MYS_ROW_INSERTED = 0,
	MYS_ROW_CHANGED,
	MYS_ROW_UNCHANGED
} MysUpsertRow;

/* per-statement flags computed by mys_rewrite.c, consumed by mysql_proto.c */
typedef struct MysStmtInfo
{
	bool		silent;				/* SET ...: send OK, not a result set */
	bool		capture_insert_id;	/* hidden RETURNING of the AUTO_INCREMENT column */
	uint8		upsert;				/* MysUpsertKind: hidden RETURNING of MysUpsertRow */
	bool		suppress_response;	/* helper statement of a multi-statement rewrite */
	bool		continued;			/* EXECUTE: its statement answers, as the next query */
	bool		group_start;		/* first PG statement of a MySQL statement */
	bool		txn_control;		/* hidden statement (mys_txn.c) */
	uint8		txn_op;				/* MysTxnOp of a hidden statement */
	uint8		txn_kind;			/* MysTxnKind of the MySQL statement */
	int8		set_autocommit;		/* SET autocommit = 0 / 1, -1 unknown */
	bool		is_commit;			/* the client's COMMIT */
	bool		is_write;			/* INSERT / UPDATE (strict mode rules) */
	bool		ignore;				/* INSERT IGNORE: lenient assignments */
	bool		diagnostic;			/* SHOW WARNINGS / ERRORS preserves diagnostics */
	List	   *update_cols;		/* columns assigned by UPDATE / ON DUPLICATE KEY */
	List	   *rewrite_warnings;	/* diagnostics belonging to this statement */
} MysStmtInfo;

/* mysql_proto.c */
extern MysStmtInfo *mys_current_stmt_info(void);
extern void mys_add_warning(uint16 code, const char *message);
extern uint64 mys_upsert_row(struct TupleTableSlot *oldslot,
						   struct TupleTableSlot *newslot, uint64 deleted);

/*
 * MySQL errors that have no PostgreSQL SQLSTATE: mys_errno() maps these
 * implementation-defined subclasses, and the client gets MySQL's SQLSTATE.
 */
#define ERRCODE_MYS_USER_LOCK_WRONG_NAME	MAKE_SQLSTATE('4','2','M','5','7')	/* 3057, 42000 */
#define ERRCODE_MYS_USER_LOCK_DEADLOCK		MAKE_SQLSTATE('4','0','M','5','8')	/* 3058, 40001 */

/* mys_lock.c: GET_LOCK() and friends */
extern int64 mys_release_user_locks(void);

/* mys_binary.c: hexadecimal literals (mysql.hexlit), HEX(), UNHEX() */
extern bool mys_hexlit_available;
extern void mys_binary_session_init(void);
extern Oid	mys_binary_base_type(Oid typid);
extern bytea *mys_bpchar_bytes(Datum value);
extern Node *mys_fold_hexlit_default(Node *expr, Oid typid);
extern void mys_hexlit_columns_binary(Node *query);

/* mys_semantics.c */
extern void mys_set_auto_increment(Oid relid, int64 next);
extern void mys_sync_auto_increment(Oid relid);
extern void mys_sequence_write(struct RelationData *sequence);

/* mys_rewrite.c: ENGINE= / ENCRYPTION= -> table access method, or NULL */
struct DefElem;
struct ColumnDef;
extern char *mys_engine_access_method(struct DefElem *d);
extern void mys_rectify_column_collation(struct ColumnDef *col, const char *collation,
										 bool rewriting);
extern const char *mys_table_default_collation(Oid relid);

/* mys_utility.c: per-table setup after MySQL DDL (triggers) */
#define MYS_TABLE_SETUP_MARKER	"halo_mysql_table_setup"
/* aux_mysql security label of a generated column declared VIRTUAL (stored) */
#define MYS_GENERATED_VIRTUAL	"virtual"
/* IMMUTABLE SQL function computing a generated column PostgreSQL finds mutable */
#define MYS_GENERATOR_PREFIX	"aux_mysql_gen_"
extern Node *mys_unwrap_generator(Node *expr);
/* mysql.generated_column(name, expr): a generated column read by another */
#define MYS_GENERATED_REF		"generated_column"
extern bool mys_generated_virtual(Oid relid, AttrNumber attnum);
extern char *halo_mysql_default_collation;

/* mys_txn.c */
extern bool halo_mysql_statement_rollback;
extern void mys_txn_classify(Node *stmt, MysStmtInfo *info);
extern List *mys_txn_apply(List *stmts, List **infos);
extern void mys_txn_stmt_done(MysStmtInfo *info);
extern bool mys_txn_skip_control(MysStmtInfo *info);
extern void mys_txn_recover(void);
extern List *mys_txn_deferred_error(ErrorData *edata);
extern bool mys_txn_is_deferred_error(Node *stmt);
extern void mys_txn_raise_deferred_error(void);

/* authentication plugin announced in the greeting */
typedef enum
{
	MYS_AUTH_CACHING_SHA2 = 0,
	MYS_AUTH_CLEARTEXT			/* legacy: AuthSwitch to mysql_clear_password */
} MysAuthPlugin;

extern int	halo_mysql_auth_plugin;
extern int	halo_mysql_auth_cache_size;
extern char *halo_mysql_rsa_private_key_file;

/* mys_auth.c */
extern Size mys_auth_shmem_size(void);
extern void mys_auth_shmem_request(void);
extern void mys_auth_shmem_startup(void);
extern void mys_auth_postmaster_init(void);
extern bool mys_sha2_fast_auth(const char *user, const char *shadow_pass,
							   const char *nonce, int nonce_len,
							   const uint8 *resp, int resp_len);
extern void mys_sha2_cache_store(const char *user, const char *shadow_pass,
								 const char *password);
extern const char *mys_rsa_public_key(int *len);
extern char *mys_rsa_decrypt_password(const uint8 *in, int inlen,
									  const char *nonce, int nonce_len);

/*
 * Position map between a statement text with '?' markers (as prepared) and
 * the text that is parsed (markers replaced by literals); lets mys_rewrite.c
 * name result columns after the original text, as MySQL does.
 */
typedef struct MysTextMap
{
	const char *orig;
	int			nparams;
	const int  *orig_loc;		/* offset of each '?' in orig */
	const int  *new_loc;		/* offset of its replacement in the parsed text */
	const int  *new_len;		/* length of the replacement */
} MysTextMap;

/* mys_stmt.c: set while a prepared statement's text is parsed */
extern MysTextMap *mys_text_map;

/* mys_stmt.c: SQL PREPARE / EXECUTE / DEALLOCATE PREPARE */
extern Node *mys_sql_ps_marker(Node *stmt, const char *rest);
extern bool mys_sql_ps_utility(Node *parsetree, const char *queryString);

/* mysql_proto.c: queries to run before the next command is read */
extern void mys_queue_query(const char *query, const MysTextMap *map, const char *rest);

/* SHOW CREATE metadata, called by mys_rewrite_raw_stmts inside a transaction. */
extern Node *mys_rewrite_show_create(Node *stmt);
/* Implemented by the binary worker; includes the new bytea-layout base types. */
extern Oid mys_binary_base_type(Oid type);

/* mys_rewrite.c */
extern List *mys_rewrite_raw_stmts(List *raw_stmts, const char *query_string,
								   List **infos);

/* mys_utility.c */
extern void mysql_utility_init(void);

/* mysql_parser.c */
extern List *mysql_raw_parser(const char *str);
extern char *mys_normalize_query(const char *query, int len);

#endif							/* HALO_MYSQL_H */
