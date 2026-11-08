#!/usr/bin/env python3
"""
Port openHalo's PG14-based MySQL grammar (mys_gram.y) to PostgreSQL 18 parse
node structures.

    port_grammar.py <openhalo mys_gram.y> <output mys_gram.y>

Every transformation is an exact-text (or tightly anchored) replacement that
asserts its expected match count, so upstream changes fail loudly instead of
silently producing a broken grammar.
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8', errors='surrogateescape').read()


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if count is not None and n != count:
        sys.exit("expected %d match(es), got %d for: %r" % (count, n, old[:120]))
    if n == 0:
        sys.exit("no match for: %r" % old[:120])
    s = s.replace(old, new)


def resub(pattern, repl, expect=None, flags=0):
    global s
    s, n = re.subn(pattern, repl, s, flags=flags)
    if expect is not None and n != expect:
        sys.exit("pattern %r: expected %d, got %d" % (pattern, expect, n))


# ---------------------------------------------------------------------------
# 0. includes
# ---------------------------------------------------------------------------
rep('#include "parser/mysql/mys_gramparse.h"', '#include "mys_gramparse.h"')
rep('#include "parser/mysql/mys_parser.h"', '#include "mys_parser.h"')
rep('#include "parser/parser_helper.h"', '#include "parser_helper.h"')
rep('#include "adapter/mysql/systemVar.h"\n', '')
rep('#include "miscadmin.h"\n', '''#include "miscadmin.h"

/* PG18 exports a makeStringConst() of its own; keep the grammar's copy private */
#define makeStringConst mys_makeStringConst

/* openHalo kept this flag in the core executor; it is only parsed here */
static bool isIgnoreStmt = false;
''')

# ---------------------------------------------------------------------------
# 1. PG15 removed the Value node; A_Const holds a union of value nodes
# ---------------------------------------------------------------------------
rep('static Node *makeAConst(Value *v, int location);', 'static Node *makeAConst(Node *v, int location);')
rep('static void doNegateFloat(Value *v);', 'static void doNegateFloat(Float *v);')
rep('static RawStmt *makeRawStmt(Node *stmt, int stmt_location);', '''static RawStmt *makeRawStmt(Node *stmt, int stmt_location);
static ReturningClause *makeReturningClauseFromList(List *exprs);
static List *makePublicationTables(List *rels);
static PartitionStrategy parsePartitionStrategy(char *strategy, int location,
												core_yyscan_t yyscanner);
static Node *makeLimitedModifyQual(RangeVar *relation, Node *whereClause,
								   List *sortClause, SelectLimit *limit,
								   int location);''')
rep('\tValue\t\t\t\t*value;\n', '')
rep('%type <value>\tTriggerFuncArg', '%type <node>\tTriggerFuncArg')
rep('%type <value>\tNumericOnly', '%type <node>\tNumericOnly')

rep('''	n->val.type = T_String;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;
}

static Node *
makeStringConstCast''', '''	n->val.sval.type = T_String;
	n->val.sval.sval = str;
	n->location = location;

	return (Node *)n;
}

static Node *
makeStringConstCast''')
rep('''	n->val.type = T_Integer;
	n->val.val.ival = val;''', '''	n->val.ival.type = T_Integer;
	n->val.ival.ival = val;''')
rep('''	n->val.type = T_Float;
	n->val.val.str = str;''', '''	n->val.fval.type = T_Float;
	n->val.fval.fval = str;''')
rep('''	n->val.type = T_BitString;
	n->val.val.str = str;''', '''	n->val.bsval.type = T_BitString;
	n->val.bsval.bsval = str;''')
rep('''	A_Const *n = makeNode(A_Const);

	n->val.type = T_ByteaString;
	n->val.val.str = str;
	n->location = location;

	return (Node *)n;''', '''	/*
	 * openHalo had a special A_Const value type for binary strings; PG18 has
	 * none, so emit '\\\\x<hex>'::bytea.  The scanner delivers two shapes:
	 *	 _binary X'ABCD'  -> "xABCD"
	 *	 _binary '...'    -> 16 byte NUL padded decimal length + raw bytes
	 *						 (escapes already processed, may contain NULs)
	 */
	StringInfoData buf;

	if (str[0] == 'x' || str[0] == 'X')
		return makeStringConstCast(psprintf("\\\\x%s", str + 1), location,
								   SystemTypeName("bytea"));

	initStringInfo(&buf);
	appendStringInfoString(&buf, "\\\\x");
	{
		int			len = atoi(str);
		const unsigned char *p = (const unsigned char *) str + 16;

		for (int i = 0; i < len; i++)
			appendStringInfo(&buf, "%02x", p[i]);
	}
	return makeStringConstCast(buf.data, location, SystemTypeName("bytea"));''')
rep('''	n->val.type = T_Null;
	n->location = location;''', '''	n->isnull = true;
	n->location = location;''')
rep('''makeAConst(Value *v, int location)
{
	Node *n;

	switch (v->type)
	{
		case T_Float:
			n = makeFloatConst(v->val.str, location);
			break;

		case T_Integer:
			n = makeIntConst(v->val.ival, location);
			break;

		case T_String:
		default:
			n = makeStringConst(v->val.str, location);
			break;
	}''', '''makeAConst(Node *v, int location)
{
	Node *n;

	switch (v->type)
	{
		case T_Float:
			n = makeFloatConst(castNode(Float, v)->fval, location);
			break;

		case T_Integer:
			n = makeIntConst(castNode(Integer, v)->ival, location);
			break;

		case T_String:
		default:
			n = makeStringConst(strVal(v), location);
			break;
	}''')
rep('''	n->val.type = T_String;
	n->val.val.str = (state ? "t" : "f");
	n->location = location;

	return makeTypeCast((Node *)n, SystemTypeName("bool"), -1);''', '''	n->val.boolval.type = T_Boolean;
	n->val.boolval.boolval = state;
	n->location = location;

	return (Node *) n;''')
rep('''		if (con->val.type == T_Integer)
		{
			con->val.val.ival = -con->val.val.ival;
			return n;
		}
		if (con->val.type == T_Float)
		{
			doNegateFloat(&con->val);
			return n;
		}''', '''		if (IsA(&con->val, Integer))
		{
			con->val.ival.ival = -con->val.ival.ival;
			return n;
		}
		if (IsA(&con->val, Float))
		{
			doNegateFloat(&con->val.fval);
			return n;
		}''')
rep('''doNegateFloat(Value *v)
{
	char   *oldval = v->val.str;

	Assert(IsA(v, Float));
	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->val.str = oldval+1;	/* just strip the '-' */
	else
		v->val.str = psprintf("-%s", oldval);''', '''doNegateFloat(Float *v)
{
	char   *oldval = v->fval;

	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->fval = oldval+1;	/* just strip the '-' */
	else
		v->fval = psprintf("-%s", oldval);''')
rep('\tValue\t   *ndirectargs;', '\tInteger\t   *ndirectargs;')

# A_Const accessors inside rule actions
rep('((A_Const*)(defElem->arg))->val.type == T_Integer', 'IsA(&((A_Const*)(defElem->arg))->val, Integer)', 3)
rep('((A_Const*)(defElem->arg))->val.val.ival', '((A_Const*)(defElem->arg))->val.ival.ival', 3)
rep('((A_Const*)(defElem->arg))->val.val.str', '((A_Const*)(defElem->arg))->val.sval.sval', 3)
rep('rel->val.val.str', 'rel->val.sval.sval', None)
rep('n->val.val.ival & ~(INTERVAL_MASK', 'n->val.ival.ival & ~(INTERVAL_MASK')
rep('if (n->val.type == T_String)', 'if (IsA(&n->val, String))', 3)
# list members that used to be Value* are String nodes
resub(r'\bValue\s*\*\s*(\w+)\s*=\s*\(Value\s*\*\)', r'Node *\1 = (Node *)')
rep('((Value *)lsecond($2->names))->val.str', 'strVal(lsecond($2->names))')
rep('''                    int len;
                    Value *v;
''', '''                    int len;
                    String *v;
''')
resub(r'\b(v|val|value|unit)->val\.str\b', r'strVal(\1)')

# ---------------------------------------------------------------------------
# 2. Node fields that changed between PG14 and PG18
# ---------------------------------------------------------------------------
rep('                    n->inProc = false;\n', '')                       # openHalo-only field
rep('''					n->strategy = $3;
					n->partParams = $5;''', '''					n->strategy = parsePartitionStrategy($3, @3, yyscanner);
					n->partParams = $5;''')
rep('\t\t\t\t\tn->admin_opt = $5;\n',
    '\t\t\t\t\tn->opt = $5 ? list_make1(makeDefElem("admin", (Node *) makeBoolean(true), @5)) : NIL;\n')
rep('\t\t\t\t\tn->admin_opt = false;\n', '\t\t\t\t\tn->opt = NIL;\n')
rep('\t\t\t\t\tn->admin_opt = true;\n',
    '\t\t\t\t\tn->opt = list_make1(makeDefElem("admin", (Node *) makeBoolean(false), @2));\n')
rep('n->oldNode = InvalidOid;', 'n->oldNumber = InvalidRelFileNumber;', None)
rep('n->oldFirstRelfilenodeSubid = InvalidSubTransactionId;',
    'n->oldFirstRelfilelocatorSubid = InvalidSubTransactionId;', None)
rep('\t\t\t\t\t\t\tn->tables = (List *)$4;', '\t\t\t\t\t\t\tn->pubobjects = makePublicationTables((List *) $4);')
for act, pg in (('ADD', 'Add'), ('SET', 'Set'), ('DROP', 'Drop')):
    rep('''					n->tables = $6;
					n->tableAction = DEFELEM_%s;''' % act, '''					n->pubobjects = makePublicationTables($6);
					n->action = AP_%sObjects;''' % pg)
resub(r'(\w+|\$\d+)->returningList\s*=\s*([^;]+);', r'\1->returningClause = makeReturningClauseFromList(\2);')
rep('LIMIT_OPTION_DEFAULT', 'LIMIT_OPTION_COUNT', None)

# UPDATE t1 JOIN t2 ON cond SET ... WHERE w  ->  UPDATE t1 SET ... FROM t2 WHERE cond AND w
rep('''                    n->fromClause = $6;
                    n->onClause = $7;
                    if ($4 == JOIN_INNER)
                    {
                        n->whereClause = $10;''', '''                    n->fromClause = $6;
                    if ($4 == JOIN_INNER)
                    {
                        n->whereClause = $10 ? makeAndExpr($7, $10, -1) : $7;''')
rep('                    n->onClause = NULL;\n', '')

# DELETE/UPDATE ... ORDER BY ... LIMIT: PG has no such syntax; turn the
# limit into a (tableoid, ctid) IN (SELECT ... ORDER BY ... LIMIT ...) qual.
lines = s.split('\n')
out = []
i = 0
nlimit = 0
while i < len(lines):
    m = re.match(r'^( +)n->sortClause = \$(\d+);$', lines[i])
    if m:
        ind, sv = m.group(1), m.group(2)
        j = i + 1
        while not re.match(r'^ +n->limitOption = \$(\d+)->limitOption;$', lines[j]):
            assert not lines[j].lstrip().startswith('//'), lines[j]
            j += 1
        lv = re.match(r'^ +n->limitOption = \$(\d+)->limitOption;$', lines[j]).group(1)
        assert lines[j + 1].strip() == '}', lines[j + 1]
        out.append('%sif ($%s != NIL || $%s != NULL)' % (ind, sv, lv))
        out.append('%s\tn->whereClause = makeLimitedModifyQual(n->relation, n->whereClause, $%s, $%s, @1);' % (ind, sv, lv))
        i = j + 2
        nlimit += 1
        continue
    out.append(lines[i])
    i += 1
if nlimit != 3:
    sys.exit("expected 3 DELETE/UPDATE limit blocks, got %d" % nlimit)
s = '\n'.join(out)

# value nodes are no longer "Value *"; add casts where rules return them as Node *
resub(r'\{ \$\$ = makeFloat\((\$\d)\); \}', r'{ $$ = (Node *) makeFloat(\1); }', 2)
rep('\t\t\t\t\t$$ = makeFloat($2);\n\t\t\t\t\tdoNegateFloat($$);',
    '\t\t\t\t\t$$ = (Node *) makeFloat($2);\n\t\t\t\t\tdoNegateFloat((Float *) $$);')
resub(r'\{ \$\$ = makeInteger\(\$1\); \}', r'{ $$ = (Node *) makeInteger($1); }')
rep('\t\t\t\t\t$$ = makeString(psprintf("%d", $1));', '\t\t\t\t\t$$ = (Node *) makeString(psprintf("%d", $1));')
resub(r'\{ \$\$ = makeString\(\$1\); \}', r'{ $$ = (Node *) makeString($1); }')
# PG17: statistics target is a Node (Integer) instead of int
rep('n->stxstattarget = $6;', 'n->stxstattarget = (Node *) makeInteger($6);')
rep('n->stxstattarget = $8;', 'n->stxstattarget = (Node *) makeInteger($8);')
# functions openHalo had in its modified core; provided by aux_mysql now
rep('static bool isIgnoreStmt = false;\n', '''static bool isIgnoreStmt = false;

/* provided by aux_mysql (mys_compat.c) */
extern bool isSystemVariable(char *varName);
extern Oid	getCurrentNamespaceOid(void);
''')

# MySQL variables in expressions: openHalo's analyzer turned these custom
# nodes into function calls (transformUserVarRef & co.).  PG18 has no hook
# for custom expression nodes, so emit the very same calls from the grammar.
rep('''            | MysqlUserVariableName
                {
                    UserVarRef *n = makeNode(UserVarRef);

                    n->userVarName = $1;
                    n->location = @1;

                    $$ = (Node *)n;
                }
            | MysqlUserVariableName COLON_EQUALS a_expr
                {
					UserVarAssign *n = makeNode(UserVarAssign);

                    n->userVarName = $1;
					n->expr = $3;
                    n->location = @1;

                    $$ = (Node *)n;
                }
            | MysSysVarName
                {
                    SysVarRef *n = makeNode(SysVarRef);
                    n->sysVarName = $1;
                    n->location = @1;
                    $$ = (Node *)n;
                }''', '''            | MysqlUserVariableName
                {
                    $$ = makeUserVarRefCall($1, @1);
                }
            | MysqlUserVariableName COLON_EQUALS a_expr
                {
                    $$ = makeUserVarAssignCall($1, $3, @1);
                }
            | MysSysVarName
                {
                    $$ = makeSysVarRefCall($1, @1);
                }''')
rep('static RawStmt *makeRawStmt(Node *stmt, int stmt_location);', '''static RawStmt *makeRawStmt(Node *stmt, int stmt_location);
static Node *makeUserVarRefCall(char *name, int location);
static Node *makeUserVarAssignCall(char *name, Node *expr, int location);
static Node *makeSysVarRefCall(char *name, int location);''')

# ---------------------------------------------------------------------------
# 3. helper functions appended to the epilogue
# ---------------------------------------------------------------------------
s = s.rstrip('\n') + '''

/*
 * Helpers added for the PostgreSQL 18 port (aux_mysql).
 */
static ReturningClause *
makeReturningClauseFromList(List *exprs)
{
	ReturningClause *rc;

	if (exprs == NIL)
		return NULL;
	rc = makeNode(ReturningClause);
	rc->options = NIL;
	rc->exprs = exprs;
	return rc;
}

static List *
makePublicationTables(List *rels)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, rels)
	{
		PublicationObjSpec *obj = makeNode(PublicationObjSpec);
		PublicationTable *pt = makeNode(PublicationTable);

		pt->relation = (RangeVar *) lfirst(lc);
		obj->pubobjtype = PUBLICATIONOBJ_TABLE;
		obj->pubtable = pt;
		obj->location = pt->relation->location;
		result = lappend(result, obj);
	}
	return result;
}

static PartitionStrategy
parsePartitionStrategy(char *strategy, int location, core_yyscan_t yyscanner)
{
	if (pg_strcasecmp(strategy, "list") == 0)
		return PARTITION_STRATEGY_LIST;
	else if (pg_strcasecmp(strategy, "range") == 0)
		return PARTITION_STRATEGY_RANGE;
	else if (pg_strcasecmp(strategy, "hash") == 0)
		return PARTITION_STRATEGY_HASH;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized partitioning strategy \\"%s\\"", strategy),
			 parser_errposition(location)));
	return PARTITION_STRATEGY_LIST;		/* keep compiler quiet */
}

/* @name  ==>  mysql.get_user_var('name')   (openHalo transformUserVarRef) */
static Node *
makeUserVarRefCall(char *name, int location)
{
	return (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("get_user_var")),
								 list_make1(makeStringConst(name, location)),
								 COERCE_EXPLICIT_CALL, location);
}

/* @name := expr  ==>  mysql.set_user_var_in_expr('name', expr) */
static Node *
makeUserVarAssignCall(char *name, Node *expr, int location)
{
	return (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("set_user_var_in_expr")),
								 list_make2(makeStringConst(name, location), expr),
								 COERCE_EXPLICIT_CALL, location);
}

/* @@[global.|session.|local.]name  ==>  mysql.get_system_{global|session}_variable('name') */
static Node *
makeSysVarRefCall(char *name, int location)
{
	const char *func = "get_system_session_variable";

	if (pg_strncasecmp(name, "global.", 7) == 0)
	{
		func = "get_system_global_variable";
		name += 7;
	}
	else if (pg_strncasecmp(name, "session.", 8) == 0)
		name += 8;
	else if (pg_strncasecmp(name, "local.", 6) == 0)
		name += 6;

	return (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString(pstrdup(func))),
								 list_make1(makeStringConst(name, location)),
								 COERCE_EXPLICIT_CALL, location);
}

/*
 * DELETE/UPDATE ... [ORDER BY ...] LIMIT n  ==>
 *   WHERE (tableoid, ctid) IN (SELECT tableoid, ctid FROM rel
 *                               WHERE <qual> ORDER BY ... LIMIT n)
 */
static Node *
makeLimitedModifyQual(RangeVar *relation, Node *whereClause,
					  List *sortClause, SelectLimit *limit, int location)
{
	SelectStmt *sub = makeNode(SelectStmt);
	SubLink    *link = makeNode(SubLink);
	RowExpr    *row = makeNode(RowExpr);
	ColumnRef  *c1 = makeNode(ColumnRef);
	ColumnRef  *c2 = makeNode(ColumnRef);
	ResTarget  *t1 = makeNode(ResTarget);
	ResTarget  *t2 = makeNode(ResTarget);

	c1->fields = list_make1(makeString("tableoid"));
	c1->location = -1;
	c2->fields = list_make1(makeString("ctid"));
	c2->location = -1;
	t1->val = (Node *) copyObject(c1);
	t1->location = -1;
	t2->val = (Node *) copyObject(c2);
	t2->location = -1;

	sub->targetList = list_make2(t1, t2);
	sub->fromClause = list_make1(copyObject(relation));
	sub->whereClause = whereClause;
	sub->sortClause = sortClause;
	if (limit)
	{
		sub->limitOffset = limit->limitOffset;
		sub->limitCount = limit->limitCount;
		sub->limitOption = limit->limitOption;
	}

	row->args = list_make2(c1, c2);
	row->row_format = COERCE_IMPLICIT_CAST;
	row->location = -1;

	link->subLinkType = ANY_SUBLINK;
	link->testexpr = (Node *) row;
	link->operName = NIL;
	link->subselect = (Node *) sub;
	link->location = location;
	return (Node *) link;
}
'''

# --- aux_mysql semantics -------------------------------------------------
# MySQL DIV is integer division (NULL for 0); openHalo mapped it to "/",
# which aux_mysql makes return DECIMAL like MySQL's "/".
rep('''			| a_expr DIV a_expr
				{ $$ = (Node *) makeSimpleA_Expr(AEXPR_OP, "/", $1, $3, @2); }''',
    '''			| a_expr DIV a_expr
				{
					$$ = (Node *) makeFuncCall(list_make2(makeString("mysql"), makeString("int_div")),
											   list_make2($1, $3), COERCE_EXPLICIT_CALL, @2);
				}''')

# REGEXP / RLIKE: PostgreSQL has no regular expressions for nondeterministic
# (case-insensitive) collations; MySQL's REGEXP is case-insensitive anyway
# (~*), so match the subject under the deterministic default collation.
for op, a, b in (('REGEXP BINARY_LA', '"~"', '$4'), ('REGEXP', '"~*"', '$3'),
                 ('RLIKE', '"~*"', '$3'), ('NOT REGEXP BINARY_LA', '"!~"', '$5'),
                 ('NOT REGEXP', '"!~*"', '$4'), ('NOT RLIKE', '"!~*"', '$4')):
    rep('''			| a_expr %s a_expr%s%%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, list_make1(makeString(%s)), $1, %s, @2); }'''
        % (op, '       ' if op in ('REGEXP BINARY_LA', 'RLIKE', 'NOT REGEXP BINARY_LA', 'NOT RLIKE') else '               ', a, b),
        '''			| a_expr %s a_expr%s%%prec Op
				{ $$ = (Node *) makeA_Expr(AEXPR_OP, list_make1(makeString(%s)), makeRegexSubject($1), %s, @2); }'''
        % (op, '       ' if op in ('REGEXP BINARY_LA', 'RLIKE', 'NOT REGEXP BINARY_LA', 'NOT RLIKE') else '               ', a, b))

# ENGINE= / ENCRYPTION= table options: keep them (openHalo dropped them) so
# that aux_mysql can map them to table access methods (orioledb, tde_heap).
rep('''                | ENCRYPTION opt_equal Sconst
                    {
                        $$ = NULL;
                    }
                | ENGINE opt_equal IDENT
                    {
                        $$ = NULL;
                    }''', '''                | ENCRYPTION opt_equal Sconst
                    {
                        $$ = makeDefElem("encryption", (Node *)makeString($3), @1);
                    }
                | ENGINE opt_equal IDENT
                    {
                        $$ = makeDefElem("engine", (Node *)makeString($3), @1);
                    }''', 2)

rep('''static Node *
makeByteaStringConst(char *str, int location)''', '''static Node *
makeRegexSubject(Node *arg)
{
	CollateClause *c = makeNode(CollateClause);

	c->arg = arg;
	c->collname = list_make2(makeString("pg_catalog"), makeString("default"));
	c->location = -1;
	return (Node *) c;
}

static Node *
makeByteaStringConst(char *str, int location)''')
rep('''static Node *makeByteaStringConst(char *str, int location);''',
    '''static Node *makeByteaStringConst(char *str, int location);
static Node *makeRegexSubject(Node *arg);''')

# openHalo 14.18 moved the column COLLATE mapping out of the grammar (into its
# forked parse_utilcmd; aux_mysql does it in mys_rewrite.c) and commented the
# old code out, leaving typeName computed but unused.
rep('''					//n->collClause = collateClause;
''', '''					//n->collClause = collateClause;
					(void) typeName;	/* only read by the code commented out above */
''')

# The IF EXISTS spelling has two more tokens than the ordinary RENAME.
start = s.index('| ALTER TABLE IF_P EXISTS relation_expr RENAME TO relation_expr')
end = s.index('| ALTER SEQUENCE qualified_name RENAME TO name', start)
f = s[start:end]
assert f.count('n->object = (Node *)$6;') == 2
s = s[:start] + f.replace('n->object = (Node *)$6;', 'n->object = (Node *)$8;') + s[end:]

# FLOAT without a precision is single precision; DOUBLE remains float8.
start = s.index('opt_float:')
end = s.index('\n\t\t;', start)
f = s[start:end]
assert f.count('SystemTypeName("float8")') == 3
# Retain FLOAT(25..53), but fix the unqualified/deprecated (M,D) forms.
f = f.replace('$$ = SystemTypeName("float8");', '$$ = SystemTypeName("float4");')
f = f.replace('else if ($2 <= 53)\n\t\t\t\t\t\t$$ = SystemTypeName("float4");',
              'else if ($2 <= 53)\n\t\t\t\t\t\t$$ = SystemTypeName("float8");')
s = s[:start] + f + s[end:]

# DATETIME is a timestamp without time zone. A domain cannot carry a column's
# precision; the old grammar discarded (N), making all prepared columns claim
# six fractional digits. Keep the typmod on the native type instead.
start = s.index("| DATETIME '(' Iconst ')'")
end = s.index('| DATE\n', start)
s = s[:start] + '''| DATETIME '(' Iconst ')'
                {
                    $$ = SystemTypeName("timestamp");
                    $$->typmods = list_make1(makeIntConst($3, @3));
                    $$->location = @1;
                }
            | DATETIME
                {
                    $$ = SystemTypeName("timestamp");
                    $$->typmods = list_make1(makeIntConst(0, -1));
                    $$->location = @1;
                }
            ''' + s[end:]

# SHOW diagnostics reads the per-session diagnostic area through an SRF.
rep('VariableShowStmt:\n', '''VariableShowStmt:
            SHOW IDENT opt_select_limit
                {
                    SelectStmt *sel = makeNode(SelectStmt);
                    RangeFunction *rf = makeNode(RangeFunction);
                    ResTarget *target = makeNode(ResTarget);
                    ColumnRef *star = makeNode(ColumnRef);
                    FuncCall *call;

                    if (pg_strcasecmp($2, "warnings") != 0 && pg_strcasecmp($2, "errors") != 0)
                        ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                            errmsg("unsupported SHOW command: %s", $2), parser_errposition(@2)));
                    call = makeFuncCall(list_make2(makeString("mysql"), makeString("show_diagnostics")),
                        list_make1(makeBoolAConst(pg_strcasecmp($2, "errors") == 0, @2)),
                        COERCE_EXPLICIT_CALL, @1);
                    rf->functions = list_make1(list_make2(call, NIL));
                    sel->fromClause = list_make1(rf);
                    star->fields = list_make1(makeNode(A_Star));
                    star->location = -1;
                    target->val = (Node *) star;
                    target->location = -1;
                    sel->targetList = list_make1(target);
                    insertSelectOptions(sel, NIL, NIL, $3, NULL, yyscanner);
                    $$ = (Node *) sel;
                }
            |
''')

# ---------------------------------------------------------------------------
# START TRANSACTION WITH CONSISTENT SNAPSHOT
# ---------------------------------------------------------------------------
# mysqldump --single-transaction sends START TRANSACTION /*!40100 WITH
# CONSISTENT SNAPSHOT */, now that executable comments run; mydumper and
# CDC snapshots send it plainly.  InnoDB creates the read view at START:
# mys_utility.c runs it as REPEATABLE READ and takes the snapshot then.
# CONSISTENT is no keyword here.
rep('\t\t\t| READ ONLY\n'
    '\t\t\t\t\t{ $$ = makeDefElem("transaction_read_only",\n',
    '\t\t\t| WITH IDENT SNAPSHOT\n'
    '\t\t\t\t\t{\n'
    '\t\t\t\t\t\tif (pg_strcasecmp($2, "consistent") != 0)\n'
    '\t\t\t\t\t\t\tereport(ERROR,\n'
    '\t\t\t\t\t\t\t\t\t(errcode(ERRCODE_SYNTAX_ERROR),\n'
    '\t\t\t\t\t\t\t\t\t errmsg("syntax error at or near \\"%s\\"", $2),\n'
    '\t\t\t\t\t\t\t\t\t parser_errposition(@2)));\n'
    '\t\t\t\t\t\t$$ = makeDefElem("mysql_consistent_snapshot",\n'
    '\t\t\t\t\t\t\t\t\t\t makeIntConst(true, @1), @1);\n'
    '\t\t\t\t\t}\n'
    '\t\t\t| READ ONLY\n'
    '\t\t\t\t\t{ $$ = makeDefElem("transaction_read_only",\n')

# ---------------------------------------------------------------------------
# Multi-table UPDATE
# ---------------------------------------------------------------------------
# set_clause_list_for_mysql yields [qualifier, ResTarget] pairs (qualifier is
# NULL or a String A_Const).  The JOIN form passed that list to PG as the
# targetList, so the rewriter read an A_Const or NULL as a ResTarget and the
# backend crashed.  Keep only the ResTargets; PG updates just the left table,
# so a qualifier naming another table is refused rather than misapplied.
# Unqualified columns resolve to the left table, as PG does for UPDATE.
rep('                    n->targetList = $9;\n', (
    '\t\t\t\t\t{\n'
    '\t\t\t\t\t\tconst char *target = $3->alias ? $3->alias->aliasname : $3->relname;\n'
    '\t\t\t\t\t\tint\t\t\ti;\n'
    '\n'
    '\t\t\t\t\t\tn->targetList = NIL;\n'
    '\t\t\t\t\t\tfor (i = 0; i + 1 < list_length($9); i += 2)\n'
    '\t\t\t\t\t\t{\n'
    '\t\t\t\t\t\t\tNode\t   *qualifier = (Node *) list_nth($9, i);\n'
    '\t\t\t\t\t\t\tResTarget  *rt = list_nth_node(ResTarget, $9, i + 1);\n'
    '\n'
    '\t\t\t\t\t\t\tif (qualifier != NULL &&\n'
    '\t\t\t\t\t\t\t\t(!IsA(qualifier, A_Const) ||\n'
    '\t\t\t\t\t\t\t\t !IsA(&((A_Const *) qualifier)->val, String) ||\n'
    '\t\t\t\t\t\t\t\t pg_strcasecmp(strVal(&((A_Const *) qualifier)->val), target) != 0))\n'
    '\t\t\t\t\t\t\t\tereport(ERROR,\n'
    '\t\t\t\t\t\t\t\t\t\t(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),\n'
    '\t\t\t\t\t\t\t\t\t\t errmsg("UPDATE ... JOIN can only change columns of \\"%s\\"", target),\n'
    '\t\t\t\t\t\t\t\t\t\t parser_errposition(rt->location)));\n'
    '\t\t\t\t\t\t\tn->targetList = lappend(n->targetList, rt);\n'
    '\t\t\t\t\t\t}\n'
    '\t\t\t\t\t}\n'))
# The comma form compared only a prefix of the target name, so aliases "o"
# and "oi" both matched "o".  Compare whole names (case-insensitively, as
# with lower_case_table_names=1).
rep('''                        int len;
                        ListCell* lc;
                        char* targetRelationName;
''', '''                        ListCell* lc;
                        char* targetRelationName;
''')
rep('\n                        len = 0;\n', '\n')
rep('\n' + ' ' * 40 + 'len = strlen(targetRelationName);\n', '\n')
rep('\n' + ' ' * 40 + 'if (strncasecmp(targetRelationName, rel->val.sval.sval, len) != 0)',
    '\n' + '\t' * 10 + 'if (pg_strcasecmp(targetRelationName, rel->val.sval.sval) != 0)')
rep(' ' * 32 + 'if (strncasecmp(targetRelationName, rangeVar->alias->aliasname, len) == 0)',
    '\t' * 8 + 'if (pg_strcasecmp(targetRelationName, rangeVar->alias->aliasname) == 0)')
rep(' ' * 32 + 'if ((strncasecmp(targetRelationName, rangeVar->relname, len)) == 0)',
    '\t' * 8 + 'if (pg_strcasecmp(targetRelationName, rangeVar->relname) == 0)')

open(dst, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print("ported grammar written to", dst)
