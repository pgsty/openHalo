/*-------------------------------------------------------------------------
 *
 * mys_rewrite.c
 *	  Raw parse tree rewriting between the MySQL grammar and the stock
 *	  PostgreSQL 18 analyzer.
 *
 * openHalo implemented MySQL semantics inside a forked copy of the PG14
 * analyzer (mys_analyze.c, mys_parse_utilcmd.c, ...).  PG18 offers no
 * analyzer hooks, so aux_mysql rewrites the raw parse tree instead, right
 * after parsing (exec_simple_query parses inside a transaction, so catalog
 * lookups are allowed here):
 *
 *	- SET ... / SELECT ... INTO @var   -> SELECT of setter functions, no result
 *	- CREATE TABLE: AUTO_INCREMENT, inline KEY, column/table COMMENT, table
 *	  options (ENGINE, CHARSET, AUTO_INCREMENT=, COMMENT=)
 *	- INSERT ... ON DUPLICATE KEY UPDATE / REPLACE: infer the arbiter index
 *	- INSERT into AUTO_INCREMENT tables: hidden RETURNING to feed
 *	  LAST_INSERT_ID() and the OK packet (same trick as openHalo)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_index.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "utils/formatting.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "halo_mysql.h"
#include "mys_prefix.h"
#include "mys_parsenodes.h"

static ResTarget *
make_target(char *name, Node *val)
{
	ResTarget  *rt = makeNode(ResTarget);

	rt->name = name;
	rt->val = val;
	rt->location = -1;
	return rt;
}

static Node *
make_colref(char *qualifier, char *colname)
{
	ColumnRef  *cr = makeNode(ColumnRef);

	cr->fields = qualifier ? list_make2(makeString(qualifier), makeString(colname))
		: list_make1(makeString(colname));
	cr->location = -1;
	return (Node *) cr;
}

static List *
relation_name_list(RangeVar *rel)
{
	List	   *names = NIL;

	if (rel->schemaname)
		names = lappend(names, makeString(pstrdup(rel->schemaname)));
	return lappend(names, makeString(pstrdup(rel->relname)));
}

static Node *
make_comment(ObjectType objtype, List *names, char *comment)
{
	CommentStmt *c = makeNode(CommentStmt);

	c->objtype = objtype;
	c->object = (Node *) names;
	c->comment = comment;
	return (Node *) c;
}

/* ----------------------------------------------------------------
 * Result column names
 *
 * MySQL names an unaliased select-list expression after its source text
 * ("COUNT(*)", "@@version", "1+1"); PostgreSQL uses the function name or
 * "?column?".  Applications read columns by name, so copy MySQL's rule.
 * ----------------------------------------------------------------
 */
static const char *const clause_keywords[] = {
	"from", "into", "where", "group", "having", "window", "order",
	"limit", "union", "intersect", "except", "for", "lock", NULL
};

/*
 * End of the select-list expression at start: the last character that is
 * neither space nor comment before the next ',' ';' ')' or clause keyword.
 * MySQL names an item after its tokens, so a comment after the expression is
 * no part of the name ("select 1+1 # sum" is named 1+1); one inside it is.
 * Comments are those of mys_scan.l: '#' to \n; "--" to \n if a space or a
 * control character follows it or the text ends there ("1--1" is 1 - -1);
 * slash-star to star-slash.  An executable comment (slash-star-bang) holds
 * SQL.  q is the whole text, so q[end] can be read.
 */
static int
expr_text_end(const char *q, int start, int end)
{
	int			depth = 0;
	int			i = start;
	int			last = start;

	while (i < end)
	{
		char		c = q[i];

		if (c == '#' ||
			(c == '-' && i + 1 < end && q[i + 1] == '-' &&
			 ((unsigned char) q[i + 2] <= ' ' || q[i + 2] == '\177')))
		{
			while (i < end && q[i] != '\n')
				i++;
			continue;
		}
		if (c == '/' && i + 1 < end && q[i + 1] == '*' &&
			!(i + 2 < end && q[i + 2] == '!'))
		{
			for (i += 2; i < end && !(q[i] == '*' && i + 1 < end && q[i + 1] == '/'); i++)
				;
			i += 2;
			continue;
		}
		if (c == '\'' || c == '"' || c == '`')
		{
			i++;
			while (i < end && q[i] != c)
			{
				if (q[i] == '\\' && c != '`')
					i++;
				i++;
			}
			i++;
			last = Min(i, end);
			continue;
		}
		if (c == '(')
			depth++;
		else if (c == ')')
		{
			if (depth == 0)
				return last;
			depth--;
		}
		else if (depth == 0 && (c == ',' || c == ';'))
			return last;
		else if (depth == 0 && isalpha((unsigned char) c) &&
				 (i == start || !(isalnum((unsigned char) q[i - 1]) || q[i - 1] == '_')))
		{
			for (int k = 0; clause_keywords[k]; k++)
			{
				int			len = strlen(clause_keywords[k]);

				if (i + len <= end && pg_strncasecmp(q + i, clause_keywords[k], len) == 0 &&
					(i + len == end || !(isalnum((unsigned char) q[i + len]) || q[i + len] == '_')))
					return last;
			}
		}
		if (!isspace((unsigned char) c))
			last = i + 1;
		i++;
	}
	return last;
}

/*
 * Offset in the prepared text (with '?') of an offset in the parsed text
 * (with the parameter values rendered in).  A position inside a rendered
 * value maps to its '?'.
 */
static int
map_to_prepared_text(int loc)
{
	const MysTextMap *m = mys_text_map;
	int			delta = 0;

	for (int i = 0; i < m->nparams; i++)
	{
		if (loc < m->new_loc[i])
			break;
		if (loc < m->new_loc[i] + m->new_len[i])
			return m->orig_loc[i];
		delta = (m->new_loc[i] + m->new_len[i]) - (m->orig_loc[i] + 1);
	}
	return loc - delta;
}

static void
fix_target_names(SelectStmt *sel, const char *query, int stmt_end)
{
	ListCell   *lc;

	if (sel == NULL || query == NULL)
		return;
	if (sel->op != SETOP_NONE)
	{
		fix_target_names(sel->larg, query, stmt_end);
		return;
	}

	foreach(lc, sel->targetList)
	{
		ResTarget  *rt = (ResTarget *) lfirst(lc);
		const char *text = query;
		int			start = rt->location;
		int			end_limit = stmt_end;
		int			end;

		if (!IsA(rt, ResTarget) || rt->name != NULL || rt->location < 0 ||
			rt->location >= stmt_end || IsA(rt->val, ColumnRef))
			continue;

		/* prepared statement: name after the text the client prepared */
		if (mys_text_map)
		{
			text = mys_text_map->orig;
			start = map_to_prepared_text(rt->location);
			end_limit = map_to_prepared_text(stmt_end);
		}

		/* 'abc' is named abc (but a bare '?' is named ?) */
		if (IsA(rt->val, A_Const) && IsA(&((A_Const *) rt->val)->val, String) &&
			text[start] != '?')
		{
			rt->name = pstrdup(strVal(&((A_Const *) rt->val)->val));
			continue;
		}

		end = expr_text_end(text, start, end_limit);
		while (end > start && isspace((unsigned char) text[end - 1]))
			end--;
		if (end > start)
			rt->name = pnstrdup(text + start, Min(end - start, NAMEDATALEN - 1));
	}
}

/* ----------------------------------------------------------------
 * SET ... and SELECT ... INTO @var
 * ----------------------------------------------------------------
 */
static bool
rewrite_select_into_vars(SelectStmt *sel)
{
	List	   *vars;
	ListCell   *lc;
	int			i = 0;

	if (sel->intoClause == NULL || sel->intoClause->rel != NULL)
		return false;

	vars = sel->intoClause->colNames;
	sel->intoClause = NULL;
	foreach(lc, sel->targetList)
	{
		ResTarget  *rt = lfirst_node(ResTarget, lc);
		Node	   *var = (i < list_length(vars)) ? list_nth(vars, i) : NULL;

		if (var && nodeTag(var) == T_UserVarRef)
		{
			UserVarRef *uv = (UserVarRef *) var;

			rt->val = (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("set_user_var")),
											list_make2(makeStringConst(uv->userVarName, -1), rt->val),
											COERCE_EXPLICIT_CALL, -1);
			rt->name = NULL;
		}
		i++;
	}
	return true;
}

/* ----------------------------------------------------------------
 * CREATE TABLE
 * ----------------------------------------------------------------
 */

/* GUC mysql.default_collation: collation of string columns without COLLATE */
char	   *halo_mysql_default_collation = NULL;

/* Scratch list for the raw statement currently being rewritten. Ownership
 * moves to its MysStmtInfo before rewriting the next statement. */
static List *rewrite_warnings = NIL;

static void
queue_rewrite_warning(List **warnings, const char *message)
{
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	foreach_ptr(char, previous, *warnings)
		if (strcmp(previous, message) == 0)
		{
			MemoryContextSwitchTo(oldcontext);
			return;
		}
	*warnings = lappend(*warnings, pstrdup(message));
	MemoryContextSwitchTo(oldcontext);
}

static void
collation_warning(const char *requested, const char *actual, bool rewriting)
{
	MysStmtInfo *info = rewriting ? NULL : mys_current_stmt_info();
	List	  **warnings;
	char	   *message;

	if (!rewriting && info == NULL)
		return;
	warnings = rewriting ? &rewrite_warnings : &info->rewrite_warnings;
	message = psprintf("Collation '%s' is approximated by '%s'; comparison, ordering and UNIQUE/ON DUPLICATE KEY behavior may differ",
					   requested, actual);
	queue_rewrite_warning(warnings, message);
	pfree(message);
}

static CollateClause *
make_collate_clause(const char *schema, const char *name)
{
	CollateClause *cc = makeNode(CollateClause);

	cc->arg = NULL;
	cc->collname = list_make2(makeString(pstrdup(schema)), makeString(pstrdup(name)));
	cc->location = -1;
	return cc;
}

/* is the column of a string type (text, varchar, char, or a domain over one)? */
static bool
is_string_column(ColumnDef *col)
{
	Type		t;
	Oid			base;

	if (col->typeName == NULL)
		return false;
	t = LookupTypeName(NULL, col->typeName, NULL, true);
	if (t == NULL)
		return false;
	base = getBaseType(typeTypeId(t));
	ReleaseSysCache(t);
	return base == TEXTOID || base == VARCHAROID || base == BPCHAROID ||
		mys_is_text_string(base);
}

/* MySQL collation names ending in _ci compare case-insensitively */
static bool
mysql_collation_is_ci(const char *name)
{
	size_t		len = strlen(name);

	return len > 3 && pg_strcasecmp(name + len - 3, "_ci") == 0;
}

/* Keep the existing, bounded comparison/index implementations. Every
 * approximation is explicit in the statement diagnostics. In particular,
 * unicode_ci/520 and language-specific rules are not general_ci aliases. */
static const char *
mapped_collation(const char *requested, bool rewriting)
{
	bool		ci;
	const char *actual;
	bool		exact;

	if (pg_strcasecmp(requested, "case_insensitive") == 0)
		return mys_prefix_collation_name(false);
	if (pg_strcasecmp(requested, "case_sensitive") == 0)
		return mys_prefix_collation_name(true);
	if (pg_strcasecmp(requested, "ignore_accents") == 0 ||
		pg_strcasecmp(requested, "default") == 0)
		return asc_tolower(requested, strlen(requested));

	ci = mysql_collation_is_ci(requested);
	actual = mys_prefix_collation_name(!ci);
	exact = ci ?
		(pg_strcasecmp(requested, "utf8_general_ci") == 0 ||
		 pg_strcasecmp(requested, "utf8mb4_general_ci") == 0) :
		(pg_strcasecmp(requested, "utf8_bin") == 0 ||
		 pg_strcasecmp(requested, "utf8mb4_bin") == 0);

	/* New binaries can precede the 1.9 SQL installation. The old ICU
	 * collations are fallbacks too, even for a normally exact MySQL name. */
	if (!exact || strcmp(actual, ci ? "utf8mb4_general_ci" : "utf8mb4_bin") != 0)
		collation_warning(requested, actual, rewriting);
	return actual;
}

/* COLLATE can occur in a SELECT/INSERT/UPDATE expression as well as in a
 * column definition. The former bypasses mys_rectify_column_collation, so
 * rewrite it before PostgreSQL tries to resolve the name. */
static bool
rewrite_expression_collation(Node *node, void *context)
{
	CollateClause *clause;
	char	   *requested;
	const char *actual;

	(void) context;

	if (node == NULL)
		return false;
	if (!IsA(node, CollateClause))
		return raw_expression_tree_walker(node, rewrite_expression_collation, context);
	clause = (CollateClause *) node;
	if (clause->collname == NIL || !IsA(llast(clause->collname), String))
		return raw_expression_tree_walker(node, rewrite_expression_collation, context);
	requested = strVal(llast(clause->collname));
	if (pg_strcasecmp(requested, "default") == 0)
	{
		clause->collname = list_make2(makeString("pg_catalog"), makeString("default"));
		return raw_expression_tree_walker(node, rewrite_expression_collation, context);
	}
	actual = mapped_collation(requested, true);
	clause->collname = list_make2(makeString("mysql"), makeString(pstrdup(actual)));
	return raw_expression_tree_walker(node, rewrite_expression_collation, context);
}

static void
rewrite_expression_collations(Node *stmt)
{
	if (stmt == NULL)
		return;
	if (IsA(stmt, CreateTableAsStmt))
		raw_expression_tree_walker(((CreateTableAsStmt *) stmt)->query,
								rewrite_expression_collation, NULL);
	else if (IsA(stmt, ViewStmt))
		raw_expression_tree_walker(((ViewStmt *) stmt)->query,
								rewrite_expression_collation, NULL);
	else if (IsA(stmt, SelectStmt) || IsA(stmt, InsertStmt) ||
				 IsA(stmt, UpdateStmt) || IsA(stmt, DeleteStmt) || IsA(stmt, MergeStmt))
		raw_expression_tree_walker(stmt, rewrite_expression_collation, NULL);
}

/*
 * New MySQL string columns use the dedicated utf8mb4_general_ci collation:
 * simple MySQL 5.7 weights (case/accent insensitive) with PAD SPACE. The
 * "case_insensitive" table-label/GUC alias remains replay-compatible. Existing
 * columns and indexes on the old ICU collation are never changed in place.
 * Other legacy aliases retain their existing meanings; non-string columns
 * have no collation. See mys_prefix.c for comparison and LIKE methods.
 */
void
mys_rectify_column_collation(ColumnDef *col, const char *table_collation,
							 bool rewriting)
{
	char	   *name;

	if (col->collClause == NULL)
	{
		if (!is_string_column(col))
			return;
		col->collClause = make_collate_clause("mysql", table_collation);

	}

	if (!is_string_column(col))
	{
		col->collClause = NULL;
		return;
	}

	name = strVal(llast(col->collClause->collname));
	if (pg_strcasecmp(name, "case_insensitive") == 0 ||
		pg_strcasecmp(name, "case_sensitive") == 0 ||
		pg_strcasecmp(name, "ignore_accents") == 0)
		col->collClause = make_collate_clause("mysql",
			pg_strcasecmp(name, "case_insensitive") == 0 ? mys_prefix_collation_name(false) :
			pg_strcasecmp(name, "case_sensitive") == 0 ? mys_prefix_collation_name(true) :
			asc_tolower(name, strlen(name)));
	else if (pg_strcasecmp(name, "default") == 0)
		col->collClause = make_collate_clause("pg_catalog", "default");
	else
		col->collClause = make_collate_clause("mysql", mapped_collation(name, rewriting));
}

/* table-level COLLATE=: case-insensitive unless a non-_ci collation is given */
static const char *
table_collation_name(List *options)
{
	const char *name = halo_mysql_default_collation && halo_mysql_default_collation[0] ?
		halo_mysql_default_collation : "default";

	foreach_node(DefElem, d, options)
	{
		if (strcmp(d->defname, "collate") == 0 && d->arg && IsA(d->arg, List))
		{
			name = strVal(llast((List *) d->arg));
			break;
		}
	}
	if (pg_strcasecmp(name, "case_insensitive") == 0 ||
		pg_strcasecmp(name, "case_sensitive") == 0 ||
		pg_strcasecmp(name, "ignore_accents") == 0 ||
		pg_strcasecmp(name, "default") == 0)
		return asc_tolower(name, strlen(name));
	(void) mapped_collation(name, true);
	return mysql_collation_is_ci(name) ? "case_insensitive" : "case_sensitive";
}

/*
 * MySQL storage engines -> PostgreSQL table access methods.  ENGINE=<name>
 * picks the table access method of the same name when one is installed
 * (ENGINE=OrioleDB -> orioledb); InnoDB, MyISAM, ... keep the default AM.
 * ENCRYPTION='Y' picks pg_tde's tde_heap, and is an error without it: a
 * table the user asked to encrypt must not silently be stored in clear.
 */
char *
mys_engine_access_method(DefElem *d)
{
	char	   *val;

	if (d->arg == NULL || !IsA(d->arg, String))
		return NULL;
	val = asc_tolower(strVal(d->arg), strlen(strVal(d->arg)));

	if (strcmp(d->defname, "engine") == 0)
		return OidIsValid(get_table_am_oid(val, true)) ? val : NULL;

	if (strcmp(d->defname, "encryption") == 0 && strcmp(val, "y") == 0)
	{
		if (!OidIsValid(get_table_am_oid("tde_heap", true)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ENCRYPTION='Y' requires the pg_tde extension (table access method \"tde_heap\")")));
		return "tde_heap";
	}
	return NULL;
}

/* "on update current_timestamp(N)" constraint produced by the grammar */
static bool
is_on_update_clause(Node *n)
{
	return n != NULL && IsA(n, String) &&
		pg_strncasecmp(strVal(n), "on update", 9) == 0;
}

/*
 * Statement that sets up the MySQL behaviors needing triggers once the
 * table exists (mys_utility.c): ON UPDATE CURRENT_TIMESTAMP columns to add
 * or remove, and the AUTO_INCREMENT counter trigger.
 */
static Node *
make_table_setup(RangeVar *rel, List *on_update_add, List *on_update_drop)
{
	DoStmt	   *d = makeNode(DoStmt);

	d->args = list_make1(makeDefElem(MYS_TABLE_SETUP_MARKER, (Node *) copyObject(rel), -1));
	foreach_ptr(char, c, on_update_add)
		d->args = lappend(d->args, makeDefElem("on_update_add", (Node *) makeString(c), -1));
	foreach_ptr(char, c, on_update_drop)
		d->args = lappend(d->args, makeDefElem("on_update_drop", (Node *) makeString(c), -1));
	return (Node *) d;
}

/*
 * col type [GENERATED ALWAYS] AS (expr) [VIRTUAL | STORED].  MySQL computes a
 * VIRTUAL column when it reads the row and a STORED one when it writes it;
 * both read the same values.  PostgreSQL's virtual generated columns can
 * neither be indexed (MySQL indexes virtual columns) nor use user-defined
 * functions and types (all of aux_mysql's), so every generated column is
 * stored.  The declared kind is kept for SHOW and information_schema: the
 * table setup (mys_utility.c) labels a VIRTUAL column and unlabels a column
 * redefined otherwise.  Returns that setup argument, NULL for a column that
 * is not generated.
 */
static DefElem *
store_generated_column(ColumnDef *col, const char *name)
{
	foreach_ptr(Node, n, col->constraints)
	{
		Constraint *c = (Constraint *) n;

		if (IsA(n, Constraint) && c->contype == CONSTR_GENERATED)
		{
			bool		is_virtual = c->generated_kind != ATTRIBUTE_GENERATED_STORED;

			c->generated_kind = ATTRIBUTE_GENERATED_STORED;
			return makeDefElem(is_virtual ? "generated_virtual" : "generated_stored",
							   (Node *) makeString(pstrdup(name)), -1);
		}
	}
	return NULL;
}

/*
 * AUTO_INCREMENT -> GENERATED BY DEFAULT AS IDENTITY.  aux_mysql maps types
 * like "int unsigned" to domains, but identity columns need a plain integer
 * type, so fall back to the domain's base type.
 */
static Constraint *
make_identity_column(ColumnDef *col, Constraint *c)
{
	Oid			typid;
	int32		typmod;

	c->contype = CONSTR_IDENTITY;
	c->generated_when = ATTRIBUTE_IDENTITY_BY_DEFAULT;
	c->options = NIL;

	typenameTypeIdAndMod(NULL, col->typeName, &typid, &typmod);
	if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
	{
		Oid			base = getBaseType(typid);

		if (base == INT2OID || base == INT4OID || base == INT8OID)
		{
			Constraint *check = makeNode(Constraint);
			NullTest *test = makeNode(NullTest);
			TypeCast *cast = makeNode(TypeCast);

			/* Identity needs a base integer, but must retain all domain checks. */
			cast->arg = make_colref(NULL, col->colname);
			cast->typeName = makeTypeNameFromOid(typid, typmod);
			cast->location = -1;
			test->arg = (Expr *) cast;
			test->nulltesttype = IS_NOT_NULL;
			test->location = -1;
			check->contype = CONSTR_CHECK;
			check->is_enforced = true;
			check->initially_valid = true;
			check->raw_expr = (Node *) test;
			check->location = -1;
			check->conname = psprintf("aux_mysql_domain_%08x",
						hash_bytes((const unsigned char *) col->colname, strlen(col->colname)));
			col->typeName = makeTypeNameFromOid(base, -1);
			return check;
		}
	}
	return NULL;
}

static Node *int_const(int32 v);

/*
 * Warnings are emitted with their statement's result, after execution. Raw
 * rewriting can visit later statements before earlier ones have even run.
 */
void
mys_flush_rewrite_warnings(List *warnings)
{
	foreach_ptr(char, message, warnings)
		mys_add_warning(1105, message);
	list_free_deep(warnings);
}

/* a statement rewritten only to describe it (COM_STMT_PREPARE) */
void
mys_discard_rewrite_warnings(void)
{
	list_free_deep(rewrite_warnings);
	rewrite_warnings = NIL;
}

/* type of a column defined in this statement, or else in the catalog */
static bool
key_column_type(const char *name, List *coldefs, Oid relid, Oid *typid, int32 *typmod)
{
	foreach_ptr(Node, n, coldefs)
	{
		ColumnDef  *col = (ColumnDef *) n;

		if (IsA(n, ColumnDef) && col->typeName && strcmp(col->colname, name) == 0)
		{
			Type		t = LookupTypeName(NULL, col->typeName, typmod, true);

			if (t == NULL)
				return false;
			*typid = typeTypeId(t);
			ReleaseSysCache(t);
			return true;
		}
	}
	if (OidIsValid(relid))
	{
		AttrNumber	attnum = get_attnum(relid, name);
		Oid			collid;

		if (attnum != InvalidAttrNumber)
		{
			get_atttypetypmodcoll(relid, attnum, typid, typmod, &collid);
			return true;
		}
	}
	return false;
}

/*
 * Legacy expression-index description, retained for reading pre-1.9 indexes.
 * With the 1.9 SQL installed, new DDL uses mysql_prefix on original columns.
 */
static Node *
prefix_expr(const char *column, bool bytes, int length, bool digest)
{
	ColumnRef  *ref = makeNode(ColumnRef);
	Node	   *prefix;

	ref->fields = list_make1(makeString(pstrdup(column)));
	ref->location = -1;
	prefix = (Node *) makeFuncCall(list_make2(makeString(bytes ? "pg_catalog" : "mysql"),
											  makeString(bytes ? "substr" : "left")),
								   bytes ? list_make3(ref, int_const(1), int_const(length)) :
								   list_make2(ref, int_const(length)),
								   COERCE_EXPLICIT_CALL, -1);
	if (!digest)
		return prefix;
	return (Node *) makeFuncCall(list_make2(makeString("pg_catalog"), makeString("md5")),
								 list_make1(prefix), COERCE_EXPLICIT_CALL, -1);
}

/* Until the SQL upgrade installs the AM and every required opclass, preserve
 * the previous release's storage strategy and warnings. This compatibility
 * path is not a repair or migration of pre-1.9 indexes. */
static char *
legacy_prefix_key_parts(List **params, bool unique, bool primary, char **access_method,
				 Oid relid, List *coldefs)
{
	int			nparts = list_length(*params);
	int		   *length = palloc(sizeof(int) * Max(nparts, 1));
	Oid		   *base = palloc0(sizeof(Oid) * Max(nparts, 1));
	bool	   *is_long = palloc0(sizeof(bool) * Max(nparts, 1));
	int			ncut = 0;
	int			unique_bytes = 0;
	int			i = 0;
	char	   *lookup = NULL;

	foreach_node(IndexElem, ie, *params)
	{
		DefElem    *prefix = NULL;
		Oid			typid;
		int32		typmod = -1;

		length[i] = -1;
		foreach_node(DefElem, opt, ie->opclassopts)
			if (strcmp(opt->defname, "mysql_prefix") == 0)
				prefix = opt;
		if (prefix)
		{
			ie->opclassopts = list_delete_ptr(ie->opclassopts, prefix);
			length[i] = intVal(prefix->arg);
		}
		if (ie->name && key_column_type(ie->name, coldefs, relid, &typid, &typmod))
		{
			base[i] = getBaseTypeAndTypmod(typid, &typmod);
			if (mys_is_text_string(base[i]))
				base[i] = TEXTOID;
			else
				base[i] = mys_binary_base_type(base[i]);
			is_long[i] = base[i] == TEXTOID || base[i] == BYTEAOID ||
				((base[i] == VARCHAROID || base[i] == BPCHAROID) &&
				 (typmod < (int32) VARHDRSZ || typmod - VARHDRSZ > 600));
			/* a prefix as long as the column is the whole column */
			if ((base[i] == VARCHAROID || base[i] == BPCHAROID) &&
				typmod >= (int32) VARHDRSZ && length[i] >= typmod - VARHDRSZ)
				length[i] = -1;
		}
		if (is_long[i] && (length[i] > 0 ||
						   (!unique && (base[i] == TEXTOID || base[i] == BYTEAOID))))
			ncut++;
		/* the most bytes this key's prefixes take (UTF-8: 4 per character) */
		if (unique && is_long[i] && length[i] > 0)
			unique_bytes += base[i] == BYTEAOID ? length[i] : length[i] * 4;
		i++;
	}
	if (access_method && *access_method && strcmp(*access_method, DEFAULT_INDEX_TYPE) != 0)
		return NULL;

	i = 0;
	foreach_node(IndexElem, ie, *params)
	{
		bool		cut = is_long[i] && (length[i] > 0 ||
										 (!unique && (base[i] == TEXTOID || base[i] == BYTEAOID)));
		int			n = length[i];

		i++;
		if (!cut || (unique && primary))
		{
			if (unique && n > 0)
				{
					char *message = psprintf("prefix of unique key part \"%s\" is enforced on the whole column",
													 ie->name);

					queue_rewrite_warning(&rewrite_warnings, message);
					pfree(message);
				}
			continue;
		}
		if (!unique && i == 1 && access_method)
		{
			/* the key's first part: a hash index on that column alone (no ASC / DESC) */
			ie->ordering = SORTBY_DEFAULT;
			ie->nulls_ordering = SORTBY_NULLS_DEFAULT;
			ie->opclass = NIL;
			*params = list_make1(ie);
			*access_method = "hash";
			return NULL;
		}
		if (!unique)
			n = Min(n > 0 ? n : 600, Max(600 / ncut, 1));
		else if (i == 1)
			lookup = pstrdup(ie->name);
		ie->expr = prefix_expr(ie->name, base[i - 1] == BYTEAOID, n,
							   unique && base[i - 1] == BYTEAOID && unique_bytes > 2000);
		ie->indexcolname = ie->name;
		ie->name = NULL;
	}
	return lookup;
}

/* Upper bound of the stored datum, including its varlena header. Domains
 * inherit their base typmod. MySQL binary_n/varbinary_n/text_string typmods
 * are byte counts; PostgreSQL varchar/bpchar typmods are characters + 4. */
static int64
key_column_max_bytes(Oid typid, int32 typmod)
{
	Oid base = getBaseTypeAndTypmod(typid, &typmod);
	int16 len = get_typlen(base);

	if (len > 0)
		return len;
	if (base == VARCHAROID || base == BPCHAROID)
		return typmod >= VARHDRSZ ? (int64) (typmod - VARHDRSZ) * 4 + VARHDRSZ : -1;
	if (base != BYTEAOID && mys_binary_base_type(base) == BYTEAOID)
		return typmod >= 0 ? (int64) typmod + VARHDRSZ : -1;
	return type_maximum_size(base, typmod);
}

/*
 * Keep every key part and its declared length. mysql_prefix stores ordered
 * candidate keys and compares the complete original prefixes on the heap.
 * Lengths are index options so PostgreSQL can retain real PRIMARY/UNIQUE
 * constraints (including NOT NULL) while transforming IndexElems to names.
 * Zero denotes an unprefixed column. Short full-column keys stay native
 * btree so ORDER BY/LIMIT and index-only scans retain their normal plans.
 * A quarter-page budget, including per-datum alignment and tuple overhead,
 * leaves headroom below btree's one-third-page limit for all compound parts.
 */
static char *
prefix_key_parts(List **params, bool unique, bool primary, char **access_method,
				 List **options, Oid relid, List *coldefs)
{
	bool needed = false;
	int64 key_bytes = 32;        /* index tuple header and maximum null bitmap */
	StringInfoData lengths;

	if (access_method && *access_method &&
		strcmp(*access_method, DEFAULT_INDEX_TYPE) != 0 &&
		strcmp(*access_method, MYS_PREFIX_AM) != 0)
		return NULL;
	/* Check before consuming prefix markers: a mixed-version server must
	 * still run the old rewrite if SQL objects/types are not installed yet. */
	if (!mys_prefix_available())
		return legacy_prefix_key_parts(params, unique, primary, access_method, relid, coldefs);
	foreach_node(IndexElem, ie, *params)
	{
		Oid typid;
		int32 typmod = -1;
		if (ie->name && key_column_type(ie->name, coldefs, relid, &typid, &typmod) &&
			!mys_prefix_type_available(typid))
			return legacy_prefix_key_parts(params, unique, primary, access_method, relid, coldefs);
	}
	initStringInfo(&lengths);
	foreach_node(IndexElem, ie, *params)
	{
		DefElem *prefix = NULL;
		Oid typid;
		int32 typmod = -1;
		int n = 0;

		foreach_node(DefElem, opt, ie->opclassopts)
			if (strcmp(opt->defname, "mysql_prefix") == 0)
				prefix = opt;
		if (prefix)
		{
			n = intVal(prefix->arg);
			ie->opclassopts = list_delete_ptr(ie->opclassopts, prefix);
			needed = true;
		}
		if (ie->name && key_column_type(ie->name, coldefs, relid, &typid, &typmod))
		{
			int64 width = key_column_max_bytes(typid, typmod);
			if (width < 0)
				needed = true;
			else
				key_bytes += MAXALIGN(width);
		}
		else
			needed = true;
		if (lengths.len) appendStringInfoChar(&lengths, ',');
		appendStringInfo(&lengths, "%d", n);
		/* MySQL 5.7 accepts ASC/DESC syntax but stores ascending keys. */
		ie->ordering = SORTBY_DEFAULT;
		ie->nulls_ordering = SORTBY_NULLS_DEFAULT;
	}
	if (needed || key_bytes > BLCKSZ / 4)
	{
		if (access_method) *access_method = MYS_PREFIX_AM;
		*options = lappend(*options, makeDefElem("prefix_lengths",
							(Node *) makeString(lengths.data), -1));
	}
	return NULL;
}

/* Column-level UNIQUE/PRIMARY must use the same total-width decision as
 * table-level constraints, including when ALTER TABLE adds the column. */
static void
prefix_column_constraint(ColumnDef *col, Constraint *constraint)
{
	IndexElem *key;
	List *keys;

	if (constraint->contype != CONSTR_PRIMARY && constraint->contype != CONSTR_UNIQUE)
		return;
	key = makeNode(IndexElem);
	key->name = col->colname;
	keys = list_make1(key);
	(void) prefix_key_parts(&keys, true, constraint->contype == CONSTR_PRIMARY,
						   &constraint->access_method, &constraint->options,
						   InvalidOid, list_make1(col));
}

/*
 * A legacy key part (before 1.9): pg_catalog.left(col, n) or
 * pg_catalog.substr(col, 1, n) of an index expression, or its md5()
 * (*digest).
 */
bool
mys_index_prefix(Node *expr, AttrNumber *attnum, int *length, bool *bytes, bool *digest)
{
	FuncExpr   *f = (FuncExpr *) expr;
	Node	   *arg;
	Const	   *n;

	*digest = false;
	if (expr != NULL && IsA(expr, FuncExpr) &&
		(f->funcid == F_MD5_TEXT || f->funcid == F_MD5_BYTEA) && list_length(f->args) == 1)
	{
		*digest = true;
		f = (FuncExpr *) linitial(f->args);
		expr = (Node *) f;
	}
	if (expr == NULL || !IsA(expr, FuncExpr) ||
		!((f->funcid == F_LEFT && list_length(f->args) == 2) ||
		  (f->funcid == F_SUBSTR_BYTEA_INT4_INT4 && list_length(f->args) == 3)))
		return false;
	arg = linitial(f->args);
	while (IsA(arg, RelabelType))
		arg = (Node *) ((RelabelType *) arg)->arg;
	n = (Const *) llast(f->args);
	if (!IsA(arg, Var) || !IsA(n, Const) || n->consttype != INT4OID || n->constisnull)
		return false;
	if (f->funcid == F_SUBSTR_BYTEA_INT4_INT4)
	{
		Const	   *from = (Const *) lsecond(f->args);

		if (!IsA(from, Const) || from->consttype != INT4OID || from->constisnull ||
			DatumGetInt32(from->constvalue) != 1)
			return false;
	}
	*attnum = ((Var *) arg)->varattno;
	*length = DatumGetInt32(n->constvalue);
	*bytes = f->funcid == F_SUBSTR_BYTEA_INT4_INT4;
	return true;
}

/* ----------------------------------------------------------------
 * MySQL key names
 *
 * MySQL names a key per table; PostgreSQL names an index per schema, in the
 * namespace of tables, too.  So with the MySQL name as the index name, two
 * tables could not both have a KEY idx_created, nor could a key be named
 * like another table.  A key's index is named "<key>$<8 hex digits>"
 * instead: the digits, a hash of the table and key names (hashed again while
 * the name is taken), make it unique in the schema, and mys_key_name()
 * strips them.  The name alone tells the key's MySQL name, so it survives
 * pg_dump, pg_upgrade and RENAME TABLE without metadata.  An index without
 * the suffix (created in PostgreSQL, or by aux_mysql before 1.8) is known to
 * MySQL by its PostgreSQL name, a primary key as PRIMARY.  Key names compare
 * case-insensitively, as in MySQL; a key longer than 54 bytes is shortened.
 * ----------------------------------------------------------------
 */
#define KEY_SUFFIX_LEN	9		/* "$" and 8 lowercase hex digits */

static bool name_in_list(List *names, const char *name);

char *
mys_key_name(const char *relname, bool primary)
{
	size_t		len = strlen(relname);

	if (primary)
		return pstrdup("PRIMARY");
	if (len > KEY_SUFFIX_LEN && relname[len - KEY_SUFFIX_LEN] == '$' &&
		strspn(relname + len - KEY_SUFFIX_LEN + 1, "0123456789abcdef") == KEY_SUFFIX_LEN - 1)
		return pnstrdup(relname, len - KEY_SUFFIX_LEN);
	return pstrdup(relname);
}

/* the MySQL key name of an index, NULL if it does not exist */
char *
mys_index_key_name(Oid indexoid)
{
	HeapTuple	tup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexoid));
	char	   *relname = get_rel_name(indexoid);
	bool		primary;

	if (!HeapTupleIsValid(tup))
		return NULL;
	primary = ((Form_pg_index) GETSTRUCT(tup))->indisprimary;
	ReleaseSysCache(tup);
	return relname ? mys_key_name(relname, primary) : NULL;
}

/*
 * Whether key name "key" (from an index name) is "name": case-insensitively,
 * and as far as an index name holds it.
 */
static bool
key_names_match(const char *key, const char *name)
{
	int			klen = pg_mbcliplen(key, strlen(key), NAMEDATALEN - 1 - KEY_SUFFIX_LEN);
	int			len = pg_mbcliplen(name, strlen(name), NAMEDATALEN - 1 - KEY_SUFFIX_LEN);

	return klen == len && pg_strncasecmp(key, name, len) == 0;
}

/*
 * MySQL key names of a table's indexes, in OID order (*indexoids).  From
 * pg_index, not the relcache: the table need not be locked (the rewriter does
 * not lock tables that the statement locks later more strongly).
 */
static List *
table_key_names(Oid relid, List **indexoids)
{
	Relation	pg_index = table_open(IndexRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *indexes = NIL;
	List	   *names = NIL;

	ScanKeyInit(&key, Anum_pg_index_indrelid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(pg_index, IndexIndrelidIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		indexes = lappend_oid(indexes, ((Form_pg_index) GETSTRUCT(tup))->indexrelid);
	systable_endscan(scan);
	table_close(pg_index, AccessShareLock);

	list_sort(indexes, list_oid_cmp);
	foreach_oid(indexoid, indexes)
		names = lappend(names, mys_index_key_name(indexoid));
	if (indexoids)
		*indexoids = indexes;
	return names;
}

/* the index of a table that MySQL knows as key "name", or InvalidOid */
Oid
mys_find_key(Oid relid, const char *name)
{
	List	   *indexes;
	List	   *names = table_key_names(relid, &indexes);

	for (int i = 0; i < list_length(names); i++)
	{
		char	   *key = list_nth(names, i);

		if (key && key_names_match(key, name))
			return list_nth_oid(indexes, i);
	}
	return InvalidOid;
}

/*
 * The PostgreSQL name for key "key" of table "relname": free in schema nspid
 * (InvalidOid: on the search path) and not in reserved.
 */
char *
mys_key_pg_name(const char *relname, const char *key, Oid nspid, List *reserved)
{
	int			len = pg_mbcliplen(key, strlen(key), NAMEDATALEN - 1 - KEY_SUFFIX_LEN);
	StringInfoData seed;

	initStringInfo(&seed);
	appendBinaryStringInfo(&seed, relname, strlen(relname) + 1);
	appendStringInfoString(&seed, key);
	for (uint64 n = 0;; n++)
	{
		char	   *name = psprintf("%.*s$%08x", len, key,
									(uint32) hash_bytes_extended((const unsigned char *) seed.data,
																 seed.len, n));

		if (!name_in_list(reserved, name) &&
			!OidIsValid(OidIsValid(nspid) ? get_relname_relid(name, nspid) : RelnameGetRelid(name)))
			return name;
	}
}

/*
 * The keys of one table as one statement defines them.  CREATE TABLE names
 * the keys of its new table while it is rewritten.  The keys ALTER TABLE and
 * CREATE INDEX add are named when the statement runs (deferred, a marker in
 * the index options, mys_name_keys()): an earlier statement of the same packet
 * may drop or add keys of the table.  Until then, only the keys of the
 * statement itself are checked against each other.
 */
typedef struct KeyNamer
{
	const char *relname;
	Oid			nspid;			/* InvalidOid: not known yet */
	List	   *keys;			/* MySQL names of its keys */
	List	   *pgnames;		/* PostgreSQL names given in this statement */
	bool		deferred;
} KeyNamer;

#define KEY_MARKER	"mysql_key"

/* table_keys: the table's keys are in the catalog (not CREATE TABLE, not deferred) */
static void
init_key_namer(KeyNamer *kn, RangeVar *rel, bool table_keys, bool deferred)
{
	Oid			relid = table_keys ? RangeVarGetRelid(rel, NoLock, true) : InvalidOid;

	kn->relname = rel->relname;
	kn->nspid = InvalidOid;
	kn->keys = NIL;
	kn->pgnames = NIL;
	kn->deferred = deferred;
	if (OidIsValid(relid))
	{
		kn->nspid = get_rel_namespace(relid);
		kn->keys = table_key_names(relid, NULL);
	}
	else if (rel->schemaname)
		kn->nspid = get_namespace_oid(rel->schemaname, true);
}

static bool
key_name_taken(KeyNamer *kn, const char *name)
{
	foreach_ptr(char, key, kn->keys)
		if (key && key_names_match(key, name))
			return true;
	return false;
}

/* a key the statement drops (ALTER TABLE ... DROP INDEX): its name is free */
static void
release_key_name(KeyNamer *kn, const char *name)
{
	foreach_ptr(char, key, kn->keys)
		if (key && key_names_match(key, name))
		{
			kn->keys = list_delete_ptr(kn->keys, key);
			return;
		}
}

/* the first column of a key, which names a key without a name */
static const char *
first_key_column(List *keys)
{
	if (keys == NIL)
		return NULL;
	if (IsA(linitial(keys), IndexElem))
		return linitial_node(IndexElem, keys)->name;
	return IsA(linitial(keys), String) ? strVal(linitial(keys)) : NULL;
}

/*
 * The PostgreSQL name of a new key of the table named "name", NULL for one
 * without a name, which is named, as in MySQL, after its first column
 * (column, column_2, ...); NULL when that is an expression: PostgreSQL picks
 * a name.  MySQL's errors for a key named PRIMARY or like another key.
 */
static char *
key_pg_name(KeyNamer *kn, const char *name, const char *column)
{
	char	   *key;
	char	   *pgname;

	if (name == NULL && column == NULL)
		return NULL;
	if (name == NULL)
	{
		/* column_2, ...: of a long column, its first 30 bytes, as in MySQL 5.7 */
		int			base = pg_mbcliplen(column, strlen(column), 30);

		key = pstrdup(column);
		for (int i = 2; key_name_taken(kn, key) || pg_strcasecmp(key, "PRIMARY") == 0; i++)
		{
			/* MySQL allows 64 keys in a table */
			if (i == 100)
				ereport(ERROR,
						(errcode(MYS_ERRCODE_DUP_KEYNAME),
						 errmsg("Duplicate key name '%s'", key)));
			key = psprintf("%.*s_%d", base, column, i);
		}
	}
	else if (pg_strcasecmp(name, "PRIMARY") == 0)
		ereport(ERROR,
				(errcode(MYS_ERRCODE_WRONG_NAME_FOR_INDEX),
				 errmsg("Incorrect index name '%s'", name)));
	else if (key_name_taken(kn, name))
		ereport(ERROR,
				(errcode(MYS_ERRCODE_DUP_KEYNAME),
				 errmsg("Duplicate key name '%s'", name)));
	else
		key = pstrdup(name);
	kn->keys = lappend(kn->keys, key);
	pgname = mys_key_pg_name(kn->relname, key, kn->nspid, kn->pgnames);
	kn->pgnames = lappend(kn->pgnames, pgname);
	return pgname;
}

/*
 * A new key "name" (NULL: none) starting with column: its PostgreSQL name in
 * *pgname, or for a deferred namer the marker in *options
 */
static void
name_key(KeyNamer *kn, const char *name, const char *column, char **pgname, List **options)
{
	if (!kn->deferred)
	{
		*pgname = key_pg_name(kn, name, column);
		return;
	}
	if (name == NULL && column == NULL)
		return;
	if (name && pg_strcasecmp(name, "PRIMARY") == 0)
		ereport(ERROR,
				(errcode(MYS_ERRCODE_WRONG_NAME_FOR_INDEX),
				 errmsg("Incorrect index name '%s'", name)));
	if (name && key_name_taken(kn, name))
		ereport(ERROR,
				(errcode(MYS_ERRCODE_DUP_KEYNAME),
				 errmsg("Duplicate key name '%s'", name)));
	if (name)
		kn->keys = lappend(kn->keys, pstrdup(name));
	*pgname = NULL;
	*options = lappend(*options,
					   makeDefElem(KEY_MARKER,
								   (Node *) list_make2(makeString(pstrdup(name ? name : "")),
													   makeString(pstrdup(column ? column : ""))),
								   -1));
}

/* a key constraint of the statement */
static void
name_key_constraint(KeyNamer *kn, Constraint *c, const char *column)
{
	if (c->contype == CONSTR_PRIMARY)
		c->conname = NULL;		/* always PRIMARY in MySQL; PostgreSQL's <table>_pkey */
	else if (c->contype == CONSTR_UNIQUE || c->contype == CONSTR_KEY)
		name_key(kn, c->conname, column ? column : first_key_column(c->keys),
				 &c->conname, &c->options);
}

/* the marker name_key() left in the options of an index or constraint, taken out */
static bool
take_key_marker(List **options, char **name, char **column)
{
	foreach_node(DefElem, opt, *options)
	{
		if (strcmp(opt->defname, KEY_MARKER) == 0)
		{
			List	   *arg = (List *) opt->arg;

			*name = strVal(linitial(arg));
			*column = strVal(lsecond(arg));
			*name = (*name)[0] ? *name : NULL;
			*column = (*column)[0] ? *column : NULL;
			*options = list_delete_ptr(*options, opt);
			return true;
		}
	}
	return false;
}

static bool
has_key_marker(List *options)
{
	foreach_node(DefElem, opt, options)
		if (strcmp(opt->defname, KEY_MARKER) == 0)
			return true;
	return false;
}

/* the key constraints an ALTER TABLE command adds */
static List *
alter_cmd_constraints(AlterTableCmd *cmd)
{
	if (cmd->subtype == AT_AddConstraint && cmd->def && IsA(cmd->def, Constraint))
		return list_make1(cmd->def);
	if (cmd->subtype == AT_AddColumn && cmd->def && IsA(cmd->def, ColumnDef))
		return ((ColumnDef *) cmd->def)->constraints;
	return NIL;
}

/*
 * ALTER TABLE / CREATE INDEX about to run: a copy with the keys named (as
 * name_key() deferred them), checked against the table's keys at this time;
 * NULL if the statement adds none.  As in MySQL, the keys an ALTER TABLE
 * drops are dropped before those it adds.
 */
Node *
mys_name_keys(Node *stmt)
{
	bool		marked = false;
	KeyNamer	kn;
	char	   *name;
	char	   *column;

	if (IsA(stmt, IndexStmt))
		marked = has_key_marker(((IndexStmt *) stmt)->options);
	else if (IsA(stmt, AlterTableStmt))
		foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) stmt)->cmds)
			foreach_ptr(Node, n, alter_cmd_constraints(cmd))
				marked |= IsA(n, Constraint) && has_key_marker(((Constraint *) n)->options);
	if (!marked)
		return NULL;

	stmt = copyObject(stmt);
	if (IsA(stmt, IndexStmt))
	{
		IndexStmt  *idx = (IndexStmt *) stmt;
		Oid			relid = RangeVarGetRelid(idx->relation, NoLock, true);
		Oid			existing = InvalidOid;

		take_key_marker(&idx->options, &name, &column);
		/* IF NOT EXISTS (not MySQL): PostgreSQL skips the existing key's index */
		if (idx->if_not_exists && name && OidIsValid(relid))
			existing = mys_find_key(relid, name);
		init_key_namer(&kn, idx->relation, true, false);
		idx->idxname = OidIsValid(existing) ? get_rel_name(existing) :
			key_pg_name(&kn, name, column);
		return stmt;
	}

	init_key_namer(&kn, ((AlterTableStmt *) stmt)->relation, true, false);
	foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) stmt)->cmds)
	{
		if ((int) cmd->subtype == AT_DropIndex && cmd->name)
			release_key_name(&kn, cmd->name);
	}
	foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) stmt)->cmds)
		foreach_ptr(Node, n, alter_cmd_constraints(cmd))
		{
			Constraint *c = (Constraint *) n;

			if (IsA(n, Constraint) && take_key_marker(&c->options, &name, &column))
				c->conname = key_pg_name(&kn, name, column);
		}
	return stmt;
}

/*
 * Lookup indexes go after the statement's other keys, named as a key without
 * a name, and are left out when a non-unique key of the statement starts
 * with the same column.
 */
static List *
add_lookup_indexes(List *indexes, List *lookups, KeyNamer *kn)
{
	foreach_node(IndexStmt, lookup, lookups)
	{
		char	   *column = linitial_node(IndexElem, lookup->indexParams)->name;
		bool		served = false;

		foreach_ptr(Node, n, indexes)
		{
			IndexStmt  *idx = (IndexStmt *) n;

			if (IsA(n, IndexStmt) && !idx->unique && idx->indexParams != NIL &&
				linitial_node(IndexElem, idx->indexParams)->name &&
				strcmp(linitial_node(IndexElem, idx->indexParams)->name, column) == 0)
				served = true;
		}
		if (!served)
		{
			name_key(kn, NULL, column, &lookup->idxname, &lookup->options);
			indexes = lappend(indexes, lookup);
		}
	}
	return indexes;
}

/* a hash index on a column, for = lookups that a prefix key cannot serve */
static Node *
make_lookup_index(RangeVar *rel, char *column)
{
	IndexStmt  *idx = makeNode(IndexStmt);
	IndexElem  *ie = makeNode(IndexElem);

	ie->name = column;
	ie->ordering = SORTBY_DEFAULT;
	ie->nulls_ordering = SORTBY_NULLS_DEFAULT;
	idx->relation = copyObject(rel);
	idx->accessMethod = "hash";
	idx->indexParams = list_make1(ie);
	return (Node *) idx;
}

static Node *
make_index_stmt(RangeVar *rel, Constraint *c)
{
	IndexStmt  *idx = makeNode(IndexStmt);

	idx->idxname = c->conname;
	idx->options = c->options;	/* a deferred name (name_key()) */
	idx->relation = copyObject(rel);
	idx->accessMethod = c->access_method ? c->access_method : DEFAULT_INDEX_TYPE;
	idx->indexParams = c->keys;
	idx->unique = false;
	idx->if_not_exists = false;
	return (Node *) idx;
}

/*
 * The MySQL grammar stores PRIMARY KEY / UNIQUE columns as IndexElem nodes
 * (openHalo taught transformIndexConstraint about that); stock PG wants a
 * list of column names.  Returns false if the constraint was turned into a
 * separate unique index (expression keys), which is appended to *indexes.
 */
static bool
normalize_index_constraint(Constraint *c, RangeVar *rel, List **indexes)
{
	List	   *names = NIL;
	ListCell   *lc;

	if (c->contype != CONSTR_PRIMARY && c->contype != CONSTR_UNIQUE)
		return true;
	if (c->keys == NIL || !IsA(linitial(c->keys), IndexElem))
		return true;

	foreach(lc, c->keys)
	{
		IndexElem  *ie = lfirst_node(IndexElem, lc);

		if (ie->name == NULL)
		{
			/* expression key: only possible as a unique index */
			IndexStmt  *idx = (IndexStmt *) make_index_stmt(rel, c);

			idx->unique = true;
			idx->primary = false;
			*indexes = lappend(*indexes, idx);
			return false;
		}
		names = lappend(names, makeString(ie->name));
	}
	c->keys = names;
	if (c->including != NIL && IsA(linitial(c->including), IndexElem))
	{
		List	   *inc = NIL;

		foreach(lc, c->including)
			inc = lappend(inc, makeString(lfirst_node(IndexElem, lc)->name));
		c->including = inc;
	}
	return true;
}

/* columns an ALTER TABLE adds or redefines, for keys defined in the same statement */
static List *
alter_added_columns(AlterTableStmt *stmt)
{
	List	   *cols = NIL;

	foreach_node(AlterTableCmd, cmd, stmt->cmds)
		if ((cmd->subtype == AT_AddColumn || (int) cmd->subtype == AT_ModifyColumn ||
			 (int) cmd->subtype == AT_ChangeColumn) &&
			cmd->def && IsA(cmd->def, ColumnDef) && ((ColumnDef *) cmd->def)->colname)
			cols = lappend(cols, cmd->def);
	return cols;
}

/* DEFAULT 0x..: the literal of its value for the column's type (mys_binary.c) */
static void
fold_column_default(ColumnDef *col)
{
	foreach_ptr(Node, n, col->constraints)
	{
		Constraint *c = (Constraint *) n;
		Type		t;

		if (!IsA(n, Constraint) || c->contype != CONSTR_DEFAULT || c->raw_expr == NULL ||
			col->typeName == NULL)
			continue;
		t = LookupTypeName(NULL, col->typeName, NULL, true);
		if (t == NULL)
			continue;
		c->raw_expr = mys_fold_hexlit_default(c->raw_expr, typeTypeId(t));
		ReleaseSysCache(t);
	}
}

static List *
rewrite_create_stmt(CreateStmt *stmt)
{
	List	   *elts = NIL;
	List	   *indexes = NIL;
	List	   *lookups = NIL;
	List	   *comments = NIL;
	List	   *options = NIL;
	List	   *on_update = NIL;
	List	   *generated = NIL;
	Constraint *identity = NULL;
	ListCell   *lc;
	const char *table_collation = NULL;
	KeyNamer	kn;
	RangeVar   *like = NULL;

	if (stmt->if_not_exists && OidIsValid(RangeVarGetRelid(stmt->relation, NoLock, true)))
		return NIL;
	init_key_namer(&kn, stmt->relation, false, false);	/* a new table */
	/* LIKE copies the saved table default, independently of column overrides. */
	foreach_ptr(Node, elt, stmt->tableElts)
		if (IsA(elt, TableLikeClause))
		{
			Oid source = RangeVarGetRelid(((TableLikeClause *) elt)->relation,
										 AccessShareLock, false);
			table_collation = mys_table_default_collation(source);
			like = ((TableLikeClause *) elt)->relation;
			/* and the declared kind of the generated columns it copies */
			generated = lappend(generated,
								makeDefElem("generated_like",
											(Node *) copyObject(((TableLikeClause *) elt)->relation), -1));
		}
	if (table_collation == NULL)
		table_collation = table_collation_name(stmt->options);

	foreach(lc, stmt->tableElts)
	{
		Node	   *elt = lfirst(lc);

		if (IsA(elt, ColumnDef))
		{
			ColumnDef  *col = (ColumnDef *) elt;
			List	   *cons = NIL;
			ListCell   *lc2;
			DefElem    *kind = store_generated_column(col, col->colname);

			if (kind)
				generated = lappend(generated, kind);
			mys_rectify_column_collation(col, table_collation, true);
			fold_column_default(col);
			foreach(lc2, col->constraints)
			{
				Node	   *n = lfirst(lc2);

				if (IsA(n, CommentStmt))
					comments = lappend(comments,
									   make_comment(OBJECT_COLUMN,
													lappend(relation_name_list(stmt->relation),
															makeString(col->colname)),
													((CommentStmt *) n)->comment));
				else if (is_on_update_clause(n))
					on_update = lappend(on_update, col->colname);
				else if (IsA(n, String))
					continue;
				else if (IsA(n, Constraint) &&
						 ((Constraint *) n)->contype == CONSTR_AUTOINC)
				{
					Constraint *c = (Constraint *) n;

					Constraint *check = make_identity_column(col, c);

					if (check)
						cons = lappend(cons, check);
					identity = c;
					cons = lappend(cons, c);
				}
				else if (IsA(n, Constraint) &&
						 ((Constraint *) n)->contype == CONSTR_KEY)
				{
					name_key_constraint(&kn, (Constraint *) n, col->colname);
					indexes = lappend(indexes, make_index_stmt(stmt->relation, (Constraint *) n));
				}
				else
				{
					/* UNIQUE / PRIMARY KEY of the column */
					if (IsA(n, Constraint))
					{
						Constraint *c = (Constraint *) n;

						name_key_constraint(&kn, c, col->colname);
						prefix_column_constraint(col, c);
					}
					cons = lappend(cons, n);
				}
			}
			col->constraints = cons;
			elts = lappend(elts, col);
		}
		else if (IsA(elt, Constraint) && ((Constraint *) elt)->contype == CONSTR_KEY)
		{
			name_key_constraint(&kn, (Constraint *) elt, NULL);
			indexes = lappend(indexes, make_index_stmt(stmt->relation, (Constraint *) elt));
		}
		else if (IsA(elt, Constraint))
		{
			Constraint *c = (Constraint *) elt;
			char	   *lookup = NULL;

			name_key_constraint(&kn, c, NULL);
			if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
				lookup = prefix_key_parts(&c->keys, true, c->contype == CONSTR_PRIMARY, &c->access_method, &c->options,
										  InvalidOid, stmt->tableElts);
			if (normalize_index_constraint(c, stmt->relation, &indexes))
				elts = lappend(elts, elt);
			if (lookup)
				lookups = lappend(lookups, make_lookup_index(stmt->relation, lookup));
		}
		else
			elts = lappend(elts, elt);
	}
	foreach_ptr(Node, n, indexes)
		if (IsA(n, IndexStmt) && !((IndexStmt *) n)->unique)
			prefix_key_parts(&((IndexStmt *) n)->indexParams, false, false,
							 &((IndexStmt *) n)->accessMethod, &((IndexStmt *) n)->options, InvalidOid, stmt->tableElts);
	indexes = add_lookup_indexes(indexes, lookups, &kn);
	stmt->tableElts = elts;

	/* MySQL table options */
	foreach(lc, stmt->options)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "comment") == 0)
			comments = lappend(comments,
							   make_comment(OBJECT_TABLE, relation_name_list(stmt->relation),
											strVal(d->arg)));
		else if (strcmp(d->defname, "auto_increment") == 0)
		{
			if (identity)
				identity->options = lappend(identity->options,
											makeDefElem("start", d->arg, -1));
		}
		else if (strcmp(d->defname, "collate") == 0)
			continue;			/* applied to the columns above */
		else if (strcmp(d->defname, "engine") == 0 || strcmp(d->defname, "encryption") == 0)
		{
			char	   *am = mys_engine_access_method(d);

			if (am && stmt->accessMethod && strcmp(am, stmt->accessMethod) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("table options select two storage engines: %s and %s",
								stmt->accessMethod, am)));
			if (am)
				stmt->accessMethod = am;
		}
		else
			options = lappend(options, d);
	}
	stmt->options = options;

	indexes = list_concat(indexes, comments);
	/* LIKE can acquire an identity only during PostgreSQL's transformation. */
	{
		DoStmt *setup = (DoStmt *) make_table_setup(stmt->relation, on_update, NIL);

		setup->args = lappend(setup->args, makeDefElem("default_collation",
									(Node *) makeString(pstrdup(table_collation)), -1));
		if (like)
			setup->args = lappend(setup->args, makeDefElem("like_key_names",
														   (Node *) copyObject(like), -1));
		setup->args = list_concat(setup->args, generated);
		indexes = lappend(indexes, setup);
	}
	return indexes;
}

/* ALTER TABLE ... ADD {KEY|INDEX|UNIQUE|PRIMARY KEY} (...) */
static List *
rewrite_alter_table(AlterTableStmt *stmt)
{
	List	   *cmds = NIL;
	List	   *indexes = NIL;
	List	   *lookups = NIL;
	List	   *on_update_add = NIL;
	List	   *on_update_drop = NIL;
	List	   *generated = NIL;
	bool		setup = false;
	ListCell   *lc;
	KeyNamer	kn;

	init_key_namer(&kn, stmt->relation, false, true);

	foreach(lc, stmt->cmds)
	{
		AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

		/* MODIFY / CHANGE replace the whole definition, ON UPDATE included */
		if (((int) cmd->subtype == AT_ModifyColumn || (int) cmd->subtype == AT_ChangeColumn) &&
			cmd->def && IsA(cmd->def, ColumnDef))
		{
			ColumnDef  *col = (ColumnDef *) cmd->def;
			bool		found = false;
			Constraint *check = NULL;

			fold_column_default(col);
			foreach_ptr(Node, n, col->constraints)
			{
				if (is_on_update_clause(n))
					found = true;
				/* AUTO_INCREMENT: identity, see mys_utility.c modify_column() */
				else if (IsA(n, Constraint) && ((Constraint *) n)->contype == CONSTR_AUTOINC)
					check = make_identity_column(col, (Constraint *) n);
			}
			if (check)
				col->constraints = lappend(col->constraints, check);
			if (found)
				on_update_add = lappend(on_update_add,
										(int) cmd->subtype == AT_ChangeColumn ? col->colname : cmd->name);
			else
				on_update_drop = lappend(on_update_drop,
										 (int) cmd->subtype == AT_ChangeColumn ? col->colname : cmd->name);
			{
				char	   *name = (int) cmd->subtype == AT_ChangeColumn ? col->colname : cmd->name;
				DefElem    *kind = store_generated_column(col, name);

				generated = lappend(generated, kind ? kind :
									makeDefElem("generated_stored", (Node *) makeString(pstrdup(name)), -1));
			}
			setup = true;
		}

		if (cmd->subtype == AT_ColumnDefault && cmd->def)
		{
			Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);
			AttrNumber	attnum = OidIsValid(relid) ? get_attnum(relid, cmd->name) : InvalidAttrNumber;

			if (attnum > 0)
				cmd->def = mys_fold_hexlit_default(cmd->def, get_atttype(relid, attnum));
		}

		if (cmd->subtype == AT_AddColumn && cmd->def && IsA(cmd->def, ColumnDef))
		{
			ColumnDef  *col = (ColumnDef *) cmd->def;
			List	   *cons = NIL;
			ListCell   *lc2;
			DefElem    *kind = store_generated_column(col, col->colname);

			fold_column_default(col);
			if (kind)
			{
				generated = lappend(generated, kind);
				setup = true;
			}
			foreach(lc2, col->constraints)
			{
				Node	   *n = lfirst(lc2);

				if (IsA(n, CommentStmt))
					indexes = lappend(indexes,
									  make_comment(OBJECT_COLUMN,
												   lappend(relation_name_list(stmt->relation),
														   makeString(col->colname)),
												   ((CommentStmt *) n)->comment));
				else if (is_on_update_clause(n))
				{
					on_update_add = lappend(on_update_add, col->colname);
					setup = true;
				}
				else if (IsA(n, String))
					continue;
				else if (IsA(n, Constraint) && ((Constraint *) n)->contype == CONSTR_AUTOINC)
				{
					Constraint *check = make_identity_column(col, (Constraint *) n);

					if (check)
						cons = lappend(cons, check);
					cons = lappend(cons, n);
					setup = true;
				}
				else
				{
					/* UNIQUE / PRIMARY KEY of the column */
					if (IsA(n, Constraint))
					{
						Constraint *c = (Constraint *) n;

						name_key_constraint(&kn, c, col->colname);
						prefix_column_constraint(col, c);
					}
					cons = lappend(cons, n);
				}
			}
			col->constraints = cons;
		}
		else if (cmd->subtype == AT_AddConstraint && cmd->def && IsA(cmd->def, Constraint))
		{
			Constraint *c = (Constraint *) cmd->def;
			char	   *lookup = NULL;

			name_key_constraint(&kn, c, NULL);
			if (c->contype == CONSTR_KEY)
			{
				IndexStmt  *idx = (IndexStmt *) make_index_stmt(stmt->relation, c);

				prefix_key_parts(&idx->indexParams, false, false, &idx->accessMethod, &idx->options,
								 RangeVarGetRelid(stmt->relation, NoLock, true),
								 alter_added_columns(stmt));
				indexes = lappend(indexes, idx);
				continue;
			}
			if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
				lookup = prefix_key_parts(&c->keys, true, c->contype == CONSTR_PRIMARY, &c->access_method, &c->options,
										  RangeVarGetRelid(stmt->relation, NoLock, true),
										  alter_added_columns(stmt));
			if (lookup)
				lookups = lappend(lookups, make_lookup_index(stmt->relation, lookup));
			if (!normalize_index_constraint(c, stmt->relation, &indexes))
				continue;
		}
		cmds = lappend(cmds, cmd);
	}
	stmt->cmds = cmds;
	indexes = add_lookup_indexes(indexes, lookups, &kn);
	if (setup)
	{
		DoStmt	   *d = (DoStmt *) make_table_setup(stmt->relation, on_update_add, on_update_drop);

		d->args = list_concat(d->args, generated);
		indexes = lappend(indexes, d);
	}
	return indexes;
}

/* ----------------------------------------------------------------
 * Literal values assigned to columns: the bytes of a string literal
 * for a binary column; in non-strict sql_mode, lenient conversion
 * ----------------------------------------------------------------
 */
extern bool isStrictTransTablesOn;

/* value range of the MySQL integer types (aux_mysql domains) */
static void
integer_range(Oid typid, Oid base, int64 *lo, int64 *hi)
{
	static const struct
	{
		const char *name;
		int64		lo,
					hi;
	}			ranges[] = {
		{"tinyint", -128, 127}, {"tinyint signed", -128, 127},
		{"tinyint1", -128, 127}, {"tinyint1 signed", -128, 127},
		{"tinyint unsigned", 0, 255}, {"tinyint1 unsigned", 0, 255},
		{"smallint unsigned", 0, 65535},
		{"mediumint", -8388608, 8388607}, {"mediumint signed", -8388608, 8388607},
		{"mediumint unsigned", 0, 16777215},
		{"int unsigned", 0, PG_UINT32_MAX}, {"integer unsigned", 0, PG_UINT32_MAX},
		{"bigint unsigned", 0, PG_INT64_MAX},
		{NULL, 0, 0}
	};
	HeapTuple	tup;

	*lo = base == INT2OID ? PG_INT16_MIN : base == INT4OID ? PG_INT32_MIN : PG_INT64_MIN;
	*hi = base == INT2OID ? PG_INT16_MAX : base == INT4OID ? PG_INT32_MAX : PG_INT64_MAX;
	if (typid == base)
		return;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		return;
	for (int i = 0; ranges[i].name; i++)
	{
		if (strcmp(NameStr(((Form_pg_type) GETSTRUCT(tup))->typname), ranges[i].name) == 0)
		{
			*lo = ranges[i].lo;
			*hi = ranges[i].hi;
		}
	}
	ReleaseSysCache(tup);
}

static Node *
int_const(int32 v)
{
	A_Const    *c = makeNode(A_Const);

	c->val.ival.type = T_Integer;
	c->val.ival.ival = v;
	c->location = -1;
	return (Node *) c;
}

static Node *
int8_const(int64 v)
{
	A_Const    *c = makeNode(A_Const);
	TypeCast   *tc = makeNode(TypeCast);

	c->val.sval.type = T_String;
	c->val.sval.sval = psprintf(INT64_FORMAT, v);
	c->location = -1;
	tc->arg = (Node *) c;
	tc->typeName = makeTypeNameFromOid(INT8OID, -1);
	tc->location = -1;
	return (Node *) tc;
}

/*
 * Wrap a literal destined for column "att" in the matching
 * mysql.lenient_*() function (mys_semantics.c).
 */
static Node *
lenient_value(Node *val, Form_pg_attribute att)
{
	int32		typmod = att->atttypmod;
	Oid			base;
	TypeCast   *txt;
	char	   *fname;
	List	   *args;

	if (val == NULL || !IsA(val, A_Const) || ((A_Const *) val)->isnull)
		return val;
	base = getBaseTypeAndTypmod(att->atttypid, &typmod);

	txt = makeNode(TypeCast);
	txt->arg = val;
	txt->typeName = makeTypeNameFromOid(TEXTOID, -1);
	txt->location = -1;
	args = list_make1(txt);

	switch (base)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			{
				int64		lo,
							hi;

				integer_range(att->atttypid, base, &lo, &hi);
				fname = "lenient_int";
				args = lappend(args, int8_const(lo));
				args = lappend(args, int8_const(hi));
				break;
			}
		case NUMERICOID:
			fname = "lenient_numeric";
			args = lappend(args, int_const(typmod));
			break;
		case FLOAT4OID:
		case FLOAT8OID:
			fname = "lenient_float8";
			break;
		case VARCHAROID:
		case BPCHAROID:
			if (typmod < (int32) VARHDRSZ)
				return val;
			fname = "lenient_varchar";
			args = lappend(args, int_const(typmod - VARHDRSZ));
			break;
		default:
			return val;
	}
	return (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString(fname)),
								 args, COERCE_EXPLICIT_CALL, -1);
}

/* a string literal holding a backslash */
static bool
is_backslash_literal(Node *val)
{
	return val != NULL && IsA(val, A_Const) && !((A_Const *) val)->isnull &&
		IsA(&((A_Const *) val)->val, String) &&
		strchr(strVal(&((A_Const *) val)->val), '\\') != NULL;
}

/*
 * MySQL stores the bytes of a string literal assigned to a binary column
 * (BINARY, VARBINARY, BLOB) as they are, but PostgreSQL reads an untyped
 * literal for bytea with byteain, which takes backslashes as escapes:
 * 'a\\b' would fail and '\\x41' would store "A".  So a literal holding a
 * backslash becomes the bytea constant of its bytes.  A literal without one
 * reads as it is; one that is not valid text arrives as _binary X'hex'
 * (mys_normalize_query).
 */
static Node *
binary_value(Node *val, Form_pg_attribute att)
{
	A_Const    *c = (A_Const *) val;
	const char *s;
	size_t		len;
	char	   *hex;
	TypeCast   *tc;

	if (!is_backslash_literal(val) || mys_binary_base_type(att->atttypid) != BYTEAOID)
		return val;
	s = strVal(&c->val);
	len = strlen(s);
	hex = palloc(len * 2 + 3);
	hex[0] = '\\';
	hex[1] = 'x';
	hex[hex_encode(s, len, hex + 2) + 2] = '\0';
	c->val.sval.sval = hex;
	tc = makeNode(TypeCast);
	tc->arg = val;
	tc->typeName = makeTypeNameFromOid(BYTEAOID, -1);
	tc->location = c->location;
	return (Node *) tc;
}

/* the literal to assign to column "att" */
static Node *
column_value(Node *val, Form_pg_attribute att)
{
	val = binary_value(val, att);
	return isStrictTransTablesOn ? val : lenient_value(val, att);
}

static Form_pg_attribute
find_attribute(TupleDesc desc, const char *name)
{
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (!att->attisdropped && strcmp(NameStr(att->attname), name) == 0)
			return att;
	}
	return NULL;
}

/* SET col = literal, ... (UPDATE, ON DUPLICATE KEY UPDATE) */
static void
literal_targets(List *targets, TupleDesc desc)
{
	foreach_node(ResTarget, rt, targets)
	{
		Form_pg_attribute att;

		if (rt->name == NULL || rt->indirection != NIL)
			continue;
		att = find_attribute(desc, rt->name);
		if (att)
			rt->val = column_value(rt->val, att);
	}
}

/* INSERT ... VALUES (literal, ...) */
static void
literal_insert_values(InsertStmt *stmt, TupleDesc desc)
{
	SelectStmt *sel = (SelectStmt *) stmt->selectStmt;
	List	   *targets = NIL;

	if (sel == NULL || !IsA(sel, SelectStmt) || sel->valuesLists == NIL)
		return;
	if (stmt->cols != NIL)
	{
		foreach_node(ResTarget, rt, stmt->cols)
			targets = lappend(targets, rt->indirection == NIL ? find_attribute(desc, rt->name) : NULL);
	}
	else
	{
		for (int i = 0; i < desc->natts; i++)
		{
			if (!TupleDescAttr(desc, i)->attisdropped)
				targets = lappend(targets, TupleDescAttr(desc, i));
		}
	}

	foreach_ptr(List, row, sel->valuesLists)
	{
		ListCell   *vc;
		int			i = 0;

		foreach(vc, row)
		{
			Form_pg_attribute att = i < list_length(targets) ? list_nth(targets, i) : NULL;

			if (att)
				lfirst(vc) = column_value(lfirst(vc), att);
			i++;
		}
	}
}

static void
rewrite_update_stmt(UpdateStmt *stmt)
{
	Oid			relid;
	Relation	rel;

	if (isStrictTransTablesOn)
	{
		bool		any = false;

		/* only binary columns need a look at the table then */
		foreach_node(ResTarget, rt, stmt->targetList)
			any |= is_backslash_literal(rt->val);
		if (!any)
			return;
	}
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
		return;
	rel = relation_open(relid, AccessShareLock);
	literal_targets(stmt->targetList, RelationGetDescr(rel));
	relation_close(rel, NoLock);
}

/* ----------------------------------------------------------------
 * INSERT
 * ----------------------------------------------------------------
 */
static bool
name_in_list(List *names, const char *name)
{
	ListCell   *lc;

	foreach(lc, names)
	{
		if (strcmp((char *) lfirst(lc), name) == 0)
			return true;
	}
	return false;
}

/*
 * Tables that statements earlier in the batch being rewritten create: a
 * later INSERT ... ON DUPLICATE KEY UPDATE / REPLACE of the batch runs
 * against them, but the catalog does not show them yet (or still shows the
 * table that the batch drops and creates again).
 */
typedef struct BatchTable
{
	RangeVar   *relation;
	List	   *columns;		/* names of the columns REPLACE assigns */
	List	   *primary;		/* IndexElems of the PRIMARY KEY, or NIL */
	List	   *uniques;		/* IndexElem lists of the UNIQUE keys */
	Oid			like;			/* CREATE TABLE ... LIKE: the keys of this table */
	bool		known;			/* false: columns and keys unknown */
} BatchTable;

static List *batch_tables = NIL;

static BatchTable *
find_batch_table(RangeVar *rel)
{
	foreach_ptr(BatchTable, bt, batch_tables)
	{
		if (strcmp(bt->relation->relname, rel->relname) == 0 &&
			(bt->relation->schemaname == NULL || rel->schemaname == NULL ||
			 strcmp(bt->relation->schemaname, rel->schemaname) == 0))
			return bt;
	}
	return NULL;
}

static void
forget_batch_table(RangeVar *rel)
{
	BatchTable *bt;

	while ((bt = find_batch_table(rel)) != NULL)
		batch_tables = list_delete_ptr(batch_tables, bt);
}

/* a key of a batch table: its columns and prefix key parts (prefix_key_parts()) */
static void
add_batch_key(BatchTable *bt, List *parts, bool primary)
{
	List	   *elems = NIL;

	foreach_ptr(Node, part, parts)
	{
		IndexElem  *from = (IndexElem *) part;
		IndexElem  *ie = makeNode(IndexElem);

		if (IsA(part, String))
			ie->name = strVal(part);
		else if (IsA(part, IndexElem) && from->name)
			ie->name = from->name;
		else if (IsA(part, IndexElem) && from->indexcolname)
		{
			ie->expr = copyObject(from->expr);
			ie->indexcolname = from->indexcolname;
		}
		else
			return;				/* another expression: no MySQL key */
		ie->ordering = SORTBY_DEFAULT;
		ie->nulls_ordering = SORTBY_NULLS_DEFAULT;
		elems = lappend(elems, ie);
	}
	if (primary)
		bt->primary = elems;
	else
		bt->uniques = lappend(bt->uniques, elems);
}

static void
add_batch_column(BatchTable *bt, ColumnDef *col)
{
	bool		generated = false;

	foreach_ptr(Node, n, col->constraints)
	{
		Constraint *c = (Constraint *) n;

		if (!IsA(n, Constraint))
			continue;
		if (c->contype == CONSTR_GENERATED)
			generated = true;
		else if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
			add_batch_key(bt, list_make1(makeString(col->colname)),
						  c->contype == CONSTR_PRIMARY);
	}
	if (!generated)
		bt->columns = lappend(bt->columns, col->colname);
}

static void
add_batch_constraint(BatchTable *bt, Constraint *c)
{
	if ((c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE) && !c->deferrable)
		add_batch_key(bt, c->keys, c->contype == CONSTR_PRIMARY);
}

/* unique indexes that rewrite_create_stmt() / rewrite_alter_table() add */
static void
add_batch_indexes(BatchTable *bt, List *stmts)
{
	foreach_ptr(Node, n, stmts)
	{
		IndexStmt  *idx = (IndexStmt *) n;

		if (IsA(n, IndexStmt) && idx->unique && idx->whereClause == NULL)
			add_batch_key(bt, idx->indexParams, idx->primary);
	}
}

/* CREATE TABLE (after rewrite_create_stmt(), which added extra) */
static void
remember_create_table(CreateStmt *stmt, List *extra)
{
	BatchTable *bt;

	/* IF NOT EXISTS of a table that exists does nothing */
	if (extra == NIL || (stmt->if_not_exists && find_batch_table(stmt->relation)))
		return;
	forget_batch_table(stmt->relation);
	bt = palloc0(sizeof(BatchTable));
	bt->relation = stmt->relation;
	bt->known = stmt->inhRelations == NIL && stmt->partbound == NULL && stmt->ofTypename == NULL;
	foreach_ptr(Node, elt, stmt->tableElts)
	{
		if (IsA(elt, ColumnDef))
			add_batch_column(bt, (ColumnDef *) elt);
		else if (IsA(elt, Constraint))
			add_batch_constraint(bt, (Constraint *) elt);
		else if (IsA(elt, TableLikeClause) && list_length(stmt->tableElts) == 1)
		{
			RangeVar   *source = ((TableLikeClause *) elt)->relation;
			BatchTable *from = find_batch_table(source);

			if (from)
			{
				*bt = *from;
				bt->relation = stmt->relation;
				bt->columns = list_copy(from->columns);
				bt->uniques = list_copy(from->uniques);
			}
			else
				bt->like = RangeVarGetRelid(source, NoLock, true);
			bt->known = bt->known && (from || OidIsValid(bt->like));
		}
		else
			bt->known = false;
	}
	add_batch_indexes(bt, extra);
	batch_tables = lappend(batch_tables, bt);
}

/* ALTER TABLE of a batch table: added columns and keys */
static void
remember_alter_table(AlterTableStmt *stmt, List *extra)
{
	BatchTable *bt = find_batch_table(stmt->relation);

	if (bt == NULL)
		return;
	foreach_ptr(AlterTableCmd, cmd, stmt->cmds)
	{
		if (cmd == NULL)
			continue;
		if (cmd->subtype == AT_AddColumn && cmd->def && IsA(cmd->def, ColumnDef))
			add_batch_column(bt, (ColumnDef *) cmd->def);
		else if (cmd->subtype == AT_AddConstraint && cmd->def && IsA(cmd->def, Constraint))
			add_batch_constraint(bt, (Constraint *) cmd->def);
		else
			bt->known = false;
	}
	add_batch_indexes(bt, extra);
}

/*
 * ON DUPLICATE KEY UPDATE: a column name alone is the existing row's column
 * (VALUES(col), the new row's, is already EXCLUDED.col), which PostgreSQL
 * finds ambiguous between the table and EXCLUDED.  The grammar qualifies
 * the operands of a top-level operator; qualify the others too, but not
 * inside a subquery, which has tables of its own.
 */
static bool
qualify_upsert_columns(Node *node, void *refname)
{
	if (node == NULL)
		return false;
	if (IsA(node, ColumnRef))
	{
		ColumnRef  *cref = (ColumnRef *) node;

		if (list_length(cref->fields) == 1 && IsA(linitial(cref->fields), String))
			cref->fields = lcons(makeString(pstrdup(refname)), cref->fields);
		return false;
	}
	if (IsA(node, SubLink) || nodeTag(node) >= T_MysNodeBase)
		return false;
	return raw_expression_tree_walker(node, qualify_upsert_columns, refname);
}

/* The executor reports the final physical row, including BEFORE triggers.
 * Keeping this hidden RETURNING free of column references avoids granting
 * SELECT on otherwise unreadable columns just to obtain an affected count. */
static ReturningClause *
upsert_returning(void)
{
	ReturningClause *rc = makeNode(ReturningClause);

	rc->exprs = list_make1(make_target(NULL, int_const(0)));
	return rc;
}

static void
rewrite_upsert(InsertStmt *stmt, List *insert_cols, Relation rel, BatchTable *bt,
			   MysStmtInfo *info)
{
	OnConflictClause *oc = stmt->onConflictClause;
	bool replace;

	if (oc == NULL || oc->infer != NULL || oc->whereClause != NULL ||
		(oc->action != ONCONFLICT_UPDATE &&
		 (int) oc->action != (int) ONCONFLICT_REPLACE))
		return;
	replace = (int) oc->action == (int) ONCONFLICT_REPLACE;
	oc->allIndexes = true;
	oc->replace = replace;
	oc->action = replace ? ONCONFLICT_NOTHING : ONCONFLICT_UPDATE;

	if (replace)
		oc->targetList = NIL;
	else
	{
		char *refname = stmt->relation->alias ? stmt->relation->alias->aliasname
			: stmt->relation->relname;

		foreach_node(ResTarget, rt, oc->targetList)
			qualify_upsert_columns(rt->val, refname);
	}
	if (info)
	{
		stmt->returningClause = upsert_returning();
		stmt->returningInternal = true;
		info->upsert = replace ? MYS_UPSERT_REPLACE : MYS_UPSERT_UPDATE;
	}
}

static void
rewrite_insert_stmt(InsertStmt *stmt, MysStmtInfo *info)
{
	Oid			relid;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *insert_cols = NIL;
	OnConflictClause *oc = stmt->onConflictClause;
	BatchTable *bt = find_batch_table(stmt->relation);
	/* a RETURNING of the statement's own: no hidden one */
	MysStmtInfo *hidden = stmt->returningClause == NULL ? info : NULL;

	foreach_node(ResTarget, rt, stmt->cols)
		insert_cols = lappend(insert_cols, rt->name);

	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
	{
		/* table created earlier in the same batch: just its keys */
		rewrite_upsert(stmt, insert_cols, NULL, bt, hidden);
		return;
	}
	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	/*
	 * bytes of string literals for binary columns; non-strict sql_mode:
	 * closest valid value plus a warning
	 */
	literal_insert_values(stmt, tupdesc);
	if (oc && oc->action == ONCONFLICT_UPDATE)
		literal_targets(oc->targetList, tupdesc);

	/* ON DUPLICATE KEY UPDATE / REPLACE need an explicit arbiter in PG */
	rewrite_upsert(stmt, insert_cols, rel, bt, hidden);

	/*
	 * hidden RETURNING of the AUTO_INCREMENT column for LAST_INSERT_ID(),
	 * the first column (before rewrite_upsert()'s)
	 */
	if (hidden)
	{
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (!att->attisdropped && att->attidentity != '\0')
			{
				ReturningClause *rc = stmt->returningClause ? stmt->returningClause
					: makeNode(ReturningClause);

				rc->exprs = lcons(make_target(NULL,
											  make_colref(NULL, pstrdup(NameStr(att->attname)))),
								  rc->exprs);
				stmt->returningClause = rc;
				stmt->returningInternal = true;
				info->capture_insert_id = true;
				break;
			}
		}
	}

	relation_close(rel, NoLock);	/* keep the lock until end of xact */
}

/* ----------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------
 */
List *
mys_rewrite_raw_stmts(List *raw_stmts, const char *query_string, List **infos)
{
	List	   *result = NIL;
	ListCell   *lc;
	bool		stop = false;

	*infos = NIL;
	rewrite_warnings = NIL;
	batch_tables = NIL;
	foreach(lc, raw_stmts)
	{
		RawStmt    *rs = lfirst_node(RawStmt, lc);
		MysStmtInfo *info = palloc0(sizeof(MysStmtInfo));
		List	   *extra = NIL;
		ListCell   *lc2;

		info->group_start = true;
		if (IsA(rs->stmt, SelectStmt))
		{
			SelectStmt *sel = (SelectStmt *) rs->stmt;

			if (list_length(sel->fromClause) == 1 && IsA(linitial(sel->fromClause), RangeFunction))
			{
				RangeFunction *rf = linitial_node(RangeFunction, sel->fromClause);
				Node *fn = linitial((List *) linitial(rf->functions));

				if (IsA(fn, FuncCall))
				{
					FuncCall *fc = (FuncCall *) fn;

					info->diagnostic = list_length(fc->funcname) == 2 &&
						strcmp(strVal(linitial(fc->funcname)), "mysql") == 0 &&
						strcmp(strVal(lsecond(fc->funcname)), "show_diagnostics") == 0;
				}
			}
		}
		mys_txn_classify(rs->stmt, info);
		rs->stmt = mys_rewrite_show_create(rs->stmt);

		/* CREATE TABLE ... AS / CREATE VIEW: a literal 0x.. column is VARBINARY */
		if (IsA(rs->stmt, CreateTableAsStmt))
			mys_hexlit_columns_binary(((CreateTableAsStmt *) rs->stmt)->query);
		else if (IsA(rs->stmt, ViewStmt))
			mys_hexlit_columns_binary(((ViewStmt *) rs->stmt)->query);
		rewrite_expression_collations(rs->stmt);

		switch ((int) nodeTag(rs->stmt))
		{
			case T_MysVariableSetStmt:
				rs->stmt = ((MysVariableSetStmt *) rs->stmt)->varSetStmt;
				info->silent = true;
				break;
			case T_MysDoStmt:
				{
					/* DO: evaluate the expressions, answer OK as SET does */
					SelectStmt *sel = makeNode(SelectStmt);

					foreach_ptr(Node, expr, ((MysDoStmt *) rs->stmt)->exprs)
						sel->targetList = lappend(sel->targetList, make_target(NULL, expr));
					rs->stmt = (Node *) sel;
					info->silent = true;
					break;
				}
			case T_PrepareStmt:
			case T_DeallocateStmt:
				rs->stmt = mys_sql_ps_marker(rs->stmt, NULL);
				break;
			case T_ExecuteStmt:
				{
					/*
					 * The prepared statement runs as a query of its own after
					 * this one, followed by the rest of the packet: what comes
					 * after EXECUTE is parsed once it has run.
					 */
					ListCell   *next = lnext(raw_stmts, lc);

					rs->stmt = mys_sql_ps_marker(rs->stmt, next ?
												 query_string + lfirst_node(RawStmt, next)->stmt_location :
												 NULL);
					info->continued = true;
					stop = true;
					break;
				}
			case T_SelectStmt:
				if (rewrite_select_into_vars((SelectStmt *) rs->stmt))
					info->silent = true;
				else
					fix_target_names((SelectStmt *) rs->stmt, query_string,
									 rs->stmt_len > 0 ? rs->stmt_location + rs->stmt_len
									 : (int) strlen(query_string));
				break;
			case T_CreateStmt:
				extra = rewrite_create_stmt((CreateStmt *) rs->stmt);
				remember_create_table((CreateStmt *) rs->stmt, extra);
				break;
			case T_RenameStmt:
				{
					RenameStmt *rename = (RenameStmt *) rs->stmt;

					/* PG14's fork stored the destination RangeVar in object. */
					if (rename->renameType == OBJECT_TABLE && rename->object &&
						IsA(rename->object, RangeVar))
					{
						RangeVar *target = (RangeVar *) rename->object;

						rename->newname = target->relname;
						rename->object = NULL;
						if (target->schemaname)
						{
							AlterObjectSchemaStmt *move = makeNode(AlterObjectSchemaStmt);

							move->objectType = OBJECT_TABLE;
							move->relation = copyObject(rename->relation);
							move->relation->relname = target->relname;
							move->newschema = target->schemaname;
							move->missing_ok = rename->missing_ok;
							extra = list_make1(move);
						}
					}
					if (rename->relation && rename->renameType == OBJECT_TABLE)
						forget_batch_table(rename->relation);
					else if (rename->relation && find_batch_table(rename->relation))
						find_batch_table(rename->relation)->known = false;
					break;
				}
			case T_InsertStmt:
				{
					InsertStmt *ins = (InsertStmt *) rs->stmt;

					info->ignore = ins->onConflictClause && ins->onConflictClause->ignoreFailure;
					rewrite_insert_stmt(ins, info);
					info->is_write = true;
					if (ins->onConflictClause)
					{
						foreach_node(ResTarget, rt, ins->onConflictClause->targetList)
							info->update_cols = lappend(info->update_cols, rt->name);
					}
					break;
				}
			case T_UpdateStmt:
				rewrite_update_stmt((UpdateStmt *) rs->stmt);
				info->is_write = true;
				foreach_node(ResTarget, rt, ((UpdateStmt *) rs->stmt)->targetList)
					info->update_cols = lappend(info->update_cols, rt->name);
				break;
			case T_IndexStmt:
				{
					IndexStmt  *idx = (IndexStmt *) rs->stmt;
					char	   *lookup;
					BatchTable *bt;
					KeyNamer	kn;

					init_key_namer(&kn, idx->relation, false, true);
					name_key(&kn, idx->idxname, first_key_column(idx->indexParams),
							 &idx->idxname, &idx->options);
					lookup = prefix_key_parts(&idx->indexParams, idx->unique, false,
											  &idx->accessMethod, &idx->options,
											  RangeVarGetRelid(idx->relation, NoLock, true), NIL);
					if (lookup)
						extra = add_lookup_indexes(NIL, list_make1(make_lookup_index(idx->relation, lookup)),
												   &kn);
					if ((bt = find_batch_table(idx->relation)) != NULL)
						add_batch_indexes(bt, list_make1(idx));
					break;
				}
			case T_CreateTableAsStmt:
				{
					CreateTableAsStmt *ctas = (CreateTableAsStmt *) rs->stmt;

					/* a table without keys */
					if (ctas->objtype == OBJECT_TABLE &&
						!(ctas->if_not_exists &&
						  (find_batch_table(ctas->into->rel) ||
						   OidIsValid(RangeVarGetRelid(ctas->into->rel, NoLock, true)))))
					{
						BatchTable *bt = palloc0(sizeof(BatchTable));

						forget_batch_table(ctas->into->rel);
						bt->relation = ctas->into->rel;
						bt->known = true;
						batch_tables = lappend(batch_tables, bt);
					}
					break;
				}
			case T_DropStmt:
				if (((DropStmt *) rs->stmt)->removeType == OBJECT_TABLE)
				{
					foreach_node(List, names, ((DropStmt *) rs->stmt)->objects)
						forget_batch_table(makeRangeVarFromNameList(names));
				}
				break;
			case T_AlterTableStmt:
				extra = rewrite_alter_table((AlterTableStmt *) rs->stmt);
				remember_alter_table((AlterTableStmt *) rs->stmt, extra);
				if (((AlterTableStmt *) rs->stmt)->cmds == NIL && extra != NIL)
				{
					/* nothing left but the indexes */
					rs->stmt = linitial(extra);
					extra = list_delete_first(extra);
				}
				break;
			default:
				break;
		}

		info->rewrite_warnings = rewrite_warnings;
		rewrite_warnings = NIL;
		result = lappend(result, rs);
		*infos = lappend(*infos, info);

		/*
		 * Extra statements implement one MySQL statement: only the last one
		 * of the group answers the client.
		 */
		foreach(lc2, extra)
		{
			RawStmt    *ers = makeNode(RawStmt);
			MysStmtInfo *einfo = palloc0(sizeof(MysStmtInfo));

			einfo->txn_kind = info->txn_kind;
			einfo->set_autocommit = -1;
			ers->stmt = lfirst(lc2);
			ers->stmt_location = rs->stmt_location;
			ers->stmt_len = rs->stmt_len;
			info->suppress_response = true;
			result = lappend(result, ers);
			*infos = lappend(*infos, einfo);
			info = einfo;
		}
		if (stop)
			break;
	}
	batch_tables = NIL;
	return result;
}
