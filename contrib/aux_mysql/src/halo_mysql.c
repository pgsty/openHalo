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
#include "commands/extension.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

#include "halo_mysql.h"

PG_MODULE_MAGIC_EXT(.name = "aux_mysql", .version = "1.5");

bool		halo_mysql_listener_on = true;
int			halo_mysql_port = 3306;
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

/*
 * "aux_mysql setup" background worker: create the extension objects in
 * mysql.database once the server accepts connections, so that MySQL
 * clients work right after initdb and start (openHalo needed a manual
 * CREATE EXTENSION aux_mysql).  Runs once per postmaster start, as the
 * bootstrap superuser; does nothing if the extension is already there.
 */
PGDLLEXPORT void halo_mysql_setup_main(Datum main_arg);

void
halo_mysql_setup_main(Datum main_arg)
{
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(halo_mysql_database, NULL, 0);

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
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
	proc_exit(0);
}

static void
register_setup_worker(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	/* not on a standby (the extension comes with replication) */
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "aux_mysql");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "halo_mysql_setup_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "aux_mysql setup");
	snprintf(worker.bgw_type, BGW_MAXLEN, "aux_mysql setup");
	RegisterBackgroundWorker(&worker);
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
							 "Create the aux_mysql extension in mysql.database at startup.",
							 "A background worker runs CREATE EXTENSION IF NOT EXISTS aux_mysql "
							 "once the server accepts connections (not on standbys, not during "
							 "pg_upgrade).",
							 &halo_mysql_auto_create_extension,
							 true,
							 PGC_POSTMASTER, 0,
							 NULL, NULL, NULL);

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = halo_mysql_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = halo_mysql_shmem_startup;

	mysql_utility_init();

	/* pg_upgrade restores the extension itself: do not create it first */
	if (halo_mysql_auto_create_extension && !IsBinaryUpgrade &&
		halo_mysql_database != NULL && halo_mysql_database[0] != '\0')
		register_setup_worker();

#ifdef HALO_BABELFISH_API
	mysql_babelfish_register();
#else
	halo_mysql_protocol_id = RegisterProtocolExtension(&mysql_protocol_config);
#endif
}
