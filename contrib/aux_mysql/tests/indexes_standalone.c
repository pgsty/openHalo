/* Test-only bridge: feed the same PRIMARY/UNIQUE Constraint nodes produced
 * by the MySQL rewriter into PostgreSQL's normal utility transformation.
 * This is not built into aux_mysql. */
#include "postgres.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "parser/parser.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "mys_prefix.h"

/* Catalog lookup seam, for connections spanning an extension SQL upgrade. */
PG_FUNCTION_INFO_V1(mys_prefix_test_available);
Datum
mys_prefix_test_available(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s,%s,%s,%s",
		mys_prefix_collation_name(false), mys_prefix_collation_name(true),
		mys_prefix_available() ? "yes" : "no",
		mys_prefix_type_available(PG_GETARG_OID(0)) ? "yes" : "no")));
}

PG_FUNCTION_INFO_V1(mys_prefix_test_ddl);
Datum
mys_prefix_test_ddl(PG_FUNCTION_ARGS)
{
	char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List *raw = raw_parser(sql, RAW_PARSE_DEFAULT);
	RawStmt *stmt = linitial_node(RawStmt, raw);
	PlannedStmt *plan = makeNode(PlannedStmt);
	CreateStmt *create;
	QueryCompletion completion;

	if (list_length(raw) != 1 || !IsA(stmt->stmt, CreateStmt))
		elog(ERROR, "test bridge accepts one CREATE TABLE");
	create = (CreateStmt *) stmt->stmt;
	foreach_ptr(Node, elt, create->tableElts)
	{
		List *constraints = IsA(elt, ColumnDef) ? ((ColumnDef *) elt)->constraints : list_make1(elt);
		foreach_ptr(Node, node, constraints)
			if (IsA(node, Constraint))
			{
				Constraint *c = (Constraint *) node;
				if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
					c->access_method = "mysql_prefix";
			}
	}
	plan->commandType = CMD_UTILITY;
	plan->canSetTag = true;
	plan->utilityStmt = stmt->stmt;
	plan->stmt_location = stmt->stmt_location;
	plan->stmt_len = stmt->stmt_len;
	ProcessUtility(plan, sql, false, PROCESS_UTILITY_QUERY, NULL, NULL, None_Receiver, &completion);
	PG_RETURN_VOID();
}
