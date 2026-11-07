/*-------------------------------------------------------------------------
 *
 * mys_compat.c
 *	  Functions that openHalo kept in its modified PostgreSQL core and that
 *	  the MySQL grammar depends on.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "nodes/value.h"
#include "utils/builtins.h"

#include "halo_mysql.h"
#include "adapter/mysql/adapter.h"

/*
 * MySQL session state that openHalo kept in its modified backend
 * (adapter/mysql/adapter.c, tcop/postgres.c).
 */
char	   *halo_mysql_version = NULL;	/* GUC mysql.halo_mysql_version */
bool		halo_mysql_explicit_defaults_for_timestamp = true;
bool		halo_mysql_auto_rollback_tx_on_error = false;
int			moreResultsFlag = 0;
int			warnings = 0;
unsigned long foundRows = 0;
long long	affectedRows = 0;
unsigned long lastInsertID = 0;
bool		isStrictTransTablesOn = true;
Oid			caseInsensitiveId = InvalidOid;
bool		needCommitTrx = false;
bool		needStartNewTrx = false;

/* OID of the case-insensitive collation used for MySQL *_ci collations */
Oid
getCaseInsensitiveId(void)
{
	if (!OidIsValid(caseInsensitiveId))
		caseInsensitiveId = get_collation_oid(list_make2(makeString("mysql"),
														 makeString("case_insensitive")), true);
	return caseInsensitiveId;
}

extern Oid	getCurrentNamespaceOid(void);

/*
 * The schema MySQL considers the "current database": the first schema of
 * the search path.
 */
Oid
getCurrentNamespaceOid(void)
{
	List	   *path = fetch_search_path(false);

	if (path == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_SCHEMA_NAME),
				 errmsg("No database selected")));
	return linitial_oid(path);
}
