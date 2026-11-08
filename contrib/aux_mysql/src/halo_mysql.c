/*-------------------------------------------------------------------------
 *
 * halo_mysql.c
 *	  aux_mysql module entry point: GUCs and protocol registration.
 *
 * openHalo kept its MySQL settings in the core GUC table (mysql.listener_on,
 * mysql.port, mysql.halo_mysql_version, ...); aux_mysql defines them under
 * the same names.  The "mysql" prefix is not reserved, so other mysql.*
 * lines carried over from an openHalo postgresql.conf stay harmless.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

#include "halo_mysql.h"

PG_MODULE_MAGIC_EXT(.name = "aux_mysql", .version = "1.6");

bool		halo_mysql_listener_on = true;
int			halo_mysql_port = 3306;
int			halo_mysql_max_allowed_packet = 64 * 1024 * 1024;
char	   *halo_mysql_listen_addresses = NULL;
char	   *halo_mysql_database = NULL;
int			halo_mysql_parser = MYS_PARSER_MYSQL;
int			halo_mysql_protocol_id = 0;
int			halo_mysql_auth_plugin = MYS_AUTH_CACHING_SHA2;
static bool halo_mysql_auto_create_extension = true;

/* openHalo shared state (sysvar_core.c, uuid_short_core.c) */
extern Size GlobalSystemVariablesShmemSize(void);
extern void GlobalSystemVariablesShmemInit(void);
extern Size UuidShortShmemSize(void);
extern void UuidShortShmemInit(void);

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
halo_mysql_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(add_size(add_size(GlobalSystemVariablesShmemSize(),
											 UuidShortShmemSize()),
									mys_auth_shmem_size()));
	mys_auth_shmem_request();
}

static void
halo_mysql_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	GlobalSystemVariablesShmemInit();
	UuidShortShmemInit();
	mys_auth_shmem_startup();
	LWLockRelease(AddinShmemInitLock);
}

static const struct config_enum_entry auth_plugin_options[] = {
	{"caching_sha2_password", MYS_AUTH_CACHING_SHA2, false},
	{"mysql_clear_password", MYS_AUTH_CLEARTEXT, false},
	{NULL, 0, false}
};

static const struct config_enum_entry parser_options[] = {
	{"mysql", MYS_PARSER_MYSQL, false},
	{"postgres", MYS_PARSER_POSTGRES, false},
	{"postgresql", MYS_PARSER_POSTGRES, true},
	{NULL, 0, false}
};

/* Serialize automatic and explicit installation in this database. */
void
halo_mysql_lock_setup(void)
{
	LockDatabaseObject(ExtensionRelationId, InvalidOid, 0, AccessExclusiveLock);
}

/*
 * Only a MySQL connection requests installation. PG restore/upgrade sessions
 * never create objects as a side effect of starting the server. Run as the
 * authenticated role, and finish before acknowledging the MySQL handshake.
 */
void
halo_mysql_ensure_extension(void)
{
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	if (OidIsValid(get_extension_oid("aux_mysql", true)))
	{
		CommitTransactionCommand();
		return;
	}
	if (!halo_mysql_auto_create_extension || IsBinaryUpgrade || RecoveryInProgress())
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("aux_mysql is not installed in database \"%s\"", halo_mysql_database),
				 errhint("Install aux_mysql on the primary as a superuser before connecting.")));
	if (!superuser())
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("a superuser must install aux_mysql before the first MySQL connection"),
				 errhint("Run CREATE EXTENSION aux_mysql in database \"%s\".", halo_mysql_database)));
	halo_mysql_lock_setup();
	PushActiveSnapshot(GetTransactionSnapshot());
	if (!OidIsValid(get_extension_oid("aux_mysql", true)))
	{
		const char *sql = "CREATE EXTENSION IF NOT EXISTS aux_mysql";

		pgstat_report_activity(STATE_RUNNING, sql);
		debug_query_string = sql;
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");
		if (SPI_execute(sql, false, 0) != SPI_OK_UTILITY)
			elog(ERROR, "could not create extension aux_mysql");
		SPI_finish();
		ereport(LOG,
				(errmsg("aux_mysql: created extension aux_mysql in database \"%s\"",
						halo_mysql_database)));
	}
	PopActiveSnapshot();
	CommitTransactionCommand();
	pgstat_report_activity(STATE_IDLE, NULL);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("aux_mysql must be loaded via \"shared_preload_libraries\"")));

	DefineCustomBoolVariable("mysql.listener_on",
							 "Enable the MySQL protocol listener.",
							 NULL,
							 &halo_mysql_listener_on,
							 true,
							 PGC_POSTMASTER, 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("mysql.port",
							"Sets the MySQL TCP port the server listens on.",
							"0 disables the listener.",
							&halo_mysql_port,
							3306, 0, 65535,
							PGC_POSTMASTER, 0,
							NULL, NULL, NULL);

	DefineCustomStringVariable("mysql.listen_addresses",
							   "Addresses for the MySQL protocol listener.",
							   "Defaults to listen_addresses.",
							   &halo_mysql_listen_addresses,
							   NULL,
							   PGC_POSTMASTER, GUC_LIST_INPUT,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("mysql.max_allowed_packet",
							"Maximum incoming MySQL message or accumulated long-data bytes.",
							"Set in postgresql.conf; reload to apply.",
							&halo_mysql_max_allowed_packet,
							64 * 1024 * 1024, 1024, 1024 * 1024 * 1024 - 1,
							PGC_SIGHUP, 0, NULL, NULL, NULL);

	DefineCustomStringVariable("mysql.database",
							   "PostgreSQL database that MySQL clients connect to.",
							   "MySQL databases are mapped to schemas inside this database.",
							   &halo_mysql_database,
							   "postgres",
							   PGC_SIGHUP, 0,
							   NULL, NULL, NULL);

	DefineCustomStringVariable("mysql.halo_mysql_version",
							   "Sets the mysql version the server support.",
							   "Reported in the handshake, by version() and @@version.",
							   &halo_mysql_version,
							   "5.7.32-log",
							   PGC_SIGHUP, 0,
							   NULL, NULL, NULL);

	DefineCustomEnumVariable("mysql.parser",
							 "SQL dialect used for queries received over the MySQL protocol.",
							 NULL,
							 &halo_mysql_parser,
							 MYS_PARSER_MYSQL,
							 parser_options,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomEnumVariable("mysql.default_authentication_plugin",
							 "Authentication plugin announced to MySQL clients.",
							 "caching_sha2_password checks the PostgreSQL password verifier "
							 "after a TLS or RSA protected full authentication, then uses "
							 "the fast-auth cache.  mysql_clear_password is the legacy mode.",
							 &halo_mysql_auth_plugin,
							 MYS_AUTH_CACHING_SHA2,
							 auth_plugin_options,
							 PGC_SIGHUP, 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("mysql.auth_cache_size",
							"Number of users in the caching_sha2_password fast-auth cache.",
							"0 disables the cache: every login does a full authentication.",
							&halo_mysql_auth_cache_size,
							1024, 0, 1000000,
							PGC_POSTMASTER, 0,
							NULL, NULL, NULL);

	DefineCustomStringVariable("mysql.rsa_private_key_file",
							   "RSA private key for the caching_sha2_password public key exchange.",
							   "Relative paths are relative to the data directory; "
							   "the key pair is generated there if the file does not exist.",
							   &halo_mysql_rsa_private_key_file,
							   "mysql_rsa_private_key.pem",
							   PGC_POSTMASTER, 0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("mysql.statement_rollback",
							 "Roll back only the failing statement inside a transaction, as MySQL does.",
							 "Each statement in a transaction block runs under a hidden savepoint. "
							 "Off: an error aborts the whole transaction (PostgreSQL behavior).",
							 &halo_mysql_statement_rollback,
							 true,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("mysql.default_collation",
							   "Collation (in schema mysql) of string columns created without COLLATE.",
							   "MySQL compares strings case-insensitively by default. "
							   "Empty: keep the database default collation.",
							   &halo_mysql_default_collation,
							   "case_insensitive",
							   PGC_USERSET, 0,
							   NULL, NULL, NULL);

	DefineCustomBoolVariable("mysql.auto_create_extension",
							 "Install aux_mysql on the first MySQL connection by a superuser.",
							 "Disabled on standbys and during pg_upgrade. PostgreSQL connections "
							 "never install the extension automatically.",
							 &halo_mysql_auto_create_extension,
							 true,
							 PGC_POSTMASTER, 0,
							 NULL, NULL, NULL);

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = halo_mysql_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = halo_mysql_shmem_startup;

	mysql_utility_init();

#ifdef HALO_BABELFISH_API
	mysql_babelfish_register();
#else
	halo_mysql_protocol_id = RegisterProtocolExtension(&mysql_protocol_config);
#endif
}
