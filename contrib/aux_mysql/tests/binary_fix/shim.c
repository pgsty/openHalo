/* Local, isolated test module. Never install in a shared openHalo instance. */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "halo_mysql.h"
#include "mys_prefix.h"
#include "adapter/mysql/systemVar.h"
PG_MODULE_MAGIC;
uint64 mys_sqlMode = MYS_MODE_STRICT_TRANS_TABLES;
static bool test_mysql = false;
static bool test_strict = true;
static bool test_ignore = false;
static bool test_ignoring(void) { return test_ignore; }
void _PG_init(void);
void _PG_init(void)
{
	mys_prefix_init();
	DefineCustomBoolVariable("review_fix.mysql_session", "test protocol", NULL,
							 &test_mysql, false, PGC_USERSET, 0, NULL, NULL, NULL);
	mys_binary_ignore_hook = test_ignoring;
	DefineCustomBoolVariable("review_fix.ignore", "test IGNORE flag", NULL,
							 &test_ignore, false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("review_fix.strict", "test sql_mode", NULL,
							 &test_strict, true, PGC_USERSET, 0, NULL, NULL, NULL);
}
bool mys_is_mysql_session(void)
{
	mys_sqlMode = test_strict ? MYS_MODE_STRICT_TRANS_TABLES : 0;
	return test_mysql;
}
void mys_add_warning(uint16 code, const char *message)
{
	ereport(WARNING, (errmsg("%d: %s", code, message)));
}

/* Exercise regenerated grammar + PostgreSQL analysis/execution without an
 * openHalo listener. The real protocol and rewrite layer are not mocked as
 * tested here; their exact integration changes are supplied separately. */
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "parser/analyze.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
extern List *mys_raw_parser(const char *str);
char *halo_mysql_version = "5.7.32";
int halo_mysql_parser = MYS_PARSER_MYSQL;
bool isSystemVariable(char *name) { return false; }
Oid getCurrentNamespaceOid(void) { return get_namespace_oid("review_fix_binary", true); }

PG_FUNCTION_INFO_V1(review_fix_value);
Datum review_fix_value(PG_FUNCTION_ARGS)
{
	char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	MemoryContext caller = CurrentMemoryContext;
	List *raw;
	Query *query;
	char *pgsql;
	text *out;
	bool withtype = PG_NARGS() > 1 && PG_GETARG_BOOL(1);

	mys_binary_session_init();
	raw = mys_raw_parser(sql);
	if (list_length(raw) != 1)
		elog(ERROR, "test requires exactly one statement");
	query = parse_analyze_fixedparams(linitial_node(RawStmt, raw), sql, NULL, 0, NULL);
	pgsql = pg_get_querydef(query, false);
	SPI_connect();
	SPI_execute(pgsql, true, 1);
	if (SPI_processed != 1 || SPI_tuptable == NULL)
		elog(ERROR, "test requires one result row");
	MemoryContextSwitchTo(caller);
	out = cstring_to_text(psprintf("%s%s%s",
		withtype ? format_type_be(SPI_gettypeid(SPI_tuptable->tupdesc, 1)) : "",
		withtype ? "|" : "",
		SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1)));
	SPI_finish();
	PG_RETURN_TEXT_P(out);
}
