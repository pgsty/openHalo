/* Generated-column policy, private to mys_utility.c.  Check before planning:
 * several compatibility getters have historically been marked IMMUTABLE.
 * Volatility alone is neither a permission nor a reason to reject a builtin.
 */
#ifndef MYS_GENERATED_POLICY_H
#define MYS_GENERATED_POLICY_H

#include "access/transam.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_namespace_d.h"
#include "commands/extension.h"
#include "common/cryptohash.h"
#include "parser/analyze.h"
#include "utils/typcache.h"
#include "utils/regproc.h"

/* These are implementation symbols, not caller-supplied SQL function names. */
static const char *const gen_c_implementations[] = {
	"text_concat", "text_concat_ws", "mys_concat_one", "mys_raw_bytes",
	"mys_cast_char", "mys_convert_using", "mys_string_result", "mys_concat_ws_bytes", "mys_elt_index",
	"mys_generated_varchar", "mys_generated_numeric",
	"mys_div_int8", "mys_div_numeric", "mys_div_float8",
	"mys_mod_int8", "mys_mod_numeric", "mys_binary_truth",
	"mys_binary_coerce", "mys_binary_in",
	"mys_hexlit_in", "mys_hexlit_out", "mys_hexlit_numeric", "mys_hexlit_int8",
	"mys_hexlit_text", "mys_hexlit_varbit", "mys_hexlit_bit", "mys_hexlit_bool",
	"mys_hex_bytes", "mys_hex_int8", "mys_hex_numeric", "mys_hex_float8",
	"mys_hex_varbit", "mys_unhex",
	"mys_varbit_numeric", "mys_uint64_bitop", "mys_uint64_bitnot",
	"mys_string_substr", "mys_string_replace", "mys_string_trim",
	"mys_string_locate", "mys_string_instr",
	"mys_binary_left", "mys_binary_right", "mys_binary_ascii",
	"text_substr", "text_substr_no_len", "text_left", "text_right",
	"text_instr", "text_locate2", "text_locate3", "substringIndex",
	"texteq_mys", "textne_mys", "char_eq_char_for_date_format",
	NULL
};

/* Exact SQL sources shipped by aux_mysql; their resolved bodies are checked
 * recursively as well.  No arbitrary SQL UDF becomes trusted via IMMUTABLE. */
static const char *const gen_sql_bodies[] = {
	"SELECT $1", "SELECT $2", "SELECT coalesce($1, $2)",
	"\nSELECT coalesce(p1::text, p2);\n",
	"SELECT trunc(mysql.mys_div_numeric($1, $2))::int8",
	"SELECT mysql.int_div(mysql.hexlit_numeric($1), $2)",
	"SELECT mysql.int_div($1, mysql.hexlit_numeric($2))",
	"SELECT mysql.int_div(mysql.hexlit_numeric($1), mysql.hexlit_numeric($2))",
	"SELECT mysql.mys_div_int8($1, $2)",
	"\n    SELECT mysql.DATE_FORMAT($1::timestamp without time zone, $2)\n",
	"\n  SELECT (_calc_week(DATE($1), _week_mode($2)))[1];\n",
	"\n  SELECT week($1, 0);\n",
	"\n  SELECT (_calc_week($1::date, _week_mode($2)))[1];\n",
	"\n  SELECT mysql.week($1, 0);\n",
	NULL
};

typedef struct GenPolicy
{
	List *active;
	bool stored;                 /* allow inspected legacy generator bodies */
} GenPolicy;

static bool gen_policy_walk(Node *node, GenPolicy *policy);
static void gen_policy_function(Oid oid, Node *call, GenPolicy *policy);

static void
gen_policy_error(const char *detail)
{
	ereport(ERROR, (errcode(MAKE_SQLSTATE('M','3','1','0','2')),
					errmsg("Expression of generated column contains a disallowed function or construct"),
					errdetail_internal("%s", detail)));
}

static bool
gen_string_member(const char *value, const char *const *values)
{
	for (int i = 0; values[i]; i++)
		if (strcmp(value, values[i]) == 0)
			return true;
	return false;
}

static bool
gen_relative_time(const char *s)
{
	static const char *const words[] = {"now", "today", "tomorrow", "yesterday", NULL};
	char *copy = pstrdup(s);
	char *end;

	while (isspace((unsigned char) *copy)) copy++;
	end = copy + strlen(copy);
	while (end > copy && isspace((unsigned char) end[-1])) *--end = '\0';
	for (int i = 0; words[i]; i++)
		if (pg_strcasecmp(copy, words[i]) == 0)
			return true;
	return false;
}

static bool
gen_temporal(Oid type)
{
	type = getBaseType(type);
	return type == DATEOID || type == TIMEOID || type == TIMETZOID ||
		type == TIMESTAMPOID || type == TIMESTAMPTZOID;
}

/* Unknown literals are consumed by type input during parse analysis. */
static bool
gen_policy_raw(Node *node, void *unused)
{
	if (!node) return false;
	if (IsA(node, SQLValueFunction) || IsA(node, SubLink))
		gen_policy_error("current time/user or subquery");
	if (IsA(node, TypeCast))
	{
		TypeCast *cast = (TypeCast *) node;
		Node *arg = cast->arg;

		if (IsA(arg, A_Const) && !((A_Const *) arg)->isnull &&
			IsA(&((A_Const *) arg)->val, String) &&
			gen_relative_time(strVal(&((A_Const *) arg)->val)) &&
			gen_temporal(typenameTypeId(NULL, cast->typeName)))
			gen_policy_error("relative date/time literal");
	}
	return raw_expression_tree_walker(node, gen_policy_raw, unused);
}

static bool
gen_body_hash(const char *src, const char *expected)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	uint8 digest[32];
	char hex[65];

	if (!ctx || pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, (const uint8 *) src, strlen(src)) < 0 ||
		pg_cryptohash_final(ctx, digest, sizeof(digest)) < 0)
		elog(ERROR, "could not hash generated-column function body");
	pg_cryptohash_free(ctx);
	for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
	return strcmp(hex, expected) == 0;
}

/* PL/pgSQL is not a general expression language.  Only these reviewed bodies
 * are admitted, by full body fingerprint and extension-owned identity. */
static bool
gen_plpgsql_body(const char *name, const char *src)
{
	static const struct { const char *name; const char *hash; } approved[] = {
		{"ifnull", "91720b3708b11c7296064904cb25e8a63a3c643e0b051f0f544464fc10a3e903"},
		{"ifnull", "e990a89156782529fbc3153c6ff16a4ec31ffa8a3f3c3be3a1a401853fe3f013"},
		{"ifnull", "180e2aaa6fa6db564f41335ee2f16ac4e885908d39e35bb6b8d33ea807805007"},
		{"ifnull", "6b849830b24f98a79fb6a713228c19e175ddcc3507c07a81a1a17e42f8be9e34"},
		{"date_format", "388256fa53cc53502cf81bb77328942d5fe889ba1bcab5a54915bff484049b23"},
		{"_calc_week", "f9264146d2c01a15fe91375b0b90f318ecbea999da0eff945ff838fcb50fb174"},
		{"_week_mode", "931981c45eb3b87ae933c20cd7b0d87fb4fdf3681047b679761ae2cf529b6a66"},
		{"_calc_weekday", "861905ad854edee6c5bdcde381f1660ed1abe64d2c1d4529d7948e9ce299c9b6"},
		{"_calc_days_in_year", "aa12eb7bcf916d4ef690d5a4f67b707a2d81c95151802dfa51a6713851452ded"}
	};
	for (int i = 0; i < lengthof(approved); i++)
		if (strcmp(name, approved[i].name) == 0 && gen_body_hash(src, approved[i].hash))
			return true;
	return false;
}

static Node *
gen_substitute(Node *node, List *args)
{
	if (!node) return NULL;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN &&
		((Param *) node)->paramid > 0 && ((Param *) node)->paramid <= list_length(args))
	{
		Node *arg = list_nth(args, ((Param *) node)->paramid - 1);

		/* Propagate literal values for relative-time checks. Callable
		 * arguments are inspected separately at the call site: expanding
		 * them into the body would mistake f(f(x)) for recursive SQL. */
		if (IsA(arg, Const))
			return copyObject(arg);
	}
	return expression_tree_mutator(node, gen_substitute, args);
}

static void
gen_policy_query(Query *query, Node *call, GenPolicy *policy)
{
	Node *body;

	if (!IsA(query, Query) || query->commandType != CMD_SELECT ||
		list_length(query->targetList) != 1 || query->rtable || query->cteList ||
		query->hasAggs || query->hasWindowFuncs || query->hasSubLinks ||
		query->hasTargetSRFs || query->setOperations || query->groupClause ||
		query->havingQual || query->limitOffset || query->limitCount ||
		(query->jointree && (query->jointree->fromlist || query->jointree->quals)))
		gen_policy_error("SQL generator must be a single expression without external reads");
	body = (Node *) linitial_node(TargetEntry, query->targetList)->expr;
	if (call && IsA(call, FuncExpr))
		body = gen_substitute(body, ((FuncExpr *) call)->args);
	(void) gen_policy_walk(body, policy);
}

/* Check the known DATE_FORMAT PL/pgSQL helper closure as resolved under the
 * same fixed search_path as generators.  Other PL/pgSQL UDFs are not admitted. */
static void
gen_date_helpers(GenPolicy *policy)
{
	static const struct { const char *name; int n; Oid types[2]; } helpers[] = {
		{"week", 2, {TIMESTAMPOID, INT4OID}},
		{"_calc_week", 2, {DATEOID, INT4OID}},
		{"_week_mode", 1, {INT4OID}},
		{"_calc_weekday", 2, {DATEOID, BOOLOID}},
		{"_calc_days_in_year", 1, {INT4OID}}
	};
	for (int i = 0; i < lengthof(helpers); i++)
	{
		Oid oid = LookupFuncName(list_make2(makeString("mysql"),
			makeString(pstrdup(helpers[i].name))), helpers[i].n, helpers[i].types, false);
		if (!list_member_oid(policy->active, oid)) gen_policy_function(oid, NULL, policy);
	}
}

static void
gen_policy_function(Oid oid, Node *call, GenPolicy *policy)
{
	HeapTuple tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(oid));
	Form_pg_proc proc;
	char *src;
	bool isnull;
	bool extension;
	bool legacy;
	bool builtin_sql;
	Datum datum;

	check_stack_depth();
	if (!HeapTupleIsValid(tup)) elog(ERROR, "cache lookup failed for function %u", oid);
	proc = (Form_pg_proc) GETSTRUCT(tup);
	src = TextDatumGetCString(SysCacheGetAttrNotNull(PROCOID, tup, Anum_pg_proc_prosrc));
	extension = proc->pronamespace == get_namespace_oid("mysql", true) &&
		getExtensionOfObject(ProcedureRelationId, oid) == get_extension_oid("aux_mysql", true);
	legacy = policy->stored && proc->prolang == SQLlanguageId &&
		strncmp(NameStr(proc->proname), MYS_GENERATOR_PREFIX, strlen(MYS_GENERATOR_PREFIX)) == 0;
	/* Some immutable PG builtins (for example trunc(numeric)) are SQL
	 * wrappers. Their bootstrap identity admits the body for recursive
	 * inspection; a user function's IMMUTABLE label is not sufficient. */
	builtin_sql = oid < FirstNormalObjectId &&
		proc->pronamespace == PG_CATALOG_NAMESPACE &&
		proc->prolang == SQLlanguageId && proc->provolatile == PROVOLATILE_IMMUTABLE;
	if (proc->prokind != PROKIND_FUNCTION || proc->proretset || proc->prosecdef ||
		list_member_oid(policy->active, oid)) gen_policy_error(format_procedure(oid));
	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_proconfig, &isnull);
	if (!isnull)
	{
		ArrayType *config = DatumGetArrayTypeP(datum);
		Datum *entries; bool *nulls; int count;
		deconstruct_array(config, TEXTOID, -1, false, TYPALIGN_INT, &entries, &nulls, &count);
		if (!legacy || count != 1 || nulls[0] ||
			strcmp(TextDatumGetCString(entries[0]), "search_path=mysql, pg_catalog") != 0)
			gen_policy_error(format_procedure(oid));
	}
	if (proc->prolang == INTERNALlanguageId &&
		(proc->pronamespace == PG_CATALOG_NAMESPACE || extension))
	{
		Oid builtin = fmgr_internal_function(src);
		/* Stable temporal IO/casts support the audited DATE_FORMAT wrappers;
		 * raw/constant relative inputs are separately rejected. */
		static const char *const stable[] = {"timestamp_in", "timestamp_out", "date_in",
			"date_out", "time_in", "time_out", "timestamptz_in", "timestamptz_out",
			"timestamp_timestamptz", "timestamptz_timestamp", "timestamptz_date", NULL};
		if (OidIsValid(builtin) && (func_volatile(builtin) == PROVOLATILE_IMMUTABLE ||
			gen_string_member(src, stable)))
		{
			ReleaseSysCache(tup);
			return;
		}
	}
	if (extension && proc->prolang == ClanguageId && gen_string_member(src, gen_c_implementations))
	{
		datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_probin, &isnull);
		if (!isnull && strcmp(TextDatumGetCString(datum), "$libdir/aux_mysql") == 0)
		{
			ReleaseSysCache(tup);
			return;
		}
	}
	policy->active = lappend_oid(policy->active, oid);
	if ((extension || legacy || builtin_sql) && proc->prolang == SQLlanguageId &&
		(legacy || builtin_sql || gen_string_member(src, gen_sql_bodies)))
	{
		Node *body;
		datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
		body = isnull ? NULL : stringToNode(TextDatumGetCString(datum));
		if (body)
			gen_policy_query((Query *) body, call, policy);
		else
		{
			List *raw = raw_parser(src, RAW_PARSE_DEFAULT);
			SQLFunctionParseInfo *pinfo = prepare_sql_fn_parse_info(tup, call, InvalidOid);
			int nest = NewGUCNestLevel();
			if (list_length(raw) != 1) gen_policy_error(format_procedure(oid));
			(void) gen_policy_raw((Node *) linitial_node(RawStmt, raw)->stmt, NULL);
			PG_TRY();
			{
				set_config_option("search_path", "mysql,pg_catalog", PGC_USERSET,
					PGC_S_SESSION, GUC_ACTION_SAVE, true, 0, false);
				gen_policy_query(parse_analyze_withcb(linitial_node(RawStmt, raw), src,
					(ParserSetupHook) sql_fn_parser_setup, pinfo, NULL), call, policy);
			}
			PG_FINALLY();
			{ AtEOXact_GUC(true, nest); }
			PG_END_TRY();
		}
	}
	else if (extension && strcmp(get_language_name(proc->prolang, false), "plpgsql") == 0 &&
		gen_plpgsql_body(NameStr(proc->proname), src))
	{
		if (strcmp(NameStr(proc->proname), "date_format") == 0) gen_date_helpers(policy);
	}
	else gen_policy_error(format_procedure(oid));
	policy->active = list_delete_last(policy->active);
	ReleaseSysCache(tup);
}

static bool
gen_policy_function_cb(Oid oid, void *context)
{
	gen_policy_function(oid, NULL, context);
	return false;
}

static bool
gen_policy_walk(Node *node, GenPolicy *policy)
{
	if (!node) return false;
	check_stack_depth();
	if (IsA(node, SQLValueFunction) || IsA(node, NextValueExpr) || IsA(node, SubLink) ||
		IsA(node, Query) || IsA(node, Aggref) || IsA(node, WindowFunc))
		gen_policy_error("current time/user, sequence, subquery or aggregate");
	if (IsA(node, FuncExpr)) gen_policy_function(((FuncExpr *) node)->funcid, node, policy);
	else (void) check_functions_in_node(node, gen_policy_function_cb, policy);
	if (IsA(node, MinMaxExpr))
	{
		TypeCacheEntry *type = lookup_type_cache(((MinMaxExpr *) node)->minmaxtype, TYPECACHE_CMP_PROC);
		if (OidIsValid(type->cmp_proc)) gen_policy_function(type->cmp_proc, NULL, policy);
	}
	/* A user domain can hide arbitrary code in its CHECK constraints.  The
	 * shipped MySQL domains only implement scalar type/range constraints. */
	if (IsA(node, CoerceToDomain) &&
		getExtensionOfObject(TypeRelationId, ((CoerceToDomain *) node)->resulttype) !=
		get_extension_oid("aux_mysql", false))
		gen_policy_error("user-defined domain constraint");
	if (IsA(node, CoerceViaIO) && gen_temporal(((CoerceViaIO *) node)->resulttype))
	{
		Node *arg = strip_implicit_coercions((Node *) ((CoerceViaIO *) node)->arg);
		if (IsA(arg, Const) && !((Const *) arg)->constisnull &&
			(((Const *) arg)->consttype == TEXTOID || ((Const *) arg)->consttype == UNKNOWNOID) &&
			gen_relative_time(((Const *) arg)->consttype == UNKNOWNOID ?
				DatumGetCString(((Const *) arg)->constvalue) : TextDatumGetCString(((Const *) arg)->constvalue)))
			gen_policy_error("relative date/time literal in SQL wrapper");
	}
	return expression_tree_walker(node, gen_policy_walk, policy);
}

static void
gen_policy_check(Node *expr, bool stored)
{
	GenPolicy policy = {NIL, stored};
	(void) gen_policy_walk(expr, &policy);
}

/* Validate retained old generators before LIKE/ALTER can plan or evaluate
 * them.  DROP/MODIFY of the unsafe column remains a usable repair path. */
static void
gen_policy_table(Oid relid, List *replaced)
{
	Relation rel = table_open(relid, AccessShareLock);
	TupleConstr *constr = RelationGetDescr(rel)->constr;
	for (int i = 0; constr && i < constr->num_defval; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), constr->defval[i].adnum - 1);
		bool skip = false;
		foreach_ptr(char, name, replaced)
			if (strcmp(name, NameStr(att->attname)) == 0) skip = true;
		if (att->attgenerated && !skip)
			gen_policy_check(stringToNode(constr->defval[i].adbin), true);
	}
	table_close(rel, AccessShareLock);
}
#endif
