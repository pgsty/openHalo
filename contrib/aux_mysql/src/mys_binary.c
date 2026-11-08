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
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parser.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/varbit.h"

#include "halo_mysql.h"

/*
 * Does this session's grammar make hexadecimal literals mysql.hexlit?  Only
 * once the extension has the type (aux_mysql 1.8): until ALTER EXTENSION
 * UPDATE, new binaries keep the old bit-string literals.
 */
bool		mys_hexlit_available = false;

static bool
is_hexlit(Oid typid)
{
	HeapTuple	tup;
	bool		result = false;

	if (typid < FirstNormalObjectId)
		return false;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_type t = (Form_pg_type) GETSTRUCT(tup);

		result = strcmp(NameStr(t->typname), "hexlit") == 0 &&
			t->typnamespace == get_namespace_oid("mysql", true);
		ReleaseSysCache(tup);
	}
	return result;
}

/* in a transaction, when a MySQL session starts */
void
mys_binary_session_init(void)
{
	Oid			nsp = get_namespace_oid("mysql", true);

	mys_hexlit_available = OidIsValid(nsp) &&
		OidIsValid(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum("hexlit"),
								   ObjectIdGetDatum(nsp)));
}

/*
 * getBaseType(), except that mysql.hexlit counts as bytea: code that sends
 * or copies the bytes of a binary string handles both alike.
 */
Oid
mys_binary_base_type(Oid typid)
{
	Oid			base = getBaseType(typid);

	return is_hexlit(base) ? BYTEAOID : base;
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
make_const(NodeTag tag, char *str, int ival)
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
					return make_const(T_Integer, NULL, (int) v);
				return make_const(T_Float, psprintf("%llu", (unsigned long long) v), 0);
			}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			{
				char	   *s = palloc(len + 1);

				memcpy(s, VARDATA_ANY(b), len);
				s[len] = '\0';
				pg_verifymbstr(s, len, false);
				return make_const(T_String, s, 0);
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
