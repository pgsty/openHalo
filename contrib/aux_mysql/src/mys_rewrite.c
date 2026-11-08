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
#include "utils/formatting.h"
#include "nodes/makefuncs.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "halo_mysql.h"
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

static int
expr_text_end(const char *q, int start, int end)
{
	int			depth = 0;
	int			i = start;

	while (i < end)
	{
		char		c = q[i];

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
			continue;
		}
		if (c == '(')
			depth++;
		else if (c == ')')
		{
			if (depth == 0)
				return i;
			depth--;
		}
		else if (depth == 0 && (c == ',' || c == ';'))
			return i;
		else if (depth == 0 && isalpha((unsigned char) c) &&
				 (i == start || !(isalnum((unsigned char) q[i - 1]) || q[i - 1] == '_')))
		{
			for (int k = 0; clause_keywords[k]; k++)
			{
				int			len = strlen(clause_keywords[k]);

				if (i + len <= end && pg_strncasecmp(q + i, clause_keywords[k], len) == 0 &&
					(i + len == end || !(isalnum((unsigned char) q[i + len]) || q[i + len] == '_')))
					return i;
			}
		}
		i++;
	}
	return end;
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
	return base == TEXTOID || base == VARCHAROID || base == BPCHAROID;
}

/* MySQL collation names ending in _ci compare case-insensitively */
static bool
mysql_collation_is_ci(const char *name)
{
	size_t		len = strlen(name);

	return len > 3 && pg_strcasecmp(name + len - 3, "_ci") == 0;
}

/*
 * MySQL compares strings with the column's collation, by default a
 * case-insensitive one (utf8mb4_0900_ai_ci).  Give string columns declared
 * without COLLATE the configured nondeterministic collation (PG18 supports
 * them for =, LIKE, GROUP BY, unique indexes ...).
 */
/*
 * Map the collation of a MySQL column definition to a PostgreSQL one
 * (openHalo 14.18, rectifyColumnCollate):
 *
 *	COLLATE xxx_ci                 -> mysql.case_insensitive
 *	COLLATE <other> (utf8mb4_bin)  -> mysql.case_sensitive (code point order)
 *	no COLLATE                     -> mysql.default_collation, or
 *	                                  case_sensitive when the table's
 *	                                  COLLATE= is not a _ci collation
 *
 * Collation names shown by SHOW CREATE TABLE (case_insensitive,
 * case_sensitive, ignore_accents, default) are accepted as such, so its
 * output can be replayed.  COLLATE on a non-string column is ignored, as
 * openHalo did.
 */
void
mys_rectify_column_collation(ColumnDef *col, const char *table_collation)
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
		col->collClause = make_collate_clause("mysql", asc_tolower(name, strlen(name)));
	else if (pg_strcasecmp(name, "default") == 0)
		col->collClause = make_collate_clause("pg_catalog", "default");
	else if (mysql_collation_is_ci(name))
		col->collClause = make_collate_clause("mysql", "case_insensitive");
	else
		col->collClause = make_collate_clause("mysql", "case_sensitive");
}

/* table-level COLLATE=: case-insensitive unless a non-_ci collation is given */
static const char *
table_collation_name(List *options)
{
	foreach_node(DefElem, d, options)
	{
		if (strcmp(d->defname, "collate") == 0 && d->arg && IsA(d->arg, List))
		{
			char *name = strVal(llast((List *) d->arg));

			if (pg_strcasecmp(name, "case_insensitive") == 0 ||
				pg_strcasecmp(name, "case_sensitive") == 0 ||
				pg_strcasecmp(name, "ignore_accents") == 0 ||
				pg_strcasecmp(name, "default") == 0)
				return asc_tolower(name, strlen(name));
			return mysql_collation_is_ci(name) ? "case_insensitive" : "case_sensitive";
		}
	}
	return halo_mysql_default_collation && halo_mysql_default_collation[0] ?
		halo_mysql_default_collation : "default";
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
 * Warnings found while rewriting: the diagnostics area is reset after the
 * packet is rewritten (it depends on whether it is SHOW WARNINGS), so they
 * are kept here until mys_flush_rewrite_warnings().
 */
static List *rewrite_warnings = NIL;

static void
add_rewrite_warning(const char *message)
{
	rewrite_warnings = lappend(rewrite_warnings, pstrdup(message));
}

void
mys_flush_rewrite_warnings(void)
{
	foreach_ptr(char, message, rewrite_warnings)
		mys_add_warning(1105, message);
	rewrite_warnings = NIL;
}

/* a statement rewritten only to describe it (COM_STMT_PREPARE) */
void
mys_discard_rewrite_warnings(void)
{
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
 * pg_catalog.left(col, n) / pg_catalog.substr(col, 1, n): the prefix of a
 * column; digest: its pg_catalog.md5(), for UNIQUE BLOB prefixes that may not
 * fit a btree entry together (MySQL allows 3072 bytes).
 */
static Node *
prefix_expr(const char *column, bool bytes, int length, bool digest)
{
	ColumnRef  *ref = makeNode(ColumnRef);
	Node	   *prefix;

	ref->fields = list_make1(makeString(pstrdup(column)));
	ref->location = -1;
	prefix = (Node *) makeFuncCall(list_make2(makeString("pg_catalog"),
											  makeString(bytes ? "substr" : "left")),
								   bytes ? list_make3(ref, int_const(1), int_const(length)) :
								   list_make2(ref, int_const(length)),
								   COERCE_EXPLICIT_CALL, -1);
	if (!digest)
		return prefix;
	return (Node *) makeFuncCall(list_make2(makeString("pg_catalog"), makeString("md5")),
								 list_make1(prefix), COERCE_EXPLICIT_CALL, -1);
}

/*
 * MySQL prefix key parts, col(n), marked by the grammar.  A btree entry holds
 * ~2.7kB, so a key must not index whole values of a "long" column (TEXT,
 * BLOB, VARCHAR / CHAR over 600 characters), or a long value would fail an
 * INSERT that MySQL accepts:
 *
 * - A non-unique key starting with a long column, prefixed or (TEXT / BLOB,
 *   where MySQL wants a prefix) not, becomes a hash index on that column: it
 *   serves = lookups on it, as the key did.  Other long parts become
 *   left(col, n), the n of all of them together at most 600 characters.
 * - A UNIQUE key enforces the prefix of a long column as MySQL does, on
 *   left(col, n) (ON DUPLICATE KEY infers it as its arbiter); when the
 *   prefixes of a key may exceed 2000 bytes together, its BLOB prefixes are
 *   compared by MD5 (text keeps its collation's comparison).  When the key starts with one, the
 *   column is returned: the caller adds a hash index on it for = lookups,
 *   which left(col, n) cannot serve.
 * - Values of other columns fit an entry, and they are indexed whole: lookups
 *   and foreign keys keep using the key, but a UNIQUE or PRIMARY KEY prefix
 *   shorter than the column then accepts rows that differ only after it; a
 *   warning says so.  A PRIMARY KEY, which cannot have expressions, is
 *   always indexed whole.
 */
static char *
prefix_key_parts(List **params, bool unique, bool primary, char **access_method,
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
			is_long[i] = base[i] == TEXTOID || base[i] == BYTEAOID ||
				((base[i] == VARCHAROID || base[i] == BPCHAROID) &&
				 (typmod < (int32) VARHDRSZ || typmod - VARHDRSZ > 600));
			/* a prefix as long as the column is the whole column */
			if (typmod >= (int32) VARHDRSZ && length[i] >= typmod - VARHDRSZ)
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
	if (access_method && strcmp(*access_method, DEFAULT_INDEX_TYPE) != 0)
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
				add_rewrite_warning(psprintf("prefix of unique key part \"%s\" is enforced on the whole column",
											 ie->name));
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

/*
 * A key part prefix_key_parts() made: pg_catalog.left(col, n) or
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

/*
 * Lookup indexes go after the statement's other keys, so that they cannot
 * take the name of one (PostgreSQL picks a free name), and are left out when
 * a non-unique key of the statement starts with the same column.
 */
static List *
add_lookup_indexes(List *indexes, List *lookups)
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
			indexes = lappend(indexes, lookup);
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

static List *
rewrite_create_stmt(CreateStmt *stmt)
{
	List	   *elts = NIL;
	List	   *indexes = NIL;
	List	   *lookups = NIL;
	List	   *comments = NIL;
	List	   *options = NIL;
	List	   *on_update = NIL;
	Constraint *identity = NULL;
	ListCell   *lc;
	const char *table_collation = table_collation_name(stmt->options);

	if (stmt->if_not_exists && OidIsValid(RangeVarGetRelid(stmt->relation, NoLock, true)))
		return NIL;
	/* LIKE copies the saved table default, independently of column overrides. */
	foreach_ptr(Node, elt, stmt->tableElts)
		if (IsA(elt, TableLikeClause))
		{
			Oid source = RangeVarGetRelid(((TableLikeClause *) elt)->relation,
										 AccessShareLock, false);
			table_collation = mys_table_default_collation(source);
		}

	foreach(lc, stmt->tableElts)
	{
		Node	   *elt = lfirst(lc);

		if (IsA(elt, ColumnDef))
		{
			ColumnDef  *col = (ColumnDef *) elt;
			List	   *cons = NIL;
			ListCell   *lc2;

			mys_rectify_column_collation(col, table_collation);
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
					indexes = lappend(indexes, make_index_stmt(stmt->relation, (Constraint *) n));
				else
					cons = lappend(cons, n);
			}
			col->constraints = cons;
			elts = lappend(elts, col);
		}
		else if (IsA(elt, Constraint) && ((Constraint *) elt)->contype == CONSTR_KEY)
			indexes = lappend(indexes, make_index_stmt(stmt->relation, (Constraint *) elt));
		else if (IsA(elt, Constraint))
		{
			Constraint *c = (Constraint *) elt;
			char	   *lookup = NULL;

			if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
				lookup = prefix_key_parts(&c->keys, true, c->contype == CONSTR_PRIMARY, NULL,
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
							 &((IndexStmt *) n)->accessMethod, InvalidOid, stmt->tableElts);
	indexes = add_lookup_indexes(indexes, lookups);
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
	bool		setup = false;
	ListCell   *lc;

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
			setup = true;
		}

		if (cmd->subtype == AT_AddColumn && cmd->def && IsA(cmd->def, ColumnDef))
		{
			ColumnDef  *col = (ColumnDef *) cmd->def;
			List	   *cons = NIL;
			ListCell   *lc2;

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
					cons = lappend(cons, n);
			}
			col->constraints = cons;
		}
		else if (cmd->subtype == AT_AddConstraint && cmd->def && IsA(cmd->def, Constraint))
		{
			Constraint *c = (Constraint *) cmd->def;
			char	   *lookup = NULL;

			if (c->contype == CONSTR_KEY)
			{
				IndexStmt  *idx = (IndexStmt *) make_index_stmt(stmt->relation, c);

				prefix_key_parts(&idx->indexParams, false, false, &idx->accessMethod,
								 RangeVarGetRelid(stmt->relation, NoLock, true),
								 alter_added_columns(stmt));
				indexes = lappend(indexes, idx);
				continue;
			}
			if (c->contype == CONSTR_PRIMARY || c->contype == CONSTR_UNIQUE)
				lookup = prefix_key_parts(&c->keys, true, c->contype == CONSTR_PRIMARY, NULL,
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
	indexes = add_lookup_indexes(indexes, lookups);
	if (setup)
		indexes = lappend(indexes, make_table_setup(stmt->relation, on_update_add, on_update_drop));
	return indexes;
}

/* ----------------------------------------------------------------
 * Non-strict sql_mode: lenient conversion of literal values
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
lenient_targets(List *targets, TupleDesc desc)
{
	foreach_node(ResTarget, rt, targets)
	{
		Form_pg_attribute att;

		if (rt->name == NULL || rt->indirection != NIL)
			continue;
		att = find_attribute(desc, rt->name);
		if (att)
			rt->val = lenient_value(rt->val, att);
	}
}

/* INSERT ... VALUES (literal, ...) */
static void
lenient_insert_values(InsertStmt *stmt, TupleDesc desc)
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
				lfirst(vc) = lenient_value(lfirst(vc), att);
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
		return;
	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
		return;
	rel = relation_open(relid, AccessShareLock);
	lenient_targets(stmt->targetList, RelationGetDescr(rel));
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
 * Pick the unique index MySQL would most likely hit: the primary key if all
 * its columns are supplied, else the first unique index whose columns are
 * all supplied, else the primary key.  A prefix key part counts as its
 * column; other expressions make an index unusable.
 */
static Oid
choose_arbiter_index(Relation rel, List *insert_cols)
{
	List	   *indexes = RelationGetIndexList(rel);
	Oid			pk = InvalidOid;
	Oid			covered = InvalidOid;
	ListCell   *lc;

	foreach(lc, indexes)
	{
		Oid			indexoid = lfirst_oid(lc);
		Relation	idx = index_open(indexoid, AccessShareLock);
		Form_pg_index form = idx->rd_index;
		bool		usable = form->indisunique && form->indimmediate &&
			heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL);
		bool		all = usable;
		List	   *exprs = usable ? RelationGetIndexExpressions(idx) : NIL;
		ListCell   *expr = list_head(exprs);

		for (int i = 0; usable && i < form->indnkeyatts; i++)
		{
			AttrNumber	attnum = form->indkey.values[i];
			int			length;
			bool		bytes;
			bool		digest;

			if (attnum == 0 && expr != NULL)
			{
				if (!mys_index_prefix(lfirst(expr), &attnum, &length, &bytes, &digest))
					attnum = 0;
				expr = lnext(exprs, expr);
			}
			if (attnum <= 0)
			{
				usable = all = false;
				break;
			}
			if (insert_cols != NIL &&
				!name_in_list(insert_cols, get_attname(RelationGetRelid(rel), attnum, false)))
				all = false;
		}
		if (usable && form->indisprimary)
			pk = indexoid;
		if (all && (!OidIsValid(covered) || form->indisprimary))
			covered = indexoid;
		index_close(idx, AccessShareLock);
	}
	return OidIsValid(covered) ? covered : pk;
}

static InferClause *
make_infer_clause(Relation rel, Oid indexoid)
{
	InferClause *infer = makeNode(InferClause);
	Oid			conoid = get_index_constraint(indexoid);

	infer->location = -1;
	if (OidIsValid(conoid))
		infer->conname = get_constraint_name(conoid);
	else
	{
		Relation	idx = index_open(indexoid, AccessShareLock);
		List	   *exprs = RelationGetIndexExpressions(idx);
		ListCell   *expr = list_head(exprs);

		for (int i = 0; i < idx->rd_index->indnkeyatts; i++)
		{
			IndexElem  *ie = makeNode(IndexElem);
			AttrNumber	attnum = idx->rd_index->indkey.values[i];
			int			length;
			bool		bytes;
			bool		digest;

			/* a prefix key part (choose_arbiter_index() took no other expression) */
			if (attnum == 0 && mys_index_prefix(lfirst(expr), &attnum, &length, &bytes, &digest))
			{
				ie->expr = prefix_expr(get_attname(RelationGetRelid(rel), attnum, false),
									   bytes, length, digest);
				expr = lnext(exprs, expr);
			}
			else
				ie->name = get_attname(RelationGetRelid(rel), attnum, false);
			ie->ordering = SORTBY_DEFAULT;
			ie->nulls_ordering = SORTBY_NULLS_DEFAULT;
			infer->indexElems = lappend(infer->indexElems, ie);
		}
		index_close(idx, AccessShareLock);
	}
	return infer;
}

static void
rewrite_insert_stmt(InsertStmt *stmt, MysStmtInfo *info)
{
	Oid			relid;
	Relation	rel;
	TupleDesc	tupdesc;
	List	   *insert_cols = NIL;
	OnConflictClause *oc = stmt->onConflictClause;
	ListCell   *lc;

	relid = RangeVarGetRelid(stmt->relation, NoLock, true);
	if (!OidIsValid(relid))
	{
		/* table created later in the same batch: no MySQL extras */
		if (oc && (int) oc->action == (int) ONCONFLICT_REPLACE)
			oc->action = ONCONFLICT_UPDATE;
		return;
	}
	rel = relation_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	foreach(lc, stmt->cols)
		insert_cols = lappend(insert_cols, lfirst_node(ResTarget, lc)->name);

	/* non-strict sql_mode: closest valid value plus a warning */
	if (!isStrictTransTablesOn)
	{
		lenient_insert_values(stmt, tupdesc);
		if (oc && oc->action == ONCONFLICT_UPDATE)
			lenient_targets(oc->targetList, tupdesc);
	}

	/* ON DUPLICATE KEY UPDATE / REPLACE need an explicit arbiter in PG */
	if (oc && oc->infer == NULL &&
		(oc->action == ONCONFLICT_UPDATE || (int) oc->action == (int) ONCONFLICT_REPLACE))
	{
		Oid			arbiter = choose_arbiter_index(rel, insert_cols);

		if (OidIsValid(arbiter))
			oc->infer = make_infer_clause(rel, arbiter);

		if ((int) oc->action == (int) ONCONFLICT_REPLACE)
		{
			/* REPLACE: overwrite every supplied column with the new values */
			oc->action = ONCONFLICT_UPDATE;
			oc->targetList = NIL;
			for (int i = 0; i < tupdesc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tupdesc, i);
				char	   *name = NameStr(att->attname);

				if (att->attisdropped || att->attgenerated)
					continue;
				if (insert_cols != NIL && !name_in_list(insert_cols, name))
					continue;
				oc->targetList = lappend(oc->targetList,
										 make_target(pstrdup(name),
													 make_colref("excluded", pstrdup(name))));
			}
		}
	}

	/* hidden RETURNING of the AUTO_INCREMENT column for LAST_INSERT_ID() */
	if (stmt->returningClause == NULL)
	{
		for (int i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (!att->attisdropped && att->attidentity != '\0')
			{
				ReturningClause *rc = makeNode(ReturningClause);

				rc->exprs = list_make1(make_target(NULL,
												   make_colref(NULL, pstrdup(NameStr(att->attname)))));
				stmt->returningClause = rc;
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

	*infos = NIL;
	rewrite_warnings = NIL;
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

		switch ((int) nodeTag(rs->stmt))
		{
			case T_MysVariableSetStmt:
				rs->stmt = ((MysVariableSetStmt *) rs->stmt)->varSetStmt;
				info->silent = true;
				break;
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
					break;
				}
			case T_InsertStmt:
				{
					InsertStmt *ins = (InsertStmt *) rs->stmt;

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

					lookup = prefix_key_parts(&idx->indexParams, idx->unique, false,
											  &idx->accessMethod,
											  RangeVarGetRelid(idx->relation, NoLock, true), NIL);
					if (lookup)
						extra = list_make1(make_lookup_index(idx->relation, lookup));
					break;
				}
			case T_AlterTableStmt:
				extra = rewrite_alter_table((AlterTableStmt *) rs->stmt);
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
	}
	return result;
}
