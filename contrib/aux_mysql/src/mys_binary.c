/*-------------------------------------------------------------------------
 *
 * mys_binary.c
 *	  MySQL binary strings that PostgreSQL has no type or function for
 *	  (SQL objects in sql/aux_mysql_binary.sql).
 *
 *	- Hexadecimal (X'41', 0x41) and bit-value (b'101', 0b101) literals:
 *	  in MySQL a binary string that is an unsigned 64-bit integer in
 *	  numeric context (0x41 + 0 = 65, CONCAT(0x41, 'b') = 'Ab').  The
 *	  grammar makes them constants of type mysql.hexlit: bytea's
 *	  representation, the bits right-aligned in whole bytes (b'101' =
 *	  0x05).  Operators with numbers use the integer value; casts store it
 *	  into numeric, string, BIT and binary columns.
 *	- HEX() and UNHEX()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/varbit.h"

#include "halo_mysql.h"
#include "mys_binary.h"
#include "mys_prefix.h"
#include "adapter/mysql/systemVar.h"
#include "catalog/pg_collation.h"
#include "common/hashfn.h"
#include "utils/formatting.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "parser/parse_oper.h"
#include "regex/regex.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"

extern uint64 mys_sqlMode;
bool mys_binary_types_available = false;
static bool binary_types_dirty = true;
static bool binary_types_callback_registered = false;
bool (*mys_binary_ignore_hook)(void) = NULL;

/*
 * Does this session's grammar make hexadecimal literals mysql.hexlit?  Only
 * once the extension has the type (aux_mysql 1.8): until ALTER EXTENSION
 * UPDATE, new binaries keep the old bit-string literals.
 */
bool		mys_hexlit_available = false;

static bool
is_mysql_type(Oid typid, const char *name)
{
	HeapTuple tup;
	bool result = false;

	if (typid < FirstNormalObjectId)
		return false;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_type t = (Form_pg_type) GETSTRUCT(tup);

		result = strcmp(NameStr(t->typname), name) == 0 &&
			t->typnamespace == get_namespace_oid("mysql", true);
		ReleaseSysCache(tup);
	}
	return result;
}

bool
mys_is_hexlit(Oid typid)
{
	return is_mysql_type(typid, "hexlit");
}

/* A callback only marks state. Catalog lookups must wait for a transaction. */
static void
binary_types_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	binary_types_dirty = true;
}

/* Called at handshake AND immediately before MySQL raw parsing, inside a
 * transaction. Unchanged catalogs cost a boolean check, not a catalog scan.
 * TYPEOID invalidation also covers renames, drops and rolled-back DDL. */
void
mys_binary_session_init(void)
{
	Oid nsp;

	if (!binary_types_callback_registered)
	{
		CacheRegisterSyscacheCallback(TYPEOID, binary_types_invalidate, (Datum) 0);
		CacheRegisterSyscacheCallback(NAMESPACEOID, binary_types_invalidate, (Datum) 0);
		binary_types_callback_registered = true;
	}
	if (!binary_types_dirty)
		return;
	nsp = get_namespace_oid("mysql", true);
	mys_hexlit_available = OidIsValid(nsp) &&
		OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								 CStringGetDatum("hexlit"), ObjectIdGetDatum(nsp)));
	mys_binary_types_available = OidIsValid(nsp) &&
		OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								 CStringGetDatum("binary_n"), ObjectIdGetDatum(nsp)));
	binary_types_dirty = false;
}

/*
 * getBaseType(), except that mysql.hexlit counts as bytea: code that sends
 * or copies the bytes of a binary string handles both alike.
 */
Oid
mys_binary_base_type(Oid typid)
{
	Oid			base = getBaseType(typid);

	return (mys_is_hexlit(base) || is_mysql_type(base, "binary_n") ||
			is_mysql_type(base, "varbinary_n"))
		? BYTEAOID : base;
}

/* ----------------------------------------------------------------
 * Numeric value of a literal
 * ----------------------------------------------------------------
 */

/*
 * The bytes as a big-endian unsigned integer.  More than 8 significant
 * bytes saturate to 2^64 - 1, as in MySQL (0x0100000000000000ff + 0).
 */
static uint64
bytes_uint64(const unsigned char *p, int len)
{
	uint64		v = 0;

	while (len > 0 && *p == 0)
		p++, len--;
	if (len > 8)
		return PG_UINT64_MAX;
	for (int i = 0; i < len; i++)
		v = (v << 8) | p[i];
	return v;
}

/* ----------------------------------------------------------------
 * Literals in DDL (raw parse trees, mys_rewrite.c)
 * ----------------------------------------------------------------
 */

/* the grammar's '\x..'::mysql.hexlit, or NULL */
static A_Const *
hexlit_literal(Node *n)
{
	TypeCast   *tc;
	A_Const    *c;
	List	   *names;

	if (n == NULL || !IsA(n, TypeCast))
		return NULL;
	tc = (TypeCast *) n;
	names = tc->typeName->names;
	if (tc->arg == NULL || !IsA(tc->arg, A_Const) || list_length(names) != 2 ||
		strcmp(strVal(linitial(names)), "mysql") != 0 ||
		strcmp(strVal(lsecond(names)), "hexlit") != 0)
		return NULL;
	c = (A_Const *) tc->arg;
	if (c->isnull || !IsA(&c->val, String))
		return NULL;
	return c;
}

static Node *
binary_make_const(NodeTag tag, char *str, int ival)
{
	A_Const    *c = makeNode(A_Const);

	if (tag == T_Integer)
	{
		c->val.ival.type = T_Integer;
		c->val.ival.ival = ival;
	}
	else if (tag == T_Float)
	{
		c->val.fval.type = T_Float;
		c->val.fval.fval = str;
	}
	else
	{
		c->val.sval.type = T_String;
		c->val.sval.sval = str;
	}
	c->location = -1;
	return (Node *) c;
}

/*
 * A hexadecimal literal as the DEFAULT of a column of type typid: the plain
 * literal of its value for that type (INT DEFAULT 0x10: 16, BIT DEFAULT
 * b'1': 1, VARCHAR DEFAULT 0x41: 'A', BINARY DEFAULT X'00': a bytea), so
 * that SHOW CREATE TABLE and information_schema show the value, as MySQL
 * does.  Any other DEFAULT is returned as it is.
 */
Node *
mys_fold_hexlit_default(Node *expr, Oid typid)
{
	A_Const    *c = hexlit_literal(expr);
	bytea	   *b;
	int			len;

	if (OidIsValid(typid) && mys_binary_base_type(typid) == BYTEAOID &&
		getBaseType(typid) != BYTEAOID && !mys_is_hexlit(typid))
	{
		if (expr && IsA(expr, A_Const))
		{
			A_Const *raw = (A_Const *) expr;

			if (!raw->isnull && IsA(&raw->val, String))
			{
				TypeCast *tc = makeNode(TypeCast);

				tc->arg = expr;
				tc->typeName = SystemTypeName("text");
				tc->location = raw->location;
				return (Node *) tc;
			}
		}
		return expr;
	}
	if (c == NULL || !OidIsValid(typid))
		return expr;
	b = DatumGetByteaPP(DirectFunctionCall1(byteain, CStringGetDatum(strVal(&c->val))));
	len = VARSIZE_ANY_EXHDR(b);
	switch (getBaseType(typid))
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case NUMERICOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case BITOID:
		case VARBITOID:
			{
				uint64		v = bytes_uint64((const unsigned char *) VARDATA_ANY(b), len);

				if (v <= (uint64) PG_INT32_MAX)
					return binary_make_const(T_Integer, NULL, (int) v);
				return binary_make_const(T_Float, psprintf("%llu", (unsigned long long) v), 0);
			}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			{
				char	   *s = palloc(len + 1);

				memcpy(s, VARDATA_ANY(b), len);
				s[len] = '\0';
				pg_verifymbstr(s, len, false);
				return binary_make_const(T_String, s, 0);
			}
		case BYTEAOID:
			{
				TypeCast   *tc = copyObject((TypeCast *) expr);

				tc->typeName = SystemTypeName("bytea");
				return (Node *) tc;
			}
		default:
			return expr;
	}
}

/*
 * CREATE TABLE ... AS / CREATE VIEW: a column that is just a hexadecimal
 * literal is a binary string (MySQL: VARBINARY), not of type mysql.hexlit.
 */
void
mys_hexlit_columns_binary(Node *query)
{
	SelectStmt *sel = (SelectStmt *) query;

	if (sel == NULL || !IsA(sel, SelectStmt))
		return;
	if (sel->op != SETOP_NONE)
	{
		mys_hexlit_columns_binary((Node *) sel->larg);
		mys_hexlit_columns_binary((Node *) sel->rarg);
		return;
	}
	foreach_node(ResTarget, rt, sel->targetList)
	{
		if (hexlit_literal(rt->val))
			((TypeCast *) rt->val)->typeName = SystemTypeName("bytea");
	}
}

static Datum
uint64_numeric(uint64 v)
{
	char		buf[32];

	if (v <= (uint64) PG_INT64_MAX)
		return NumericGetDatum(int64_to_numeric((int64) v));
	snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v);
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static uint64
hexlit_uint64(FunctionCallInfo fcinfo, int arg)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(arg);

	return bytes_uint64((const unsigned char *) VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
}

static void
out_of_range(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("BIGINT UNSIGNED value is out of range"),
			 errdetail("aux_mysql computes with signed 64-bit integers (at most 9223372036854775807).")));
}

PG_FUNCTION_INFO_V1(mys_hexlit_numeric);
Datum
mys_hexlit_numeric(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(uint64_numeric(hexlit_uint64(fcinfo, 0)));
}

PG_FUNCTION_INFO_V1(mys_hexlit_int8);
Datum
mys_hexlit_int8(PG_FUNCTION_ARGS)
{
	uint64		v = hexlit_uint64(fcinfo, 0);

	if (v > (uint64) PG_INT64_MAX)
		out_of_range();
	PG_RETURN_INT64((int64) v);
}

PG_FUNCTION_INFO_V1(mys_hexlit_bool);
Datum
mys_hexlit_bool(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	const char *p = VARDATA_ANY(b);
	int			len = VARSIZE_ANY_EXHDR(b);

	for (int i = 0; i < len; i++)
	{
		if (p[i] != 0)
			PG_RETURN_BOOL(true);
	}
	PG_RETURN_BOOL(false);
}

/* the bytes as a string (TEXT, VARCHAR, CHAR): they must be valid text */
PG_FUNCTION_INFO_V1(mys_hexlit_text);
Datum
mys_hexlit_text(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_P_COPY(0);

	pg_verifymbstr(VARDATA(b), VARSIZE(b) - VARHDRSZ, false);
	PG_RETURN_TEXT_P((text *) b);
}

/*
 * The value as BIT(n) (typmod n): the low n bits; a value of more than n
 * significant bits does not fit (MySQL: 1406 in strict mode).  Without a
 * typmod, all bits of the bytes.
 */
PG_FUNCTION_INFO_V1(mys_hexlit_varbit);
Datum
mys_hexlit_varbit(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	int32		typmod = PG_GETARG_INT32(1);
	const unsigned char *p = (const unsigned char *) VARDATA_ANY(b);
	int			len = VARSIZE_ANY_EXHDR(b);
	int			nbits = len * BITS_PER_BYTE;
	int			sig = nbits;
	int			bitlen;
	VarBit	   *result;
	bits8	   *r;

	for (int i = 0; i < len; i++)
	{
		if (p[i] != 0)
		{
			for (int m = 0x80; m && !(p[i] & m); m >>= 1)
				sig--;
			break;
		}
		sig -= BITS_PER_BYTE;
	}
	if (typmod <= 0)
		bitlen = nbits;
	else
	{
		if (sig > typmod)
			ereport(ERROR,
					(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
					 errmsg("bit string length %d does not match type bit varying(%d)",
							sig, typmod)));
		bitlen = typmod;
	}
	result = (VarBit *) palloc0(VARBITTOTALLEN(bitlen));
	SET_VARSIZE(result, VARBITTOTALLEN(bitlen));
	VARBITLEN(result) = bitlen;
	r = VARBITS(result);
	for (int i = 0; i < bitlen; i++)
	{
		int			src = nbits - bitlen + i;	/* bit of the bytes, MSB first */

		if (src >= 0 && (p[src / BITS_PER_BYTE] >> (7 - src % BITS_PER_BYTE)) & 1)
			r[i / BITS_PER_BYTE] |= 0x80 >> (i % BITS_PER_BYTE);
	}
	PG_RETURN_VARBIT_P(result);
}

/* unsigned value of a BIT column (at most 64 bits in MySQL) */
PG_FUNCTION_INFO_V1(mys_varbit_numeric);
Datum
mys_varbit_numeric(PG_FUNCTION_ARGS)
{
	VarBit	   *v = PG_GETARG_VARBIT_P(0);
	int			bitlen = VARBITLEN(v);
	bits8	   *p = VARBITS(v);
	uint64		r = 0;

	for (int i = 0; i < bitlen; i++)
	{
		if (r >> 63)
			PG_RETURN_DATUM(uint64_numeric(PG_UINT64_MAX));
		r = (r << 1) | ((p[i / BITS_PER_BYTE] >> (7 - i % BITS_PER_BYTE)) & 1);
	}
	PG_RETURN_DATUM(uint64_numeric(r));
}

/* ----------------------------------------------------------------
 * Bit operations on unsigned 64-bit integers, as MySQL's & | ^ << >> ~
 * ----------------------------------------------------------------
 */

/*
 * MySQL's conversion of a number to an unsigned 64-bit operand: rounded,
 * negative values in two's complement; beyond the 64-bit range 2^64 - 1.
 */
static uint64
numeric_uint64(Numeric num)
{
	bool		have_error = false;
	int64		v;
	char	   *s;
	unsigned long long u;

	if (numeric_is_nan(num) || numeric_is_inf(num))
		return PG_UINT64_MAX;
	v = numeric_int8_opt_error(num, &have_error);
	if (!have_error)
		return (uint64) v;
	/* beyond int8: 2^63 .. 2^64 - 1 still fits */
	s = DatumGetCString(DirectFunctionCall1(numeric_out,
											DirectFunctionCall2(numeric_round,
																NumericGetDatum(num),
																Int32GetDatum(0))));
	if (s[0] == '-')
		return PG_UINT64_MAX;
	errno = 0;
	u = strtoull(s, NULL, 10);
	if (errno == ERANGE)
		return PG_UINT64_MAX;
	return (uint64) u;
}

PG_FUNCTION_INFO_V1(mys_uint64_bitop);
Datum
mys_uint64_bitop(PG_FUNCTION_ARGS)
{
	uint64		a = numeric_uint64(PG_GETARG_NUMERIC(0));
	uint64		b = numeric_uint64(PG_GETARG_NUMERIC(1));
	char		op = PG_GETARG_CHAR(2);
	uint64		r;

	switch (op)
	{
		case '&':
			r = a & b;
			break;
		case '|':
			r = a | b;
			break;
		case '^':
			r = a ^ b;
			break;
		case '<':
			r = b >= 64 ? 0 : a << b;
			break;
		case '>':
			r = b >= 64 ? 0 : a >> b;
			break;
		default:
			elog(ERROR, "unknown bit operation \"%c\"", op);
			r = 0;				/* keep compiler quiet */
	}
	if (r > (uint64) PG_INT64_MAX)
		out_of_range();
	PG_RETURN_INT64((int64) r);
}

/* ~x: the result is 2^64 - 1 - x, beyond int8 for most x */
PG_FUNCTION_INFO_V1(mys_uint64_bitnot);
Datum
mys_uint64_bitnot(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(uint64_numeric(~numeric_uint64(PG_GETARG_NUMERIC(0))));
}

/* ----------------------------------------------------------------
 * HEX() and UNHEX()
 * ----------------------------------------------------------------
 */
static const char hexdigits[] = "0123456789ABCDEF";

static text *
hex_uint64(uint64 v)
{
	char		buf[17];

	snprintf(buf, sizeof(buf), "%llX", (unsigned long long) v);
	return cstring_to_text(buf);
}

/* HEX(string): two hexadecimal digits per byte */
PG_FUNCTION_INFO_V1(mys_hex_bytes);
Datum
mys_hex_bytes(PG_FUNCTION_ARGS)
{
	bytea	   *b = PG_GETARG_BYTEA_PP(0);
	const unsigned char *p = (const unsigned char *) VARDATA_ANY(b);
	int			len = VARSIZE_ANY_EXHDR(b);
	text	   *result = (text *) palloc(VARHDRSZ + (Size) len * 2);
	char	   *d = VARDATA(result);

	for (int i = 0; i < len; i++)
	{
		*d++ = hexdigits[p[i] >> 4];
		*d++ = hexdigits[p[i] & 0xf];
	}
	SET_VARSIZE(result, VARHDRSZ + (Size) len * 2);
	PG_RETURN_TEXT_P(result);
}

/* HEX(N): the 64-bit two's complement, without leading zeros */
PG_FUNCTION_INFO_V1(mys_hex_int8);
Datum
mys_hex_int8(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(hex_uint64((uint64) PG_GETARG_INT64(0)));
}

/* HEX(DECIMAL): rounded half away from zero */
PG_FUNCTION_INFO_V1(mys_hex_numeric);
Datum
mys_hex_numeric(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(hex_uint64(numeric_uint64(PG_GETARG_NUMERIC(0))));
}

PG_FUNCTION_INFO_V1(mys_hex_float8);
Datum
mys_hex_float8(PG_FUNCTION_ARGS)
{
	float8		f = round(PG_GETARG_FLOAT8(0));
	uint64		v;

	if (isnan(f) || f < -9223372036854775808.0 || f >= 18446744073709551616.0)
		v = PG_UINT64_MAX;
	else if (f < 0)
		v = (uint64) (int64) f;
	else
		v = (uint64) f;
	PG_RETURN_TEXT_P(hex_uint64(v));
}

/* HEX(BIT column): the number, as for an integer */
PG_FUNCTION_INFO_V1(mys_hex_varbit);
Datum
mys_hex_varbit(PG_FUNCTION_ARGS)
{
	VarBit	   *v = PG_GETARG_VARBIT_P(0);
	int			bitlen = VARBITLEN(v);
	bits8	   *p = VARBITS(v);
	StringInfoData buf;
	int			nibbles = (bitlen + 3) / 4;
	int			lead = nibbles * 4 - bitlen;

	initStringInfo(&buf);
	for (int n = 0; n < nibbles; n++)
	{
		int			d = 0;

		for (int j = 0; j < 4; j++)
		{
			int			i = n * 4 + j - lead;	/* bit of v, MSB first */

			d <<= 1;
			if (i >= 0 && (p[i / BITS_PER_BYTE] >> (7 - i % BITS_PER_BYTE)) & 1)
				d |= 1;
		}
		if (d != 0 || buf.len > 0)
			appendStringInfoChar(&buf, hexdigits[d]);
	}
	if (buf.len == 0)
		appendStringInfoChar(&buf, '0');
	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * UNHEX(string): the bytes of pairs of hexadecimal digits (an odd number
 * of digits gets a leading 0); NULL with warning 1411 for any other
 * character, as in MySQL.
 */
PG_FUNCTION_INFO_V1(mys_unhex);
Datum
mys_unhex(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);
	const char *s = VARDATA_ANY(t);
	int			len = VARSIZE_ANY_EXHDR(t);
	int			n = (len + 1) / 2;
	bytea	   *result = (bytea *) palloc(VARHDRSZ + n);
	unsigned char *d = (unsigned char *) VARDATA(result);
	int			i = 0;

	if (len % 2)
	{
		int			lo = hexval(s[0]);

		if (lo < 0)
			goto bad;
		*d++ = (unsigned char) lo;
		i = 1;
	}
	for (; i < len; i += 2)
	{
		int			hi = hexval(s[i]),
					lo = hexval(s[i + 1]);

		if (hi < 0 || lo < 0)
			goto bad;
		*d++ = (unsigned char) (hi << 4 | lo);
	}
	SET_VARSIZE(result, VARHDRSZ + n);
	PG_RETURN_BYTEA_P(result);

bad:
	if (pg_verifymbstr(s, len, true))
	{
		char	   *msg = psprintf("Incorrect string value: '%.*s' for function unhex",
								   pg_mbcliplen(s, len, 64), s);

		mys_add_warning(1411, msg);
		pfree(msg);
	}
	else
		mys_add_warning(1411, "Incorrect string value for function unhex");
	PG_RETURN_NULL();
}

/* ----------------------------------------------------------------
 * 1.9 binary lengths.  Independent base types leave 1.8 domains and their
 * dependencies intact; typmods are byte counts (zero is a real length).
 * Never mark these coercions immutable: sql_mode is read at execution.
 * ---------------------------------------------------------------- */
static bool
binary_strict(void)
{
	if (!mys_is_mysql_session())
		return true;
	if (mys_binary_ignore_hook && mys_binary_ignore_hook())
		return false;
	return (mys_sqlMode & (MYS_MODE_STRICT_TRANS_TABLES | MYS_MODE_STRICT_ALL_TABLES)) != 0;
}

static bytea *
binary_sized(bytea *b, int32 typmod, bool fixed, bool explicit_cast)
{
	int len = VARSIZE_ANY_EXHDR(b);
	int size;
	bytea *r;

	if (typmod < 0)
		return b;
	if (len > typmod && !explicit_cast)
	{
		if (binary_strict())
			ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						   errmsg("Data too long for %s(%d)",
								  fixed ? "binary" : "varbinary", typmod)));
		mys_add_warning(1265, "Data truncated for binary string column");
	}
	if (len > typmod && explicit_cast)
		mys_add_warning(1292, "Truncated incorrect BINARY value");
	size = fixed ? typmod : Min(len, typmod);
	if (size == len)
		return b;
	r = palloc0(VARHDRSZ + size);
	SET_VARSIZE(r, VARHDRSZ + size);
	memcpy(VARDATA(r), VARDATA_ANY(b), Min(len, size));
	return r;
}

PG_FUNCTION_INFO_V1(mys_binary_typmodin);
Datum
mys_binary_typmodin(PG_FUNCTION_ARGS)
{
	int n;
	int32 *mods = ArrayGetIntegerTypmods(PG_GETARG_ARRAYTYPE_P(0), &n);
	bool fixed = strcmp(get_func_name(fcinfo->flinfo->fn_oid), "binary_n_typmodin") == 0;
	int limit = fixed ? 255 : 65535;

	if (n != 1 || mods[0] < 0 || mods[0] > limit)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					   errmsg("%s length must be between 0 and %d",
							  fixed ? "BINARY" : "VARBINARY", limit)));
	PG_RETURN_INT32(mods[0]);
}

PG_FUNCTION_INFO_V1(mys_binary_typmodout);
Datum
mys_binary_typmodout(PG_FUNCTION_ARGS)
{
	int32 n = PG_GETARG_INT32(0);
	PG_RETURN_CSTRING(n < 0 ? pstrdup("") : psprintf("(%d)", n));
}

PG_FUNCTION_INFO_V1(mys_binary_in);
Datum
mys_binary_in(PG_FUNCTION_ARGS)
{
	bytea *b = mys_is_mysql_session() ?
		(bytea *) cstring_to_text(PG_GETARG_CSTRING(0)) :
		DatumGetByteaP(DirectFunctionCall1(byteain, PG_GETARG_DATUM(0)));
	bool fixed = strcmp(get_func_name(fcinfo->flinfo->fn_oid), "binary_n_in") == 0;

	PG_RETURN_BYTEA_P(binary_sized(b, PG_GETARG_INT32(2), fixed, false));
}

PG_FUNCTION_INFO_V1(mys_binary_recv);
Datum
mys_binary_recv(PG_FUNCTION_ARGS)
{
	bytea *b = DatumGetByteaP(DirectFunctionCall1(bytearecv, PG_GETARG_DATUM(0)));
	bool fixed = strcmp(get_func_name(fcinfo->flinfo->fn_oid), "binary_n_recv") == 0;

	PG_RETURN_BYTEA_P(binary_sized(b, PG_GETARG_INT32(2), fixed, false));
}

PG_FUNCTION_INFO_V1(mys_binary_coerce);
Datum
mys_binary_coerce(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);

	if (getBaseType(get_fn_expr_argtype(fcinfo->flinfo, 0)) == BPCHAROID &&
		!(mys_sqlMode & MYS_MODE_PAD_CHAR_TO_FULL_LENGTH))
		b = (bytea *) DatumGetTextPP(DirectFunctionCall1(rtrim1, PointerGetDatum(b)));
	PG_RETURN_BYTEA_P(binary_sized(b, PG_GETARG_INT32(1),
								  is_mysql_type(get_fn_expr_rettype(fcinfo->flinfo), "binary_n"),
								  PG_GETARG_BOOL(2)));
}

/* ----------------------------------------------------------------
 * Arithmetic: prevent PostgreSQL from turning an unknown string into a
 * second hexlit. Do not affect binary comparisons ('A' = 0x41).
 * ---------------------------------------------------------------- */
static Node *
hexlit_text_operand(Node *arg)
{
	if (arg && IsA(arg, A_Const))
	{
		A_Const *c = (A_Const *) arg;

		if (!c->isnull && IsA(&c->val, String))
		{
			TypeCast *tc = makeNode(TypeCast);

			tc->arg = arg;
			tc->typeName = SystemTypeName("text");
			tc->location = c->location;
			return (Node *) tc;
		}
	}
	return arg;
}

A_Expr *
mys_hexlit_operator(A_Expr_Kind kind, char *name, Node *left, Node *right, int location)
{
	if (kind == AEXPR_OP &&
		(strcmp(name, "+") == 0 || strcmp(name, "-") == 0 ||
		 strcmp(name, "*") == 0 || strcmp(name, "/") == 0 ||
		 strcmp(name, "%") == 0 || strcmp(name, "&") == 0 ||
		 strcmp(name, "|") == 0 || strcmp(name, "^#") == 0 ||
		 strcmp(name, "<<") == 0 || strcmp(name, ">>") == 0))
	{
		if (hexlit_literal(left))
			right = hexlit_text_operand(right);
		if (hexlit_literal(right))
			left = hexlit_text_operand(left);
	}
	return makeSimpleA_Expr(kind, name, left, right, location);
}

/* Integer comparison support: specialize only a constant that fits int8.
 * Native int2/int8 and int4/int8 operators belong to integer_ops too, so
 * they index the original column. Larger unsigned literals keep the exact
 * numeric fallback; do not clamp, wrap, or introduce an overflow error. */
PG_FUNCTION_INFO_V1(mys_hexlit_cmp_support);
Datum
mys_hexlit_cmp_support(PG_FUNCTION_ARGS)
{
	Node *raw = (Node *) PG_GETARG_POINTER(0);

	if (IsA(raw, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) raw;
		FuncExpr *f = req->fcall;
		Node *l = linitial(f->args), *r = lsecond(f->args);
		bool lefthex = mys_is_hexlit(exprType(l));
		Node *h = lefthex ? l : r;
		Node *v = lefthex ? r : l;
		Oid vt = exprType(v);

		if (IsA(h, Const) && !((Const *) h)->constisnull &&
			(vt == INT2OID || vt == INT4OID || vt == INT8OID))
		{
			bytea *b = DatumGetByteaPP(((Const *) h)->constvalue);
			uint64 n = bytes_uint64((unsigned char *) VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
			char *fn = get_func_name(f->funcid);
			const char *names[] = {"eq", "ne", "lt", "le", "gt", "ge"};
			const char *ops[] = {"=", "<>", "<", "<=", ">", ">="};

			if (n <= PG_INT64_MAX)
			{
				for (int i = 0; i < (int) lengthof(names); i++)
				{
					if (strstr(fn, psprintf("_%s_", names[i])))
					{
						Const *c = makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
											 Int64GetDatum((int64) n), false, true);
						Oid op = LookupOperName(NULL,
							list_make2(makeString("pg_catalog"), makeString(pstrdup(ops[i]))),
							lefthex ? INT8OID : vt, lefthex ? vt : INT8OID, false, -1);
						Expr *out = make_opclause(op, BOOLOID, false,
							(Expr *) (lefthex ? (Node *) c : v),
							(Expr *) (lefthex ? v : (Node *) c), InvalidOid, InvalidOid);

						PG_RETURN_POINTER(out);
					}
				}
			}
		}
	}
	PG_RETURN_POINTER(NULL);
}

/* CONCAT is a tree of typed unary conversions and typed concatenations.
 * PostgreSQL resolves its return type before any surrounding expression,
 * unlike changing the Datum or result OID after parse analysis. */
static FuncCall *
binary_call(const char *name, List *args, int location)
{
	return makeFuncCall(list_make2(makeString("mysql"), makeString(pstrdup(name))),
						args, COERCE_EXPLICIT_CALL, location);
}

/* MySQL CAST lowering, leaving PostgreSQL casts untouched. */
Node *
mys_mysql_cast(Node *arg, TypeName *type, int location)
{
	TypeCast *cast;

	if (mys_binary_types_available && type->names &&
		strcmp(strVal(llast(type->names)), "varchar") == 0)
		return (Node *) binary_call("cast_char", list_make2(arg,
			type->typmods ? linitial(type->typmods) :
			binary_make_const(T_Integer, NULL, -1)), location);
	cast = makeNode(TypeCast);
	cast->arg = arg;
	cast->typeName = type;
	cast->location = location;
	return (Node *) cast;
}

/* Infer the result's string type from every argument without evaluating
 * them. A dead CASE branch carries the existing CONCAT type resolution:
 * bytea dominates native text. Its NULL value
 * is only a polymorphic type witness; no PG text ever contains binary bytes. */
static Node *
string_result_type(List *args, int location)
{
	Node *type_expr = NULL;
	CaseExpr *marker = makeNode(CaseExpr);
	CaseWhen *when = makeNode(CaseWhen);
	A_Const *no = makeNode(A_Const);

	foreach_ptr(Node, arg, args)
	{
		Node *one = (Node *) binary_call("concat_one", list_make1(copyObject(arg)), location);
		type_expr = type_expr ? (Node *) binary_call("concat_pair", list_make2(type_expr, one), location) : one;
	}
	Assert(type_expr != NULL);
	no->val.boolval.type = T_Boolean;
	no->val.boolval.boolval = false;
	no->location = location;
	when->expr = (Expr *) no;
	when->result = (Expr *) type_expr;
	when->location = location;
	marker->args = list_make1(when);
	marker->location = location;
	return (Node *) marker;
}

Node *
mys_binary_func_call(FuncCall *call)
{
	Node *result = NULL;

	if (mys_binary_types_available && !call->agg_order && !call->agg_distinct &&
		!call->func_variadic && !call->agg_star && list_length(call->args) >= 2 &&
		(list_length(call->funcname) == 1 ||
		 (list_length(call->funcname) == 2 && strcmp(strVal(linitial(call->funcname)), "mysql") == 0)))
	{
		const char *name = strVal(llast(call->funcname));

		if (strcmp(name, "concat_ws") == 0)
			return (Node *) binary_call("string_result", list_make2(
				string_result_type(call->args, call->location),
				binary_call("concat_ws_bytes", call->args, call->location)), call->location);
		if (strcmp(name, "elt") == 0)
		{
			List *values = list_copy_tail(call->args, 1);
			CaseExpr *choice = makeNode(CaseExpr);
			int index = 1;

			choice->arg = (Expr *) binary_call("elt_index", list_make1(linitial(call->args)), call->location);
			choice->location = call->location;
			foreach_ptr(Node, arg, values)
			{
				CaseWhen *when = makeNode(CaseWhen);

				when->expr = (Expr *) binary_make_const(T_Integer, NULL, index++);
				when->result = (Expr *) binary_call("raw_bytes", list_make1(arg), call->location);
				when->location = call->location;
				choice->args = lappend(choice->args, when);
			}
			return (Node *) binary_call("string_result", list_make2(
				string_result_type(values, call->location), choice), call->location);
		}
	}
	if (mys_binary_types_available &&
		(list_length(call->funcname) == 1 ||
		 (list_length(call->funcname) == 2 && strcmp(strVal(linitial(call->funcname)), "mysql") == 0)) &&
		strcmp(strVal(llast(call->funcname)), "ifnull") == 0 &&
		list_length(call->args) == 2 &&
		(hexlit_literal(linitial(call->args)) || hexlit_literal(lsecond(call->args))))
	{
		CoalesceExpr *c = makeNode(CoalesceExpr);

		c->args = list_make2(binary_call("raw_bytes", list_make1(linitial(call->args)), call->location),
							binary_call("raw_bytes", list_make1(lsecond(call->args)), call->location));
		c->location = call->location;
		return (Node *) binary_call("raw_bytes", list_make1(c), call->location);
	}
	if (mys_binary_types_available &&
		(list_length(call->funcname) == 1 ||
		 (list_length(call->funcname) == 2 && strcmp(strVal(linitial(call->funcname)), "mysql") == 0)) &&
		strcmp(strVal(llast(call->funcname)), "if") == 0 &&
		list_length(call->args) == 3 &&
		(hexlit_literal(lsecond(call->args)) || hexlit_literal(lthird(call->args))))
	{
		CaseExpr *c = makeNode(CaseExpr);
		CaseWhen *w = makeNode(CaseWhen);

		w->expr = (Expr *) binary_call("binary_truth", list_make1(linitial(call->args)), call->location);
		w->result = (Expr *) binary_call("raw_bytes", list_make1(lsecond(call->args)), call->location);
		w->location = call->location;
		c->args = list_make1(w);
		c->defresult = (Expr *) binary_call("raw_bytes", list_make1(lthird(call->args)), call->location);
		c->location = call->location;
		/* func_application's parent expects a FuncCall. CASE inside this
		 * identity call preserves IF's lazy evaluation of the branches. */
		return (Node *) binary_call("raw_bytes", list_make1(c), call->location);
	}
	if (!mys_binary_types_available || call->agg_order || call->agg_distinct ||
		call->func_variadic || call->agg_star ||
		(list_length(call->funcname) != 1 &&
		 !(list_length(call->funcname) == 2 &&
		   strcmp(strVal(linitial(call->funcname)), "mysql") == 0)) ||
		strcmp(strVal(llast(call->funcname)), "concat") != 0)
		return (Node *) call;
	foreach_ptr(Node, arg, call->args)
	{
		Node *one = (Node *) binary_call("concat_one", list_make1(arg), call->location);

		result = result ? (Node *) binary_call("concat_pair", list_make2(result, one), call->location) : one;
	}
	return result ? result : (Node *) call;
}

/* Convert one SQL argument to its string bytes. Unknown literals are text;
 * binary and hexlit use their payloads. */
PG_FUNCTION_INFO_V1(mys_concat_one);
Datum
mys_concat_one(PG_FUNCTION_ARGS)
{
	Oid typid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Oid base = mys_binary_base_type(typid);
	Oid out;
	bool varlena;

	if (base == BYTEAOID || base == TEXTOID || base == VARCHAROID)
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	getTypeOutputInfo(typid, &out, &varlena);
	PG_RETURN_TEXT_P(cstring_to_text(OidOutputFunctionCall(out, PG_GETARG_DATUM(0))));
}

/* Bounds and negative counts follow MySQL, using byte positions for binary. */
PG_FUNCTION_INFO_V1(mys_binary_left);
Datum
mys_binary_left(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);
	int n = Min(Max(PG_GETARG_INT32(1), 0), (int) VARSIZE_ANY_EXHDR(b));

	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(VARDATA_ANY(b), n));
}

PG_FUNCTION_INFO_V1(mys_binary_right);
Datum
mys_binary_right(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);
	int len = VARSIZE_ANY_EXHDR(b);
	int n = Min(Max(PG_GETARG_INT32(1), 0), len);

	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(VARDATA_ANY(b) + len - n, n));
}

PG_FUNCTION_INFO_V1(mys_binary_ascii);
Datum
mys_binary_ascii(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);
	PG_RETURN_INT32(VARSIZE_ANY_EXHDR(b) ? (unsigned char) *VARDATA_ANY(b) : 0);
}

/* These functions iterate explicitly: pg_mbstrlen_with_len stops at NUL. */
static int
string_chars(const char *s, int len)
{
	int chars = 0;
	for (int i = 0; i < len; chars++)
		i += s[i] == '\0' ? 1 : pg_mblen(s + i);
	return chars;
}

PG_FUNCTION_INFO_V1(mys_string_substr);
Datum
mys_string_substr(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);
	const char *s = VARDATA_ANY(b);
	int len = VARSIZE_ANY_EXHDR(b);
	int count = len;
	int64 start = PG_GETARG_INT32(1);
	int64 n = PG_NARGS() > 2 ? PG_GETARG_INT32(2) : count;
	int from, to;

	start = start > 0 ? start - 1 : (start < 0 ? count + start : count);
	if (start < 0 || start >= count || n <= 0)
		PG_RETURN_BYTEA_P((bytea *) cstring_to_text(""));
	n = Min(n, count - start);
	from = start;
	to = start + n;
	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(s + from, to - from));
}

bool
mys_is_fixed_binary(Oid typid)
{
	return is_mysql_type(getBaseType(typid), "binary_n");
}

/* A safe any -> bytes conversion, without invoking bytea's escaped output. */
static bytea *
binary_arg(FunctionCallInfo fcinfo, int i)
{
	Oid t = get_fn_expr_argtype(fcinfo->flinfo, i);
	Oid base = mys_binary_base_type(t), out;
	bool varlena;

	if (base == BYTEAOID || base == TEXTOID || base == VARCHAROID)
		return PG_GETARG_BYTEA_PP(i);
	getTypeOutputInfo(t, &out, &varlena);
	return (bytea *) cstring_to_text(OidOutputFunctionCall(out, PG_GETARG_DATUM(i)));
}

/* CAST AS CHAR uses characters, validates even truncated-away bytes and
 * reports MySQL's SELECT truncation diagnostic independently of sql_mode. */
PG_FUNCTION_INFO_V1(mys_cast_char);
Datum
mys_cast_char(PG_FUNCTION_ARGS)
{
	bytea *b = getBaseType(get_fn_expr_argtype(fcinfo->flinfo, 0)) == BPCHAROID ?
		(bytea *) DatumGetTextPP(DirectFunctionCall1(rtrim1, PG_GETARG_DATUM(0))) :
		binary_arg(fcinfo, 0);
	int len = VARSIZE_ANY_EXHDR(b);
	int limit = PG_GETARG_INT32(1);
	int clipped;

	pg_verifymbstr(VARDATA_ANY(b), len, false);
	clipped = limit < 0 ? len : pg_mbcharcliplen(VARDATA_ANY(b), len, limit);
	if (clipped < len)
		mys_add_warning(1292, "Truncated incorrect CHAR value");
	PG_RETURN_TEXT_P(cstring_to_text_with_len(VARDATA_ANY(b), clipped));
}

PG_FUNCTION_INFO_V1(mys_convert_using);
Datum
mys_convert_using(PG_FUNCTION_ARGS)
{
	bytea *b = getBaseType(get_fn_expr_argtype(fcinfo->flinfo, 0)) == BPCHAROID ?
		(bytea *) DatumGetTextPP(DirectFunctionCall1(rtrim1, PG_GETARG_DATUM(0))) :
		binary_arg(fcinfo, 0);
	char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* UTF-8 MySQL charset aliases share the database's Unicode text value.
	 * Other names retain the nearest Unicode representation with a warning. */
	if (pg_strcasecmp(name, "utf8mb4") && pg_strcasecmp(name, "utf8mb3") &&
		pg_strcasecmp(name, "utf8") && pg_strcasecmp(name, "utf-8"))
		mys_add_warning(1105, "CONVERT character set mapped to UTF-8");
	pg_verifymbstr(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), false);
	PG_RETURN_TEXT_P((text *) b);
}

PG_FUNCTION_INFO_V1(mys_raw_bytes);
Datum
mys_raw_bytes(PG_FUNCTION_ARGS)
{
	PG_RETURN_BYTEA_P(binary_arg(fcinfo, 0));
}

PG_FUNCTION_INFO_V1(mys_binary_truth);
Datum
mys_binary_truth(PG_FUNCTION_ARGS)
{
	Oid t = get_fn_expr_argtype(fcinfo->flinfo, 0);
	bytea *b;
	const char *s;
	char *num;
	int len, start = 0, i;
	bool digits = false;
	double n;

	if (getBaseType(t) == BOOLOID)
		PG_RETURN_BOOL(PG_GETARG_BOOL(0));
	b = binary_arg(fcinfo, 0);
	s = VARDATA_ANY(b);
	len = VARSIZE_ANY_EXHDR(b);
	if (mys_is_hexlit(t))
		PG_RETURN_BOOL(bytes_uint64((const unsigned char *) s, len) != 0);
	while (start < len && strchr(" \t\r\n\v\f", s[start]) && s[start] != '\0') start++;
	i = start;
	if (i < len && (s[i] == '+' || s[i] == '-')) i++;
	while (i < len && s[i] >= '0' && s[i] <= '9') i++, digits = true;
	if (i < len && s[i] == '.')
	{
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') i++, digits = true;
	}
	if (digits && i < len && (s[i] == 'e' || s[i] == 'E'))
	{
		int j = i + 1;
		if (j < len && (s[j] == '+' || s[j] == '-')) j++;
		if (j < len && s[j] >= '0' && s[j] <= '9')
		{
			do { j++; } while (j < len && s[j] >= '0' && s[j] <= '9');
			i = j;
		}
	}
	num = digits ? pnstrdup(s + start, i - start) : pstrdup("0");
	n = strtod(num, NULL);
	while (i < len && s[i] != '\0' && strchr(" \t\r\n\v\f", s[i])) i++;
	if (!digits || i != len)
		mys_add_warning(1292, "Truncated incorrect DOUBLE value");
	PG_RETURN_BOOL(n != 0);
}

PG_FUNCTION_INFO_V1(mys_string_replace);
Datum
mys_string_replace(PG_FUNCTION_ARGS)
{
	bytea *a = binary_arg(fcinfo, 0), *b = binary_arg(fcinfo, 1), *c = binary_arg(fcinfo, 2);
	const char *s = VARDATA_ANY(a), *needle = VARDATA_ANY(b);
	int len = VARSIZE_ANY_EXHDR(a), n = VARSIZE_ANY_EXHDR(b), r = VARSIZE_ANY_EXHDR(c);
	StringInfoData buf;

	if (n == 0)
		PG_RETURN_BYTEA_P(a);
	initStringInfo(&buf);
	for (int i = 0; i < len;)
	{
		if (n <= len - i && memcmp(s + i, needle, n) == 0)
		{
			appendBinaryStringInfo(&buf, VARDATA_ANY(c), r);
			i += n;
		}
		else
			appendStringInfoChar(&buf, s[i++]);
	}
	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(buf.data, buf.len));
}

PG_FUNCTION_INFO_V1(mys_string_trim);
Datum
mys_string_trim(PG_FUNCTION_ARGS)
{
	bytea *b = PG_GETARG_BYTEA_PP(0);
	const char *s = VARDATA_ANY(b), *rem = " ";
	int len = VARSIZE_ANY_EXHDR(b), n = 1, lo = 0, hi = len;
	char *name = get_func_name(fcinfo->flinfo->fn_oid);

	if (PG_NARGS() == 2)
	{
		bytea *r = binary_arg(fcinfo, 1);
		rem = VARDATA_ANY(r); n = VARSIZE_ANY_EXHDR(r);
	}
	if (n > 0)
	{
		if (strcmp(name, "rtrim") != 0)
			while (n <= hi - lo && memcmp(s + lo, rem, n) == 0) lo += n;
		if (strcmp(name, "ltrim") != 0)
			while (n <= hi - lo && memcmp(s + hi - n, rem, n) == 0) hi -= n;
	}
	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(s + lo, hi - lo));
}

/* Binary types have their original 1.8 domain behavior before the 1.9 SQL
 * update. Once pg_type invalidation arrives, the next parse adopts typmods
 * even on a connection opened before ALTER EXTENSION. */
TypeName *
mys_binary_type_name(const char *name, int32 typmod, int location)
{
	TypeName *type;

	mys_binary_session_init();
	if (!mys_binary_types_available)
	{
		type = makeTypeNameFromNameList(list_make2(makeString("mysql"),
				makeString(pstrdup(strcmp(name, "binary_n") == 0 ? "binary" : "varbinary"))));
	}
	else
	{
		type = makeTypeNameFromNameList(list_make2(makeString("mysql"), makeString(pstrdup(name))));
		if (typmod >= 0)
			type->typmods = list_make1(binary_make_const(T_Integer, NULL, typmod));
	}
	type->location = location;
	return type;
}

/* The first argument is a typed NULL witness, never data to stringify. */
PG_FUNCTION_INFO_V1(mys_string_result);
Datum
mys_string_result(PG_FUNCTION_ARGS)
{
	bytea *value;
	Oid result_type = get_fn_expr_rettype(fcinfo->flinfo);

	if (PG_ARGISNULL(1))
		PG_RETURN_NULL();
	value = binary_arg(fcinfo, 1);
	if (result_type == TEXTOID)
		pg_verifymbstr(VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value), false);
	else if (result_type != BYTEAOID)
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
					   errmsg("string_result requires a string type witness")));
	PG_RETURN_BYTEA_P(value);
}

PG_FUNCTION_INFO_V1(mys_concat_ws_bytes);
Datum
mys_concat_ws_bytes(PG_FUNCTION_ARGS)
{
	bytea *sep;
	StringInfoData out;
	bool first = true;

	if (PG_NARGS() < 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					   errmsg("concat_ws_bytes requires a separator")));
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	sep = binary_arg(fcinfo, 0);
	initStringInfo(&out);
	for (int i = 1; i < PG_NARGS(); i++)
	{
		bytea *value;

		if (PG_ARGISNULL(i))
			continue;
		value = binary_arg(fcinfo, i);
		if (!first)
			appendBinaryStringInfo(&out, VARDATA_ANY(sep), VARSIZE_ANY_EXHDR(sep));
		appendBinaryStringInfo(&out, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value));
		first = false;
	}
	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(out.data, out.len));
}

/* ELT rounds numeric indexes but reads the integer prefix of strings.
 * Zero is an out-of-range sentinel: never overflow an array/argument index. */
PG_FUNCTION_INFO_V1(mys_elt_index);
Datum
mys_elt_index(PG_FUNCTION_ARGS)
{
	Oid type = getBaseType(get_fn_expr_argtype(fcinfo->flinfo, 0));
	int64 index = 0;

	if (mys_is_hexlit(type))
	{
		uint64 n = hexlit_uint64(fcinfo, 0);
		PG_RETURN_INT32(n > PG_INT32_MAX ? 0 : (int32) n);
	}
	switch (type)
	{
		case BOOLOID: index = PG_GETARG_BOOL(0) ? 1 : 0; break;
		case INT2OID: index = PG_GETARG_INT16(0); break;
		case INT4OID: index = PG_GETARG_INT32(0); break;
		case INT8OID: index = PG_GETARG_INT64(0); break;
		case NUMERICOID:
		{
			bool error = false;
			Numeric n = PG_GETARG_NUMERIC(0);

			if (numeric_is_nan(n) || numeric_is_inf(n))
				PG_RETURN_INT32(0);
			index = numeric_int8_opt_error(n, &error);
			if (error)
			{
				mys_add_warning(1292, "Truncated incorrect DECIMAL value");
				PG_RETURN_INT32(0);
			}
			break;
		}
		case FLOAT4OID:
		case FLOAT8OID:
		{
			double n = type == FLOAT4OID ? PG_GETARG_FLOAT4(0) : PG_GETARG_FLOAT8(0);
			n = rint(n);
			PG_RETURN_INT32(isfinite(n) && n > 0 && n <= PG_INT32_MAX ? (int32) n : 0);
		}
		default:
		{
			bytea *b = binary_arg(fcinfo, 0);
			const char *s = VARDATA_ANY(b);
			int len = VARSIZE_ANY_EXHDR(b), i = 0;
			bool negative = false, digits = false, overflow = false;
			uint64 n = 0;

			while (i < len && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) i++;
			if (i < len && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
			while (i < len && s[i] >= '0' && s[i] <= '9')
			{
				unsigned digit = s[i++] - '0';
				digits = true;
				if (n > (PG_UINT64_MAX - digit) / 10)
					overflow = true;
				else if (!overflow)
					n = n * 10 + digit;
			}
			while (i < len && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) i++;
			if (!digits || i != len || overflow)
				mys_add_warning(1292, "Truncated incorrect INTEGER value");
			PG_RETURN_INT32(!negative && !overflow && n <= PG_INT32_MAX ? (int32) n : 0);
		}
	}
	PG_RETURN_INT32(index > 0 && index <= PG_INT32_MAX ? (int32) index : 0);
}

static bool
binary_string_arg(FunctionCallInfo fcinfo, int argno)
{
	Oid type = get_fn_expr_argtype(fcinfo->flinfo, argno);

	return mys_binary_base_type(type) == BYTEAOID;
}

/* Unlike pg_mb2wchar_with_len(), retain U+0000 and everything after it.
 * Binary REGEXP operates on unsigned bytes, including invalid UTF8. */
static pg_wchar *
string_wchars(bytea *value, bool binary, int *count)
{
	const unsigned char *s = (const unsigned char *) VARDATA_ANY(value);
	int len = VARSIZE_ANY_EXHDR(value), n = 0;
	pg_wchar *wide = palloc_array(pg_wchar, (Size) len + 1);

	for (int i = 0; i < len;)
	{
		int step = binary || s[i] == 0 ? 1 : pg_mblen((const char *) s + i);

		if (binary || s[i] == 0)
			wide[n++] = s[i];
		else
		{
			pg_wchar one[2];

			pg_mb2wchar_with_len((const char *) s + i, one, step);
			wide[n++] = one[0];
		}
		i += step;
	}
	wide[n] = 0;
	*count = n;
	return wide;
}

typedef struct StringRegexCache
{
	MemoryContext context;
	regex_t regex;
	bytea *pattern;
	Oid collid;
	int flags;
	bool binary;
} StringRegexCache;

static void
string_regex_error(int rc, regex_t *regex)
{
	char message[256];

	pg_regerror(rc, regex, message, sizeof(message));
	ereport(ERROR, (errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				   errmsg("invalid regular expression: %s", message)));
}

static bool
string_regex(FunctionCallInfo fcinfo, bool insensitive)
{
	bytea *subject = binary_arg(fcinfo, 0), *pattern = binary_arg(fcinfo, 1);
	bool binary = binary_string_arg(fcinfo, 0) || binary_string_arg(fcinfo, 1);
	Oid collid = PG_GET_COLLATION();
	int flags = REG_ADVANCED | REG_NOSUB;
	StringRegexCache *cache = fcinfo->flinfo->fn_extra;
	pg_wchar *wide;
	int n, rc;

	if (binary)
		collid = C_COLLATION_OID;
	else if (!OidIsValid(collid) || !pg_newlocale_from_collation(collid)->deterministic)
		collid = DEFAULT_COLLATION_OID;
	if (insensitive && !binary)
		flags |= REG_ICASE;
	if (cache == NULL)
	{
		cache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt, sizeof(*cache));
		cache->context = AllocSetContextCreate(fcinfo->flinfo->fn_mcxt,
							"MySQL string regexp", ALLOCSET_SMALL_SIZES);
		fcinfo->flinfo->fn_extra = cache;
	}
	if (cache->pattern == NULL || cache->flags != flags || cache->collid != collid ||
		cache->binary != binary ||
		VARSIZE_ANY_EXHDR(cache->pattern) != VARSIZE_ANY_EXHDR(pattern) ||
		memcmp(VARDATA_ANY(cache->pattern), VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern)))
	{
		MemoryContext old;

		cache->pattern = NULL;
		MemoryContextReset(cache->context);
		old = MemoryContextSwitchTo(cache->context);
		wide = string_wchars(pattern, binary, &n);
		rc = pg_regcomp(&cache->regex, wide, n, flags, collid);
		pfree(wide);
		if (rc != REG_OKAY)
			string_regex_error(rc, &cache->regex);
		cache->pattern = (bytea *) cstring_to_text_with_len(VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern));
		cache->flags = flags;
		cache->collid = collid;
		cache->binary = binary;
		MemoryContextSwitchTo(old);
	}
	wide = string_wchars(subject, binary, &n);
	rc = pg_regexec(&cache->regex, wide, n, 0, NULL, 0, NULL, 0);
	pfree(wide);
	if (rc != REG_OKAY && rc != REG_NOMATCH)
		string_regex_error(rc, &cache->regex);
	return rc == REG_OKAY;
}

#define STRING_REGEX(name, insensitive, negate) \
PG_FUNCTION_INFO_V1(name); \
Datum name(PG_FUNCTION_ARGS) \
{ PG_RETURN_BOOL(string_regex(fcinfo, insensitive) != negate); }
STRING_REGEX(mys_string_regex, false, false)
STRING_REGEX(mys_string_iregex, true, false)
STRING_REGEX(mys_string_nregex, false, true)
STRING_REGEX(mys_string_niregex, true, true)

/* The dedicated MySQL collations assign one weight to each character.
 * Search those weights without the PAD SPACE comparison used by indexes.
 * KMP takes O(haystack + needle) character steps, even for repetitive input;
 * only the needle and its failure links are materialized. NUL is a weight,
 * not a terminator. Other ICU collations can expand/contract characters and
 * must keep the generic substring path below. */
static int32
string_weight_position(MysSimpleCollation kind,
					   const char *h, int hlen, int offset, int pos,
					   const char *p, int plen)
{
	int n = string_chars(p, plen);
	uint32 *pattern;
	int *failure;
	int poffset = 0, matched = 0, result = 0;
	int window = offset, scanned = 0;
	uint32 weight;

	Assert(n > 0);
	if (offset == hlen)
		return 0;
	pattern = palloc_array(uint32, n);
	failure = palloc_array(int, n);
	for (int i = 0; i < n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (!mys_simple_weight_next(kind, p, plen, &poffset, &pattern[i]))
			elog(ERROR, "inconsistent MySQL character count");
		if (i == 0)
			failure[i] = 0;
		else
		{
			int j = failure[i - 1];

			while (j > 0 && pattern[i] != pattern[j])
				j = failure[j - 1];
			if (pattern[i] == pattern[j])
				j++;
			failure[i] = j;
		}
	}
	while (mys_simple_weight_next(kind, h, hlen, &offset, &weight))
	{
		CHECK_FOR_INTERRUPTS();
		if (++scanned > n)
			window += pg_mblen(h + window);
		while (matched > 0 && weight != pattern[matched])
			matched = failure[matched - 1];
		if (weight == pattern[matched])
			matched++;
		if (matched == n)
		{
			/* MySQL 5.7 compares a needle-sized byte span: equivalent
			 * weights alone do not make 'e' match a two-byte accented E.
			 * Individual character widths may differ ('éA' / 'eÁ'), so
			 * check the whole span. The sliding window advances once per
			 * character and needs no haystack-sized offset array. */
			if (offset - window == plen)
			{
				result = pos - n + 1;
				break;
			}
			matched = failure[n - 1];
		}
		pos++;
	}
	pfree(failure);
	pfree(pattern);
	return result;
}

/* Character positions count NUL, while any binary operand requests byte
 * positions and byte comparisons. ICU receives explicit lengths; unlike
 * ordinary TEXT comparison, substring searches do not trim trailing spaces. */
static bool
string_search_equal(const char *a, int m, const char *b, int n, pg_locale_t locale)
{
	/* ICU otherwise treats NUL as ignorable and would report the position
	 * before a NUL as a match for a needle that starts after it. */
	for (;;)
	{
		const char *az = memchr(a, 0, m), *bz = memchr(b, 0, n);
		int x = az ? az - a : m, y = bz ? bz - b : n;

		if (pg_strncoll(a, x, b, y, locale) != 0 || (az == NULL) != (bz == NULL))
			return false;
		if (az == NULL)
			return true;
		a += x + 1; m -= x + 1;
		b += y + 1; n -= y + 1;
	}
}

static int32
string_position(FunctionCallInfo fcinfo, bool instr)
{
	bytea *haystack = binary_arg(fcinfo, instr ? 0 : 1);
	bytea *needle = binary_arg(fcinfo, instr ? 1 : 0);
	const char *h = VARDATA_ANY(haystack), *p = VARDATA_ANY(needle);
	int hlen = VARSIZE_ANY_EXHDR(haystack), plen = VARSIZE_ANY_EXHDR(needle);
	bool binary = binary_string_arg(fcinfo, 0) || binary_string_arg(fcinfo, 1);
	pg_locale_t locale;
	int start = PG_NARGS() > 2 ? PG_GETARG_INT32(2) : 1;
	int offset = 0, pos = 1;

	if (start < 1)
		return 0;
	while (offset < hlen && pos < start)
	{
		offset += binary || h[offset] == 0 ? 1 : pg_mblen(h + offset);
		pos++;
	}
	if (pos < start)
		return 0;
	if (plen == 0)
		return pos;
	if (!binary)
	{
		MysSimpleCollation kind = mys_simple_collation(PG_GET_COLLATION());

		if (kind != MYS_SIMPLE_UNSUPPORTED)
			return string_weight_position(kind, h, hlen, offset, pos, p, plen);
	}
	locale = binary ? NULL : pg_newlocale_from_collation(PG_GET_COLLATION());
	for (; offset < hlen; pos++)
	{
		CHECK_FOR_INTERRUPTS();
		if (binary || locale->deterministic)
		{
			if (plen <= hlen - offset && memcmp(h + offset, p, plen) == 0)
				return pos;
		}
		else
		{
			/* Match variable-width collation equivalents on character
			 * boundaries, as PG18 does for nondeterministic collations. */
			for (int end = offset; end < hlen;)
			{
				end += h[end] == 0 ? 1 : pg_mblen(h + end);
				if (string_search_equal(h + offset, end - offset, p, plen, locale))
					return pos;
			}
		}
		offset += binary || h[offset] == 0 ? 1 : pg_mblen(h + offset);
	}
	return 0;
}

PG_FUNCTION_INFO_V1(mys_string_locate);
Datum
mys_string_locate(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (PG_NARGS() > 2 && PG_ARGISNULL(2))
		PG_RETURN_INT32(0);
	PG_RETURN_INT32(string_position(fcinfo, false));
}

PG_FUNCTION_INFO_V1(mys_string_instr);
Datum
mys_string_instr(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(string_position(fcinfo, true));
}

/* The SQL overload fixes the return type before the expression is analyzed. */
PG_FUNCTION_INFO_V1(mys_group_concat_binary);
Datum
mys_group_concat_binary(PG_FUNCTION_ARGS)
{
	ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
	text *sep = PG_GETARG_TEXT_PP(1);
	ArrayIterator iterator = array_create_iterator(array, 0, NULL);
	StringInfoData out;
	Datum value;
	bool isnull;
	bool first = true;

	initStringInfo(&out);
	while (array_iterate(iterator, &value, &isnull))
	{
		bytea *b;

		if (isnull) continue;
		b = DatumGetByteaPP(value);
		if (!first)
			appendBinaryStringInfo(&out, VARDATA_ANY(sep), VARSIZE_ANY_EXHDR(sep));
		appendBinaryStringInfo(&out, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
		first = false;
	}
	array_free_iterator(iterator);
	if (first) PG_RETURN_NULL();
	PG_RETURN_BYTEA_P((bytea *) cstring_to_text_with_len(out.data, out.len));
}
