#!/usr/bin/env python3
"""
Port openHalo's PG14-based MySQL grammar (mys_gram.y) to PostgreSQL 18 parse
node structures.

    port_grammar.py <openhalo mys_gram.y> <output mys_gram.y>

Every transformation is an exact-text (or tightly anchored) replacement that
asserts its expected match count, so upstream changes fail loudly instead of
silently producing a broken grammar.
"""
import difflib
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8', errors='surrogateescape').read()
upstream_lines = s.splitlines(keepends=True)


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
\t *	 _binary X'ABCD'  -> "xABCD" (odd digits: a leading 0, as in MySQL)
	 *	 _binary '...'    -> 16 byte NUL padded decimal length + raw bytes
	 *						 (escapes already processed, may contain NULs)
	 */
\tint			len;
\tchar	   *hex;

	if (str[0] == 'x' || str[0] == 'X')
\t\treturn makeStringConstCast(psprintf("\\\\x%s%s", strlen(str + 1) % 2 ? "0" : "", str + 1),
\t\t\t\t\t\t\t\t   location, SystemTypeName("bytea"));

\tlen = atoi(str);
\thex = palloc((Size) len * 2 + 3);
\thex[0] = '\\\\';
\thex[1] = 'x';
\thex[hex_encode(str + 16, len, hex + 2) + 2] = '\\0';
\treturn makeStringConstCast(hex, location, SystemTypeName("bytea"));''')
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

# Hexadecimal (X'41', 0x41) and bit-value (b'101', 0b101) literals are MySQL
# binary strings that are unsigned integers in numeric context: constants of
# type mysql.hexlit (aux_mysql 1.8, src/mys_binary.c); _binary b'..' is a
# plain bytea.  A session whose aux_mysql has no mysql.hexlit yet (binaries
# newer than the installed extension) keeps openHalo's bit strings.
rep('''			| BCONST
\t\t\t\t{
\t\t\t\t\t$$ = makeBitStringConst($1, @1);
\t\t\t\t}
\t\t\t| XCONST
\t\t\t\t{
\t\t\t\t\t/* This is a bit constant per SQL99:
\t\t\t\t\t * Without Feature F511, "BIT data type",
\t\t\t\t\t * a <general literal> shall not be a
\t\t\t\t\t * <bit string literal> or a <hex string literal>.
\t\t\t\t\t */
\t\t\t\t\t$$ = makeBitStringConst($1, @1);
\t\t\t\t}
            | UNDERSCORE_BINARY BCONST
                {
                    $$ = makeBitStringConst($2, @2);
                }''', '''			| BCONST
\t\t\t\t{
\t\t\t\t\t$$ = makeHexLitConst($1, false, @1);
\t\t\t\t}
\t\t\t| XCONST
\t\t\t\t{
\t\t\t\t\t/* MySQL: a binary string, a number in numeric context */
\t\t\t\t\t$$ = makeHexLitConst($1, false, @1);
\t\t\t\t}
\t\t\t| UNDERSCORE_BINARY BCONST
\t\t\t\t{
\t\t\t\t\t$$ = makeHexLitConst($2, true, @2);
\t\t\t\t}''')
rep('''static Node *makeRegexSubject(Node *arg);''', '''static Node *makeRegexSubject(Node *arg);
static Node *makeHexLitConst(char *str, bool binary, int location);''')
rep('#include "utils/lsyscache.h"', '#include "utils/lsyscache.h"\n#include "mys_binary.h"')
# 1.9: preserve byte lengths after the SQL update, while binaries paired
# with older SQL keep the old domains. The helper refreshes only dirty caches.
for token, oldtype, newtype, n in (
        ('BINARY', 'binary', 'binary_n', '1'),
        ('BINARY', 'binary', 'binary_n', '$3'),
        ('VARBINARY', 'varbinary', 'varbinary_n', '$3')):
    production = token if n == '1' else token + " '(' Iconst ')'"
    pattern = (r'            \| ' + re.escape(production) + r' +\{\n'
               r'                //\$\$ = SystemTypeName\("bytea"\);\n'
               r'                \$\$ = makeTypeNameFromNameList\(list_make2\(makeString\("mysql"\),\n'
               r' +makeString\("' + oldtype + r'"\)\)\);\n'
               r'                //\$\$->typmods = list_make1\(makeIntConst\(' + re.escape(n) + r', -1\)\);\n'
               r'                \}')
    replacement = ('\t\t\t| ' + production + '\n\t\t\t\t{\n'
                   '\t\t\t\t\t$$ = mys_binary_type_name("' + newtype + '", ' + n + ', @1);\n'
                   '\t\t\t\t}')
    resub(pattern, replacement, expect=1)
# Legacy SHOW output omitted a VARBINARY length; keep accepting it as a
# legacy domain. Newly declared VARBINARY(n) uses the new type after upgrade.
rep('''            | YEAR_P opt_field_length opt_field_option''', '''\t\t\t| VARBINARY
\t\t\t\t{
\t\t\t\t\t$$ = makeTypeNameFromNameList(list_make2(makeString("mysql"), makeString("varbinary")));
\t\t\t\t}
            | YEAR_P opt_field_length opt_field_option''')

# CAST AS BINARY without a length does not pad; BINARY(n) does after 1.9.
rep('''            | BINARY
                {
\t\t\t\t\t$$ = SystemTypeName("bytea");
\t\t\t\t\t$$->location = @1;
                }''', '''            | BINARY
                {
                    $$ = mys_binary_types_available ? mys_binary_type_name("binary_n", -1, @1) : SystemTypeName("bytea");
                    $$->location = @1;
                }
            | BINARY '(' Iconst ')'
                {
                    $$ = mys_binary_types_available ? mys_binary_type_name("binary_n", $3, @1) : SystemTypeName("bytea");
                    $$->location = @1;
                }''')

# Preserve explicit CAST AS CHAR(n) lengths; the default CHAR(1) column
# typmod has location -1 and does not constrain a CAST AS CHAR.
rep('MySQLConvertType:\n            Character\n\t\t\t\t{\n\t\t\t\t\t$$ = SystemTypeName("varchar");\n\t\t\t\t\t$$->location = @1;\n\t\t\t\t}', 'MySQLConvertType:\n            Character\n                {\n                    $$ = SystemTypeName("varchar");\n                    if ($1->typmods && ((A_Const *) linitial($1->typmods))->location >= 0)\n                        $$->typmods = $1->typmods;\n                    $$->location = @1;\n                }')
rep('$$ = makeTypeCast($3, $5, @1);', '$$ = mys_mysql_cast($3, $5, @1);', 2)
rep('SystemFuncName("convert_to")', 'mys_binary_types_available ? list_make2(makeString("mysql"), makeString("convert_using")) : SystemFuncName("convert_to")', 2)
rep('SystemFuncName("position")', 'mys_binary_types_available ? list_make2(makeString("mysql"), makeString("instr")) : SystemFuncName("position")')

# Arithmetic must assign unknown strings their string type before the
# operator resolver's exact-(hexlit,hexlit) shortcut.
start = s.index('\na_expr:')
end = s.index('\nc_expr:', start)
block = s[start:end]
assert 'makeSimpleA_Expr(AEXPR_OP, "+", $1, $3, @2)' in block
block = block.replace('makeSimpleA_Expr(AEXPR_OP,', 'mys_hexlit_operator(AEXPR_OP,')
s = s[:start] + block + s[end:]

# TRIM dispatch must reach the overloads for binary strings.
# Existing native text overloads remain available through search_path.
for trim_name in ('btrim', 'ltrim', 'rtrim'):
    rep('SystemFuncName("' + trim_name + '")',
        'list_make1(makeString("' + trim_name + '"))',
        count=2 if trim_name == 'btrim' else 1)

# Only ordinary calls: aggregate syntax and generated SHOW code are untouched.
rep('''                    n->agg_order = $4;'''.replace('                    ', '\t\t\t\t\t') +
    '\n\t\t\t\t\t$$ = (Node *)n;',
    '\t\t\t\t\tn->agg_order = $4;\n\t\t\t\t\t$$ = mys_binary_func_call(n);')

rep('''static Node *
makeRegexSubject(Node *arg)
{''', '''/*
 * A hexadecimal (str "x41": X'41', 0x41) or bit-value ("b101": b'101',
 * 0b101) literal.  In MySQL it is a binary string that is an unsigned
 * integer in numeric context: '\\x<bytes>'::mysql.hexlit, whose operators
 * and casts give it both meanings, or with _binary a plain bytea.  The bits
 * are right-aligned in whole bytes, as in MySQL (b'101' = 0x05, 0xabc =
 * 0x0abc).  Without mysql.hexlit (binaries newer than the installed
 * aux_mysql) a literal remains a bit string.
 */
static Node *
makeHexLitConst(char *str, bool binary, int location)
{
\tstatic const char hexdigits[] = "0123456789abcdef";
\tconst char *digits = str + 1;
\tint			len = strlen(digits);
\tStringInfoData buf;

\tif (!binary && !mys_hexlit_available)
\t\treturn makeBitStringConst(str, location);

\tinitStringInfo(&buf);
\tappendStringInfoString(&buf, "\\\\x");
\tif (str[0] == 'x' || str[0] == 'X')
\t{
\t\tif (len % 2)
\t\t\tappendStringInfoChar(&buf, '0');
\t\tappendStringInfoString(&buf, digits);
\t}
\telse
\t{
\t\tint			nbytes = (len + 7) / 8;
\t\tint			lead = nbytes * 8 - len;

\t\tfor (int i = 0; i < nbytes; i++)
\t\t{
\t\t\tint			byte = 0;

\t\t\tfor (int j = 0; j < 8; j++)
\t\t\t{
\t\t\t\tint			k = i * 8 + j - lead;

\t\t\t\tbyte <<= 1;
\t\t\t\tif (k < 0)
\t\t\t\t\tcontinue;
\t\t\t\tif (digits[k] == '1')
\t\t\t\t\tbyte |= 1;
\t\t\t\telse if (digits[k] != '0')
\t\t\t\t\tereport(ERROR,
\t\t\t\t\t\t\t(errcode(ERRCODE_SYNTAX_ERROR),
\t\t\t\t\t\t\t errmsg("\\"%c\\" is not a valid binary digit", digits[k])));
\t\t\t}
\t\t\tappendStringInfoChar(&buf, hexdigits[byte >> 4]);
\t\t\tappendStringInfoChar(&buf, hexdigits[byte & 0xf]);
\t\t}
\t}
\treturn makeStringConstCast(buf.data, location,
\t\t\t\t\t\t\t   binary ? SystemTypeName("bytea") :
\t\t\t\t\t\t\t   makeTypeNameFromNameList(list_make2(makeString("mysql"),
\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t   makeString("hexlit"))));
}

static Node *
makeRegexSubject(Node *arg)
{''')

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
# SET TRANSACTION without a scope
# ---------------------------------------------------------------------------
# MySQL applies it to the next transaction only, SET SESSION TRANSACTION to
# the session.  Both called set_system_session_variable() with the same name;
# mark the one-shot form so the variable code can tell them apart.
start = s.index('            | SET TRANSACTION transaction_mode_list\n')
end = s.index('            | SET SESSION TRANSACTION transaction_mode_list\n', start)
fragment = s[start:end]
line = ' ' * 24 + 'argList = list_make2(makeStringConst(pstrdup(defElem->defname), -1),\n'
assert fragment.count(line) == 1
rep(fragment, fragment.replace(line, '\t' * 6 +
    'argList = list_make2(makeStringConst(psprintf("next_%s", defElem->defname), -1),\n'))

# ---------------------------------------------------------------------------
# Prefix key parts, col(n)
# ---------------------------------------------------------------------------
# The length was dropped, so a key on a TEXT column indexed whole values and
# an INSERT of a long value failed on btree's entry limit.  Keep it as an
# opclass option that mys_rewrite.c turns into the index definition (an
# unconsumed marker fails loudly: btree's opclasses take no options).
rep('\t\t\t| ColId \'(\' Iconst \')\' index_elem_options\n'
    '                {\n'
    '                    $$ = $5;\n'
    '\t\t\t\t\t$$->name = $1;\n'
    '                }',
    '\t\t\t| ColId \'(\' Iconst \')\' index_elem_options\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\t$$ = $5;\n'
    '\t\t\t\t\t$$->name = $1;\n'
    '\t\t\t\t\t$$->opclassopts = lappend($$->opclassopts,\n'
    '\t\t\t\t\t\t\t\t\t\t\t   makeDefElem("mysql_prefix", (Node *) makeInteger($3), @3));\n'
    '\t\t\t\t}')

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

# ---------------------------------------------------------------------------
# FOREIGN KEY
# ---------------------------------------------------------------------------
# PG18 added Constraint.is_enforced (NOT ENFORCED); gram.y sets it before
# processCASbits.  This PG14 grammar left it zero, so every MySQL foreign key
# was created NOT ENFORCED: no checks, no ON DELETE/UPDATE actions.
rep('\t\t\t\t\tn->conname = NULL;\n\t\t\t\t\tn->pktable\t\t\t= $8;\n',
    '\t\t\t\t\tn->conname = NULL;\n\t\t\t\t\tn->is_enforced = true;\n\t\t\t\t\tn->pktable\t\t\t= $8;\n')
# PG18's AT_AlterConstraint takes an ATAlterConstraint node, not the PG14
# Constraint built here (an assertion failure on cassert builds, a misread
# node otherwise).  ALTER CONSTRAINT is not MySQL syntax; MySQL rejects it too.
start = s.index('\t\t\t| ALTER CONSTRAINT name ConstraintAttributeSpec\n')
end = s.index('\t\t\t\t}\n', start) + len('\t\t\t\t}\n')
rep(s[start:end], (
    '\t\t\t| ALTER CONSTRAINT name ConstraintAttributeSpec\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\tereport(ERROR,\n'
    '\t\t\t\t\t\t\t(errcode(ERRCODE_SYNTAX_ERROR),\n'
    '\t\t\t\t\t\t\t errmsg("ALTER CONSTRAINT is not supported"),\n'
    '\t\t\t\t\t\t\t parser_errposition(@1)));\n'
    '\t\t\t\t\t$$ = NULL;\n'
    '\t\t\t\t}\n'))

# ---------------------------------------------------------------------------
# Generated columns: col type [GENERATED ALWAYS] AS (expr) [VIRTUAL | STORED]
# ---------------------------------------------------------------------------
# PG18 reads Constraint.generated_kind; PG14 had stored columns only, so the
# grammar never set it.  GENERATED ALWAYS AS became a DEFAULT (an error for an
# expression with a column reference), and the short form kept $5 (the
# VIRTUAL / STORED keyword, NULL) as its expression: a plain column, always
# NULL.  Keep the declared kind, VIRTUAL when omitted as in MySQL;
# mys_rewrite.c decides how PostgreSQL stores it.
rep('%type <node> stored_virtual\n', '%type <ival> stored_virtual\n')
rep('''stored_virtual:
            STORED          { $$ = NULL; }
            | VIRTUAL       { $$ = NULL; }
            | /* empty */   { $$ = NULL; }
''', '''stored_virtual:
\t\t\tSTORED\t\t\t{ $$ = ATTRIBUTE_GENERATED_STORED; }
\t\t\t| VIRTUAL\t\t{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
\t\t\t| /* empty */\t{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
''')
rep('''\t\t\t//| GENERATED generated_when AS '(' a_expr ')' STORED
\t\t\t| GENERATED generated_when AS '(' a_expr ')' stored_virtual
\t\t\t\t{
\t\t\t\t\tConstraint *n = makeNode(Constraint);
\t\t\t\t\tn->contype = CONSTR_GENERATED;
\t\t\t\t\tn->generated_when = $2;
\t\t\t\t\tn->raw_expr = $5;
\t\t\t\t\tn->cooked_expr = NULL;
''', '''\t\t\t| GENERATED generated_when AS '(' a_expr ')' stored_virtual
\t\t\t\t{
\t\t\t\t\tConstraint *n = makeNode(Constraint);
\t\t\t\t\tn->contype = CONSTR_GENERATED;
\t\t\t\t\tn->generated_when = $2;
\t\t\t\t\tn->raw_expr = $5;
\t\t\t\t\tn->cooked_expr = NULL;
\t\t\t\t\tn->generated_kind = $7;
''')
rep('''\t\t\t| AS '(' a_expr ')' stored_virtual
\t\t\t\t{
\t\t\t\t\tConstraint *n = makeNode(Constraint);
\t\t\t\t\tn->contype = CONSTR_GENERATED;
\t\t\t\t\tn->generated_when = ATTRIBUTE_IDENTITY_ALWAYS;
\t\t\t\t\tn->raw_expr = $5;
\t\t\t\t\tn->cooked_expr = NULL;
\t\t\t\t\tn->location = @1;

\t\t\t\t\t/*
\t\t\t\t\t * Can't do this in the grammar because of shift/reduce
\t\t\t\t\t * conflicts.  (IDENTITY allows both ALWAYS and BY
\t\t\t\t\t * DEFAULT, but generated columns only allow ALWAYS.)  We
\t\t\t\t\t * can also give a more useful error message and location.
\t\t\t\t\t */
                    // if ($2 != ATTRIBUTE_IDENTITY_ALWAYS)
                    //     ereport(ERROR,
                    //             (errcode(ERRCODE_SYNTAX_ERROR),
                    //              errmsg("for a generated column, GENERATED ALWAYS must be specified"),
                    //              parser_errposition(@2)));

\t\t\t\t\t$$ = (Node *)n;
''', '''\t\t\t| AS '(' a_expr ')' stored_virtual
\t\t\t\t{
\t\t\t\t\tConstraint *n = makeNode(Constraint);
\t\t\t\t\tn->contype = CONSTR_GENERATED;
\t\t\t\t\tn->generated_when = ATTRIBUTE_IDENTITY_ALWAYS;
\t\t\t\t\tn->raw_expr = $3;
\t\t\t\t\tn->cooked_expr = NULL;
\t\t\t\t\tn->generated_kind = $5;
\t\t\t\t\tn->location = @1;
\t\t\t\t\t$$ = (Node *)n;
''')

# ---------------------------------------------------------------------------
# DECIMAL
# ---------------------------------------------------------------------------
# MySQL's DECIMAL / DEC / NUMERIC column without a precision is DECIMAL(10,0);
# PostgreSQL's bare numeric is unbounded, so such a column kept 1.2345 where
# MySQL stores 1.  A precision of 0 means 10 (also in CAST, where PostgreSQL
# rejects numeric(0)).  CAST(x AS DECIMAL) without one stays unbounded: MySQL
# clamps an out-of-range value there with a warning, an error would be new.
rep('static Node *makeIntConst(int val, int location);\n',
    'static Node *makeIntConst(int val, int location);\n'
    'static TypeName *makeMysqlDecimalType(List *mods, int location, bool column);\n')
rep(' opt_type_modifiers opt_field_option\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\t$$ = SystemTypeName("numeric");\n'
    '\t\t\t\t\t$$->typmods = $2;\n'
    '\t\t\t\t\t$$->location = @1;\n',
    ' opt_type_modifiers opt_field_option\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\t$$ = makeMysqlDecimalType($2, @1, true);\n', 3)
rep('\t\t\t| DECIMAL_P opt_type_modifiers\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\t$$ = SystemTypeName("numeric");\n'
    '\t\t\t\t\t$$->typmods = $2;\n'
    '\t\t\t\t\t$$->location = @1;\n',
    '\t\t\t| DECIMAL_P opt_type_modifiers\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\t$$ = makeMysqlDecimalType($2, @1, false);\n')
s = s.rstrip('\n') + '\n' + (
    '\n'
    '/*\n'
    ' * MySQL DECIMAL type: a column without a precision is DECIMAL(10,0), and\n'
    ' * a precision of 0 means 10.\n'
    ' */\n'
    'static TypeName *\n'
    'makeMysqlDecimalType(List *mods, int location, bool column)\n'
    '{\n'
    '\tTypeName   *result = SystemTypeName("numeric");\n'
    '\n'
    '\tif (mods == NIL && column)\n'
    '\t\tmods = list_make1(makeIntConst(10, location));\n'
    '\telse if (mods != NIL && IsA(linitial(mods), A_Const) &&\n'
    '\t\t\t IsA(&((A_Const *) linitial(mods))->val, Integer) &&\n'
    '\t\t\t intVal(&((A_Const *) linitial(mods))->val) == 0)\n'
    '\t{\n'
    '\t\tmods = list_copy(mods);\n'
    '\t\tlinitial(mods) = makeIntConst(10, location);\n'
    '\t}\n'
    '\tresult->typmods = mods;\n'
    '\tresult->location = location;\n'
    '\treturn result;\n'
    '}\n')

# ---------------------------------------------------------------------------
# DO expr [, expr] ...
# ---------------------------------------------------------------------------
# MySQL evaluates the expressions and discards the values (DO SLEEP(n),
# DO GET_LOCK(...), DO @v := ...).  Only PostgreSQL's DO block was parsed, so
# these were syntax errors.  mys_rewrite.c runs a MysDoStmt as a SELECT that
# answers OK.  A bare string is an expression, as in MySQL; a PostgreSQL DO
# block needs its LANGUAGE clause.
rep('\t\t\t\trelation_expr_list dostmt_opt_list\n', '\t\t\t\trelation_expr_list\n')
rep('%type <defelt>\tcreatefunc_opt_item common_func_opt_item dostmt_opt_item\n',
    '%type <defelt>\tcreatefunc_opt_item common_func_opt_item\n')
rep(' *\t\tDO <anonymous code block> [ LANGUAGE language ]\n'
    ' *\n'
    ' * We use a DefElem list for future extensibility, and to allow flexibility\n'
    ' * in the clause order.\n',
    ' *\t\tDO expr [, expr] ...\t\t\t\t(MySQL: the values are discarded)\n'
    ' *\t\tDO <anonymous code block> LANGUAGE language\n'
    ' *\n'
    ' * A bare string is a MySQL expression; a PostgreSQL DO block needs LANGUAGE.\n')
start = s.index('DoStmt: DO dostmt_opt_list\n')
end = s.index('/*****', start)
assert s[start:end].count('dostmt_opt_item:\n') == 1, s[start:end]
rep(s[start:end], (
    'DoStmt: DO expr_list\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\tMysDoStmt *n = makeNode(MysDoStmt);\n'
    '\n'
    '\t\t\t\t\tn->exprs = $2;\n'
    '\t\t\t\t\t$$ = (Node *) n;\n'
    '\t\t\t\t}\n'
    '\t\t\t| DO Sconst LANGUAGE NonReservedWord_or_Sconst\n'
    '\t\t\t\t{\n'
    '\t\t\t\t\tDoStmt *n = makeNode(DoStmt);\n'
    '\n'
    '\t\t\t\t\tn->args = list_make2(makeDefElem("as", (Node *) makeString($2), @2),\n'
    '\t\t\t\t\t\t\t\t\t\t makeDefElem("language", (Node *) makeString($4), @3));\n'
    '\t\t\t\t\t$$ = (Node *) n;\n'
    '\t\t\t\t}\n'
    '\t\t;\n'
    '\n'))

# DROP INDEX k ON t
# ---------------------------------------------------------------------------
# openHalo prepended the table to the index name, so PostgreSQL took the
# table for a schema ("schema t does not exist").  MySQL key names are per
# table: run it as ALTER TABLE t DROP INDEX k, which finds the key of that
# table (mys_utility.c), PRIMARY included.  ON is only valid with INDEX.
for n in (2, 3):
    first = n + 1
    old = ('\t\t\t\t\tDropStmt *n = makeNode(DropStmt);\n'
           '\t\t\t\t\tn->removeType = $%d;\n'
           '\t\t\t\t\tn->missing_ok = false;\n'
           '                    n->objects = $%d;\n'
           '                    if ($%d != NULL)\n'
           '                    {\n'
           '                        ListCell   *lc;\n'
           '                        foreach(lc, $%d)\n'
           '                        {\n'
           '                            List *names = (List *)lfirst(lc);\n'
           '                            if (list_length(names) == 1)\n'
           '                            {\n'
           '                                names = lcons(makeString(pstrdup($%d->relname)), names);\n'
           '                            }\n'
           '                            else\n'
           '                            {\n'
           '                                names = list_insert_nth(names, 1, makeStringConst(pstrdup($%d->relname), -1));\n'
           '                            }\n'
           '\n'
           '                            break;\n'
           '                        }\n'
           '                    }\n'
           '\t\t\t\t\tn->behavior = $%d;\n'
           '\t\t\t\t\tn->concurrent = false;\n'
           '\t\t\t\t\t$$ = (Node *)n;\n') % (n, first, n + 3, first, n + 3, n + 3, n + 2)
    new = ('\t\t\t\t\tif ($%d != NULL)\n'
           '\t\t\t\t\t\t$$ = makeMysqlDropIndex($%d, $%d, $%d, @%d, yyscanner);\n'
           '\t\t\t\t\telse\n'
           '\t\t\t\t\t{\n'
           '\t\t\t\t\t\tDropStmt *n = makeNode(DropStmt);\n'
           '\t\t\t\t\t\tn->removeType = $%d;\n'
           '\t\t\t\t\t\tn->missing_ok = false;\n'
           '\t\t\t\t\t\tn->objects = $%d;\n'
           '\t\t\t\t\t\tn->behavior = $%d;\n'
           '\t\t\t\t\t\tn->concurrent = false;\n'
           '\t\t\t\t\t\t$$ = (Node *)n;\n'
           '\t\t\t\t\t}\n') % (n + 3, n, first, n + 3, n, n, first, n + 2)
    rep(old, new)
rep('static TypeName *makeMysqlDecimalType(List *mods, int location, bool column);\n',
    'static TypeName *makeMysqlDecimalType(List *mods, int location, bool column);\n'
    'static Node *makeMysqlDropIndex(ObjectType type, List *names, RangeVar *table,\n'
    '\t\t\t\t\t\t\t\tint location, core_yyscan_t yyscanner);\n')
s = s.rstrip('\n') + '\n' + (
    '\n'
    '/* DROP INDEX k, ... ON t: ALTER TABLE t DROP INDEX k, ... */\n'
    'static Node *\n'
    'makeMysqlDropIndex(ObjectType type, List *names, RangeVar *table, int location,\n'
    '\t\t\t\t   core_yyscan_t yyscanner)\n'
    '{\n'
    '\tAlterTableStmt *stmt = makeNode(AlterTableStmt);\n'
    '\tListCell   *lc;\n'
    '\n'
    '\tif (type != OBJECT_INDEX)\n'
    '\t\tereport(ERROR,\n'
    '\t\t\t\t(errcode(ERRCODE_SYNTAX_ERROR),\n'
    '\t\t\t\t errmsg("syntax error at or near \\"ON\\""),\n'
    '\t\t\t\t parser_errposition(location)));\n'
    '\tstmt->relation = table;\n'
    '\tstmt->objtype = OBJECT_TABLE;\n'
    '\tforeach(lc, names)\n'
    '\t{\n'
    '\t\tAlterTableCmd *cmd = makeNode(AlterTableCmd);\n'
    '\n'
    '\t\tcmd->subtype = AT_DropIndex;\n'
    '\t\tcmd->name = strVal(llast((List *) lfirst(lc)));\n'
    '\t\tcmd->behavior = DROP_RESTRICT;\n'
    '\t\tstmt->cmds = lappend(stmt->cmds, cmd);\n'
    '\t}\n'
    '\treturn (Node *) stmt;\n'
    '}\n')

# INSERT IGNORE has per-row foreign-key failures, unlike DO NOTHING.
rep('onconflict->action = ONCONFLICT_NOTHING;',
    'onconflict->action = ONCONFLICT_NOTHING;\n'
    '\t\t\t\t\tonconflict->allIndexes = true;\n'
    '\t\t\t\t\tonconflict->ignoreFailure = true;', 4)

# IGNORE also applies to ODKU. Keep the update clause instead of rejecting
# this combination before its value-conversion safeguards can run.
for tail, conflict_arg in [('INTO insert_target insert_rest', 7),
                           ('insert_target insert_rest', 6),
                           ('INTO insert_target SET set_clause_list_for_mysql', 8),
                           ('insert_target SET set_clause_list_for_mysql', 7)]:
    rule = 'INSERT insert_low_priority_delayed IGNORE ' + tail
    rep(rule + '\n', rule + ' opt_on_conflict\n')
    begin = s.index('onconflict->action = ONCONFLICT_NOTHING;', s.index(rule))
    end = s.index('onconflict->location = @1;', begin) + len('onconflict->location = @1;')
    init = s[begin:end].replace('\n\t\t\t\t\tonconflict->ignoreFailure = true;', '')
    s = (s[:begin] + 'if ($%d) *onconflict = *$%d;\n' % (conflict_arg, conflict_arg) +
         '\t\t\t\t\telse {\n\t\t\t\t\t\t' + init + '\n\t\t\t\t\t}\n' +
         '\t\t\t\t\tonconflict->ignoreFailure = true;' + s[end:])

# DATABASE is a schema in MySQL mode. Accept the same charset/collation
# options already accepted by CREATE SCHEMA, including mysqldump output.
rep('createdb_opt_item:\n\t\t\t',
    'createdb_opt_item:\n'
    '\t\t\tcharacter_set_collate\n'
    '\t\t\t\t{ $$ = makeDefElem("mysql_database_option", NULL, @1); }\n'
    '\t\t\t| ')

# The empty column list is MySQL's spelling of DEFAULT VALUES.
for token in ('VALUES', 'VALUE_P'):
    old = "            | " + token + " '(' ')'"
    new = ("\t\t\t| '(' ')' " + token + " '(' ')'\n"
           "\t\t\t\t{\n"
           "\t\t\t\t\t$$ = makeNode(InsertStmt);\n"
           "\t\t\t\t\t$$->selectStmt = NULL;\n"
           "\t\t\t\t}\n" + old)
    rep(old, new)

# MySQL index hints are accepted and ignored, as the original FORCE INDEX was.
# Keep hint lists nonempty at the alias boundary; the parenthesized index-name
# list can be empty. PRIMARY is a reserved word, but a valid index name here.
rep("force_index:\n        FORCE INDEX '(' ColId ')'\n        ;", '''force_index:
            mysql_index_hint_kind mysql_index_hint_key mysql_index_hint_scope '(' mysql_index_hint_names ')'
        ;
mysql_index_hint_kind: USE | IGNORE | FORCE ;
mysql_index_hint_key: INDEX | KEY ;
mysql_index_hint_scope:
            /*EMPTY*/ | FOR JOIN | FOR ORDER BY | FOR GROUP_P BY
        ;
mysql_index_hint_names:
            /*EMPTY*/ | mysql_index_hint_name_list
        ;
mysql_index_hint_name_list:
            mysql_index_hint_name | mysql_index_hint_name_list ',' mysql_index_hint_name
        ;
mysql_index_hint_name: ColId | PRIMARY ;''')

# UPDATE/DELETE use a separate relation production from SELECT/JOIN. Share its
# alias handling with the UPDATE list rather than duplicating hint reductions.
begin = s.index('\nrelation_expr_opt_alias:')
end = s.index('\nrelation_expr_opt_aliass:', begin)
block = s[begin:end]
assert block.count('\n\t\t;') == 1
block = block.replace('\n\t\t;', '''
            | relation_expr opt_force_index_clause
                { $$ = $1; }
            | relation_expr ColId opt_force_index_clause
                { $1->alias = makeAlias($2, NIL); $$ = $1; }
            | relation_expr AS ColId opt_force_index_clause
                { $1->alias = makeAlias($3, NIL); $$ = $1; }
        ;''')
s = s[:begin] + block + s[end:]
begin = s.index('\nrelation_expr_opt_aliass:')
end = s.index('\n\t\t;', begin) + len('\n\t\t;')
s = s[:begin] + '''
relation_expr_opt_aliass:
            relation_expr_opt_alias { $$ = list_make1($1); }
        ;''' + s[end:]

# MySQL's DELETE FROM target USING table_references includes the target again
# in that list. PG needs a separate target alias and a physical-row identity
# join; otherwise its namespace rejects an otherwise valid hinted statement.
rep('static ReturningClause *makeReturningClauseFromList(List *exprs);',
    'static ReturningClause *makeReturningClauseFromList(List *exprs);\n'
    'static void makeMysqlDeleteUsing(DeleteStmt *stmt, int location);')
begin = s.index('\nDeleteStmt:')
end = s.index('\ndelete_options:',begin)
block = s[begin:end]
assert block.count('$$ = (Node *)n;') == 2
block = block.replace('$$ = (Node *)n;', 'makeMysqlDeleteUsing(n, @1);\n\t\t\t\t\t$$ = (Node *)n;')
s = s[:begin]+block+s[end:]
s += '''

static RangeVar *
mysqlDeleteSource(Node *node, const char *name, const char *schema)
{
    if (IsA(node, RangeVar))
    {
        RangeVar *r = (RangeVar *) node;
        if (strcmp(r->alias ? r->alias->aliasname : r->relname, name) == 0 &&
            (schema == NULL || (r->schemaname && strcmp(schema, r->schemaname) == 0)))
            return r;
    }
    else if (IsA(node, JoinExpr))
    {
        JoinExpr *j = (JoinExpr *) node;
        RangeVar *r = mysqlDeleteSource(j->larg, name, schema);
        return r ? r : mysqlDeleteSource(j->rarg, name, schema);
    }
    return NULL;
}

static void
makeMysqlDeleteUsing(DeleteStmt *stmt, int location)
{
    RangeVar *source = NULL;
    const char *name = stmt->relation->alias ? stmt->relation->alias->aliasname : stmt->relation->relname;
    char *target_alias;
    int suffix = 0;
    bool occupied;
    List *quals = stmt->whereClause ? list_make1(stmt->whereClause) : NIL;

    foreach_ptr(Node, item, stmt->usingClause)
    {
        source = mysqlDeleteSource(item, name, stmt->relation->schemaname);
        if (source) break;
    }
    if (!source) return;
    do
    {
        target_alias = psprintf("__aux_delete_%d", suffix++);
        occupied = false;
        foreach_ptr(Node, item, stmt->usingClause)
            occupied |= mysqlDeleteSource(item, target_alias, NULL) != NULL;
    } while (occupied);
    stmt->relation = copyObject(source);
    stmt->relation->alias = makeAlias(target_alias, NIL);
    for (int i = 0; i < 2; i++)
    {
        const char *column = i == 0 ? "ctid" : "tableoid";
        ColumnRef *left = makeNode(ColumnRef), *right = makeNode(ColumnRef);
        left->fields = list_make2(makeString(target_alias), makeString(pstrdup(column)));
        right->fields = list_make2(makeString(pstrdup(name)), makeString(pstrdup(column)));
        left->location = right->location = location;
        quals = lappend(quals, makeSimpleA_Expr(AEXPR_OP, "=", (Node *) left, (Node *) right, location));
    }
    stmt->whereClause = (Node *) makeBoolExpr(AND_EXPR, quals, location);
}
'''

# Connection initialization spellings used by JDBC, PDO and dump clients.
# Retain the one-argument SET NAMES call when no COLLATE is present so new
# binaries can still connect to old SQL catalogs before ALTER EXTENSION.
rep('%type <str>\t\tcharacter ncharacter charset_name opt_field_option',
    '%type <str>\t\tcharacter ncharacter charset_name opt_field_option\n'
    '%type <str>\t\tmysql_set_charset mysql_set_collation')
resub(r'\t+\| NAMES IDENT\n.*?(?=\non_a_expr:)', '''
            | NAMES mysql_set_charset mysql_set_collation
                {
                    ResTarget *result = makeNode(ResTarget);
                    List *args = list_make1(makeStringConst($2, @2));
                    if ($3)
                        args = lappend(args, makeStringConst($3, @3));
                    result->val = (Node *) makeFuncCall(
                        list_make2(makeString("mysql"), makeString("set_system_names_variable")),
                        args, COERCE_EXPLICIT_CALL, @1);
                    result->location = @1;
                    $$ = (Node *) result;
                }
            | character_set mysql_set_charset
                {
                    ResTarget *result = makeNode(ResTarget);
                    result->val = (Node *) makeFuncCall(
                        list_make2(makeString("mysql"), makeString("set_system_charset_variable")),
                        list_make1(makeStringConst($2, @2)), COERCE_EXPLICIT_CALL, @1);
                    result->location = @1;
                    $$ = (Node *) result;
                }
        ;

mysql_set_charset:
            charset_name        { $$ = $1; }
            | DEFAULT           { $$ = pstrdup("halo_mysql_system_var_default"); }
        ;

mysql_set_collation:
            COLLATE charset_name { $$ = $2; }
            | /* EMPTY */        { $$ = NULL; }
        ;
''', expect=1, flags=re.S)

# Keep untouched upstream formatting. Normalize only generated replacement
# lines to PostgreSQL's four-column tab style, including inherited indentation
# on rewritten expressions. This is deterministic from the PG14 input alone.
output_lines = s.splitlines(keepends=True)
for tag, _, _, start, end in difflib.SequenceMatcher(
        None, upstream_lines, output_lines, autojunk=False).get_opcodes():
    if tag == 'equal':
        continue
    for i in range(start, end):
        line = output_lines[i]
        indent = re.match(r'[ \t]*', line).group()
        width = len(indent.expandtabs(4))
        body = line[len(indent):].rstrip(' \t\n')
        output_lines[i] = '\t' * (width // 4) + ' ' * (width % 4) + body + ('\n' if line.endswith('\n') else '')
s = ''.join(output_lines)

open(dst, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print("ported grammar written to", dst)
