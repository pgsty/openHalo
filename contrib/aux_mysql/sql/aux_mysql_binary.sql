-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- ==== aux_mysql 1.8: MySQL binary strings (hand-written; C in src/mys_binary.c) ====
-- Part of aux_mysql--1.8.sql, and alone the 1.7 -> 1.8 update (see Makefile).
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.8'" to load this file. \quit
SET LOCAL search_path = mysql, pg_catalog, public;

-- ---------------------------------------------------------------------------
-- Hexadecimal (X'41', 0x41) and bit-value (b'101', 0b101) literals.  MySQL
-- makes them a binary string that is an unsigned integer in numeric context:
-- 0x41 + 0 = 65, 0x10 = 16, i & 0x40, CONCAT(X'41', 'b') = 'Ab', X'41' = 'A'.
-- The grammar makes them '\x..'::mysql.hexlit, bytea's representation:
--  - the implicit cast to bytea makes one a binary string wherever no
--    operator below applies: result sets (VARBINARY), CONCAT, functions of
--    bytea, BLOB / BINARY columns;
--  - with a number (or a string or BLOB, read as a number as MySQL does),
--    arithmetic and bit operations use the integer and yield BIGINT ("/":
--    DECIMAL; with a DECIMAL / DOUBLE operand that type).  aux_mysql has no
--    unsigned 64-bit integer: a result beyond 2^63 - 1 is an error;
--  - comparisons with numbers (and BIT columns) compare the integer, with
--    strings, BLOBs and other literals the bytes;
--  - assignment casts store the integer into numeric columns, the bytes into
--    string columns (if they are valid text) and the low bits into BIT(n).
-- Each operator has an exact signature: otherwise the implicit cast to bytea
-- would select aux_mysql's BLOB operators, which read bytes as digits.
-- ---------------------------------------------------------------------------
CREATE TYPE mysql.hexlit;
CREATE FUNCTION mysql.hexlit_in(cstring) RETURNS mysql.hexlit
    AS 'byteain' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_out(mysql.hexlit) RETURNS cstring
    AS 'byteaout' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_recv(internal) RETURNS mysql.hexlit
    AS 'bytearecv' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_send(mysql.hexlit) RETURNS bytea
    AS 'byteasend' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE mysql.hexlit (
    INPUT = mysql.hexlit_in, OUTPUT = mysql.hexlit_out,
    RECEIVE = mysql.hexlit_recv, SEND = mysql.hexlit_send,
    LIKE = pg_catalog.bytea, CATEGORY = 'U');
COMMENT ON TYPE mysql.hexlit IS 'MySQL hexadecimal / bit-value literal: a binary string, an unsigned integer in numeric context';

-- the integer (more than 8 significant bytes: 2^64 - 1, as in MySQL)
CREATE FUNCTION mysql.hexlit_numeric(mysql.hexlit) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_hexlit_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_int8(mysql.hexlit) RETURNS int8
    AS '$libdir/aux_mysql', 'mys_hexlit_int8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_int4(mysql.hexlit) RETURNS int4
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hexlit_int8($1)::pg_catalog.int4';
CREATE FUNCTION mysql.hexlit_int2(mysql.hexlit) RETURNS int2
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hexlit_int8($1)::pg_catalog.int2';
CREATE FUNCTION mysql.hexlit_float8(mysql.hexlit) RETURNS float8
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hexlit_numeric($1)::pg_catalog.float8';
CREATE FUNCTION mysql.hexlit_float4(mysql.hexlit) RETURNS float4
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hexlit_numeric($1)::pg_catalog.float4';
CREATE FUNCTION mysql.hexlit_bool(mysql.hexlit) RETURNS bool
    AS '$libdir/aux_mysql', 'mys_hexlit_bool' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- the bytes as text: an error (1366) unless valid in the database encoding
CREATE FUNCTION mysql.hexlit_text(mysql.hexlit) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hexlit_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_varchar(mysql.hexlit) RETURNS varchar
    AS '$libdir/aux_mysql', 'mys_hexlit_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_bpchar(mysql.hexlit) RETURNS bpchar
    AS '$libdir/aux_mysql', 'mys_hexlit_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- BIT(n): the low n bits; more significant bits are an error (1406)
CREATE FUNCTION mysql.hexlit_varbit(mysql.hexlit, int4, bool) RETURNS varbit
    AS '$libdir/aux_mysql', 'mys_hexlit_varbit' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_bit(mysql.hexlit, int4, bool) RETURNS bit
    AS '$libdir/aux_mysql', 'mys_hexlit_varbit' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
-- the unsigned value of a BIT column
CREATE FUNCTION mysql.varbit_numeric(varbit) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_varbit_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (mysql.hexlit AS bytea) WITHOUT FUNCTION AS IMPLICIT;
CREATE CAST (mysql.hexlit AS int8) WITH FUNCTION mysql.hexlit_int8(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS int4) WITH FUNCTION mysql.hexlit_int4(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS int2) WITH FUNCTION mysql.hexlit_int2(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS numeric) WITH FUNCTION mysql.hexlit_numeric(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS float8) WITH FUNCTION mysql.hexlit_float8(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS float4) WITH FUNCTION mysql.hexlit_float4(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS bool) WITH FUNCTION mysql.hexlit_bool(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS text) WITH FUNCTION mysql.hexlit_text(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS varchar) WITH FUNCTION mysql.hexlit_varchar(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS bpchar) WITH FUNCTION mysql.hexlit_bpchar(mysql.hexlit) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS varbit) WITH FUNCTION mysql.hexlit_varbit(mysql.hexlit, int4, bool) AS ASSIGNMENT;
CREATE CAST (mysql.hexlit AS bit) WITH FUNCTION mysql.hexlit_bit(mysql.hexlit, int4, bool) AS ASSIGNMENT;

-- bit operations on unsigned 64-bit integers, as MySQL's & | ^ << >> ~
CREATE FUNCTION mysql.mys_uint64_bitop(numeric, numeric, "char") RETURNS int8
    AS '$libdir/aux_mysql', 'mys_uint64_bitop' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.mys_uint64_bitnot(numeric) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_uint64_bitnot' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Literal against literal, BLOB, string: the bytes.  Literal against literal
-- uses bytea's functions and makes the btree and hash operator classes below
-- (UNION, DISTINCT, GREATEST); against bytea an inlined bytea comparison, so
-- that WHERE binary_col = X'..' uses an index.
DO $do$
DECLARE
    c text[];
    sel text;
BEGIN
    FOREACH c SLICE 1 IN ARRAY ARRAY[
            ['=', 'eq', 'byteaeq', '=', '<>', 'eqsel', 'eqjoinsel'],
            ['<>', 'ne', 'byteane', '<>', '=', 'neqsel', 'neqjoinsel'],
            ['<', 'lt', 'bytealt', '>', '>=', 'scalarltsel', 'scalarltjoinsel'],
            ['<=', 'le', 'byteale', '>=', '>', 'scalarlesel', 'scalarlejoinsel'],
            ['>', 'gt', 'byteagt', '<', '<=', 'scalargtsel', 'scalargtjoinsel'],
            ['>=', 'ge', 'byteage', '<=', '<', 'scalargesel', 'scalargejoinsel']] LOOP
        sel := format('RESTRICT = %s, JOIN = %s', c[6], c[7]);
        EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%s(mysql.hexlit, mysql.hexlit) RETURNS bool
            AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], c[3]);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.hexlit_%s, LEFTARG = mysql.hexlit, '
                       'RIGHTARG = mysql.hexlit, COMMUTATOR = OPERATOR(mysql.%s), NEGATOR = OPERATOR(mysql.%s), %s%s)',
                       c[1], c[2], c[4], c[5], sel,
                       CASE WHEN c[1] = '=' THEN ', HASHES, MERGES' ELSE '' END);
        -- a string, compared byte for byte (MySQL compares binary strings so)
        EXECUTE format($f$CREATE FUNCTION mysql.text_%s_hexlit(text, mysql.hexlit) RETURNS bool
            AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], c[3]);
        EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%s_text(mysql.hexlit, text) RETURNS bool
            AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], c[3]);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.text_%s_hexlit, LEFTARG = text, '
                       'RIGHTARG = mysql.hexlit, COMMUTATOR = OPERATOR(mysql.%s), %s)', c[1], c[2], c[4], sel);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.hexlit_%s_text, LEFTARG = mysql.hexlit, '
                       'RIGHTARG = text, COMMUTATOR = OPERATOR(mysql.%s), %s)', c[1], c[2], c[4], sel);
        EXECUTE format($f$CREATE FUNCTION mysql.bytea_%s_hexlit(bytea, mysql.hexlit) RETURNS bool
            LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
            AS 'SELECT $1 OPERATOR(pg_catalog.%s) $2::pg_catalog.bytea'$f$, c[2], c[1]);
        EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%s_bytea(mysql.hexlit, bytea) RETURNS bool
            LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
            AS 'SELECT $1::pg_catalog.bytea OPERATOR(pg_catalog.%s) $2'$f$, c[2], c[1]);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.bytea_%s_hexlit, LEFTARG = bytea, '
                       'RIGHTARG = mysql.hexlit, COMMUTATOR = OPERATOR(mysql.%s), %s)', c[1], c[2], c[4], sel);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.hexlit_%s_bytea, LEFTARG = mysql.hexlit, '
                       'RIGHTARG = bytea, COMMUTATOR = OPERATOR(mysql.%s), %s)', c[1], c[2], c[4], sel);
    END LOOP;
END
$do$;

CREATE FUNCTION mysql.hexlit_cmp(mysql.hexlit, mysql.hexlit) RETURNS int4
    AS 'byteacmp' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_hash(mysql.hexlit) RETURNS int4
    AS 'hashvarlena' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hexlit_hash_extended(mysql.hexlit, int8) RETURNS int8
    AS 'hashvarlenaextended' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS mysql.hexlit_ops DEFAULT FOR TYPE mysql.hexlit USING btree AS
    OPERATOR 1 mysql.<, OPERATOR 2 mysql.<=, OPERATOR 3 mysql.=, OPERATOR 4 mysql.>=,
    OPERATOR 5 mysql.>, FUNCTION 1 mysql.hexlit_cmp(mysql.hexlit, mysql.hexlit);
CREATE OPERATOR CLASS mysql.hexlit_hash_ops DEFAULT FOR TYPE mysql.hexlit USING hash AS
    OPERATOR 1 mysql.=, FUNCTION 1 mysql.hexlit_hash(mysql.hexlit),
    FUNCTION 2 mysql.hexlit_hash_extended(mysql.hexlit, int8);

-- LIKE with a literal on either side: binary (case-sensitive) matching
DO $do$
DECLARE
    c text[];
    t text;
BEGIN
    FOREACH c SLICE 1 IN ARRAY ARRAY[['~~', 'like', 'bytealike'], ['!~~', 'nlike', 'byteanlike']] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%s(mysql.hexlit, mysql.hexlit) RETURNS bool
            AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], c[3]);
        EXECUTE format('CREATE OPERATOR mysql.%s (FUNCTION = mysql.hexlit_%s, LEFTARG = mysql.hexlit, '
                       'RIGHTARG = mysql.hexlit)', c[1], c[2]);
        FOREACH t IN ARRAY ARRAY['text', 'bytea'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%2$s_%1$s_hexlit(%2$s, mysql.hexlit) RETURNS bool
                AS %3$L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], t, c[3]);
            EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%1$s_%2$s(mysql.hexlit, %2$s) RETURNS bool
                AS %3$L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, c[2], t, c[3]);
            EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.%3$s_%2$s_hexlit, '
                           'LEFTARG = %3$s, RIGHTARG = mysql.hexlit)', c[1], c[2], t);
            EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.hexlit_%2$s_%3$s, '
                           'LEFTARG = mysql.hexlit, RIGHTARG = %3$s)', c[1], c[2], t);
        END LOOP;
    END LOOP;
END
$do$;

-- Literal against a number (or a BIT column): the integer.  Arithmetic and
-- bit operations with a number, a BIT column, a string or a BLOB: the
-- integer, the other operand converted as MySQL does (a string or BLOB as a
-- DOUBLE).
DO $do$
DECLARE
    c text[];
    t text;
    num text;		-- the other operand as numeric
    res text;
    vol text;
    body text;
    nm text;
BEGIN
    -- comparisons: = <> < <= > >= with integers, DECIMAL, DOUBLE, BIT
    FOREACH c SLICE 1 IN ARRAY ARRAY[
            ['=', 'eq', '=', 'eqsel', 'eqjoinsel'],
            ['<>', 'ne', '<>', 'neqsel', 'neqjoinsel'],
            ['<', 'lt', '>', 'scalarltsel', 'scalarltjoinsel'],
            ['<=', 'le', '>=', 'scalarlesel', 'scalarlejoinsel'],
            ['>', 'gt', '<', 'scalargtsel', 'scalargtjoinsel'],
            ['>=', 'ge', '<=', 'scalargesel', 'scalargejoinsel']] LOOP
        FOREACH t IN ARRAY ARRAY['int2', 'int4', 'int8', 'numeric', 'float8', 'varbit'] LOOP
            IF t = 'float8' THEN
                body := 'SELECT mysql.hexlit_numeric(%1$s)::pg_catalog.float8 OPERATOR(pg_catalog.%3$s) %2$s';
            ELSIF t = 'varbit' THEN
                body := 'SELECT mysql.hexlit_numeric(%1$s) OPERATOR(pg_catalog.%3$s) mysql.varbit_numeric(%2$s)';
            ELSE
                body := 'SELECT mysql.hexlit_numeric(%1$s) OPERATOR(pg_catalog.%3$s) %2$s::pg_catalog.numeric';
            END IF;
            EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%1$s_%2$s(mysql.hexlit, %2$s) RETURNS bool
                LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS %3$L$f$,
                c[2], t, format(body, '$1', '$2', c[1]));
            EXECUTE format($f$CREATE FUNCTION mysql.%2$s_%1$s_hexlit(%2$s, mysql.hexlit) RETURNS bool
                LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS %3$L$f$,
                c[2], t, format(body, '$2', '$1', c[3]));
            EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.hexlit_%2$s_%3$s, LEFTARG = mysql.hexlit, '
                           'RIGHTARG = %3$s, COMMUTATOR = OPERATOR(mysql.%4$s), RESTRICT = %5$s, JOIN = %6$s)',
                           c[1], c[2], t, c[3], c[4], c[5]);
            EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.%3$s_%2$s_hexlit, LEFTARG = %3$s, '
                           'RIGHTARG = mysql.hexlit, COMMUTATOR = OPERATOR(mysql.%4$s), RESTRICT = %5$s, JOIN = %6$s)',
                           c[1], c[2], t, c[3], c[4], c[5]);
        END LOOP;
    END LOOP;

    -- arithmetic and bit operations
    FOREACH c SLICE 1 IN ARRAY ARRAY[
            ['+', 'pl', '+'], ['-', 'mi', '-'], ['*', 'mul', '*'], ['/', 'div', '/'], ['%', 'mod', '%'],
            ['&', 'and', '&'], ['|', 'or', '|'], ['^#', 'xor', '^'], ['<<', 'shl', '<'], ['>>', 'shr', '>']] LOOP
        FOREACH t IN ARRAY ARRAY['int2', 'int4', 'int8', 'numeric', 'float8', 'text', 'bytea', 'varbit',
                                 'mysql.hexlit'] LOOP
            vol := 'IMMUTABLE';
            num := CASE t WHEN 'mysql.hexlit' THEN 'mysql.hexlit_numeric(%s)'
                          WHEN 'varbit' THEN 'mysql.varbit_numeric(%s)'
                          WHEN 'text' THEN 'mysql.str_to_double(%s)::pg_catalog.numeric'
                          WHEN 'bytea' THEN 'mysql.str_to_double(%s::pg_catalog.text)::pg_catalog.numeric'
                          ELSE '%s::pg_catalog.numeric' END;
            IF t IN ('text', 'bytea') THEN
                vol := 'STABLE';
            END IF;
            IF c[2] IN ('and', 'or', 'xor', 'shl', 'shr') THEN
                res := 'int8';
                body := format('SELECT mysql.mys_uint64_bitop(%%1$s, %%2$s, %L)', c[3]);
            ELSIF t IN ('float8', 'text', 'bytea') THEN
                -- a DOUBLE operand (MySQL reads strings as DOUBLE): DOUBLE
                res := 'float8';
                num := replace(num, '::pg_catalog.numeric', '');
                IF t = 'float8' THEN
                    num := '%s';
                END IF;
                IF c[2] = 'div' THEN
                    body := 'SELECT mysql.mys_div_float8(%1$s::pg_catalog.float8, %2$s::pg_catalog.float8)';
                    vol := 'STABLE';
                ELSIF c[2] = 'mod' THEN
                    body := 'SELECT mysql.mys_mod_numeric(%1$s::pg_catalog.numeric, %2$s::pg_catalog.numeric)::pg_catalog.float8';
                    vol := 'STABLE';
                ELSE
                    body := format('SELECT %%1$s::pg_catalog.float8 OPERATOR(pg_catalog.%s) %%2$s::pg_catalog.float8', c[1]);
                END IF;
            ELSIF c[2] = 'div' THEN
                res := 'numeric';
                body := 'SELECT mysql.mys_div_numeric(%1$s, %2$s)';
                vol := 'STABLE';
            ELSIF c[2] = 'mod' THEN
                res := CASE t WHEN 'numeric' THEN 'numeric' ELSE 'int8' END;
                body := format('SELECT mysql.mys_mod_numeric(%%1$s, %%2$s)::pg_catalog.%s', res);
                vol := 'STABLE';
            ELSE
                res := CASE t WHEN 'numeric' THEN 'numeric' ELSE 'int8' END;
                body := format('SELECT (%%1$s OPERATOR(pg_catalog.%s) %%2$s)::pg_catalog.%s', c[1], res);
            END IF;
            nm := replace(t, 'mysql.', '');
            EXECUTE format($f$CREATE FUNCTION mysql.hexlit_%1$s_%2$s(mysql.hexlit, %3$s) RETURNS %4$s
                LANGUAGE sql %5$s STRICT PARALLEL SAFE AS %6$L$f$,
                c[2], nm, t, res, vol,
                format(body, 'mysql.hexlit_numeric($1)', format(num, '$2')));
            EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.hexlit_%2$s_%3$s, '
                           'LEFTARG = mysql.hexlit, RIGHTARG = %4$s)', c[1], c[2], nm, t);
            IF t <> 'mysql.hexlit' THEN
                EXECUTE format($f$CREATE FUNCTION mysql.%2$s_%1$s_hexlit(%3$s, mysql.hexlit) RETURNS %4$s
                    LANGUAGE sql %5$s STRICT PARALLEL SAFE AS %6$L$f$,
                    c[2], nm, t, res, vol,
                    format(body, format(num, '$1'), 'mysql.hexlit_numeric($2)'));
                EXECUTE format('CREATE OPERATOR mysql.%1$s (FUNCTION = mysql.%3$s_%2$s_hexlit, '
                               'LEFTARG = %4$s, RIGHTARG = mysql.hexlit)', c[1], c[2], nm, t);
            END IF;
        END LOOP;
    END LOOP;
END
$do$;

-- NULL-safe equality <=>, with the "=" operators above
DO $do$
DECLARE
    t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['mysql.hexlit', 'text', 'bytea', 'int2', 'int4', 'int8', 'numeric', 'float8', 'varbit'] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.hexlit_nseq_%1$s(mysql.hexlit, %2$s) RETURNS bool
            LANGUAGE sql IMMUTABLE PARALLEL SAFE AS %3$L$f$, replace(t, 'mysql.', ''), t,
            'SELECT CASE WHEN $1 IS NULL OR $2 IS NULL THEN $1 IS NULL AND $2 IS NULL '
            'ELSE $1 OPERATOR(mysql.=) $2 END');
        EXECUTE format('CREATE OPERATOR mysql.<=> (FUNCTION = mysql.hexlit_nseq_%1$s, '
                       'LEFTARG = mysql.hexlit, RIGHTARG = %2$s)', replace(t, 'mysql.', ''), t);
        IF t <> 'mysql.hexlit' THEN
            EXECUTE format($f$CREATE FUNCTION mysql.%1$s_nseq_hexlit(%2$s, mysql.hexlit) RETURNS bool
                LANGUAGE sql IMMUTABLE PARALLEL SAFE AS %3$L$f$, t, t,
                'SELECT CASE WHEN $1 IS NULL OR $2 IS NULL THEN $1 IS NULL AND $2 IS NULL '
                'ELSE $1 OPERATOR(mysql.=) $2 END');
            EXECUTE format('CREATE OPERATOR mysql.<=> (FUNCTION = mysql.%1$s_nseq_hexlit, '
                           'LEFTARG = %1$s, RIGHTARG = mysql.hexlit)', t);
        END IF;
    END LOOP;
END
$do$;

-- -0x10 = -16, ~0x0f = 18446744073709551600
CREATE FUNCTION mysql.hexlit_um(mysql.hexlit) RETURNS int8 LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
    AS 'SELECT (OPERATOR(pg_catalog.-) mysql.hexlit_numeric($1))::pg_catalog.int8';
CREATE OPERATOR mysql.- (FUNCTION = mysql.hexlit_um, RIGHTARG = mysql.hexlit);
CREATE FUNCTION mysql.hexlit_not(mysql.hexlit) RETURNS numeric LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.mys_uint64_bitnot(mysql.hexlit_numeric($1))';
CREATE OPERATOR mysql.~ (FUNCTION = mysql.hexlit_not, RIGHTARG = mysql.hexlit);
-- DIV
CREATE FUNCTION mysql.int_div(mysql.hexlit, numeric) RETURNS int8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.int_div(mysql.hexlit_numeric($1), $2)';
CREATE FUNCTION mysql.int_div(numeric, mysql.hexlit) RETURNS int8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.int_div($1, mysql.hexlit_numeric($2))';
CREATE FUNCTION mysql.int_div(mysql.hexlit, mysql.hexlit) RETURNS int8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.int_div(mysql.hexlit_numeric($1), mysql.hexlit_numeric($2))';

-- LOWER() / UPPER() leave a binary string as it is
CREATE FUNCTION mysql.lower(mysql.hexlit) RETURNS bytea
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT $1::pg_catalog.bytea';
CREATE FUNCTION mysql.upper(mysql.hexlit) RETURNS bytea
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT $1::pg_catalog.bytea';
-- IFNULL() of BLOBs, binary strings and literals (aux_mysql has it for other types)
CREATE FUNCTION mysql.ifnull(bytea, bytea) RETURNS bytea
    LANGUAGE sql IMMUTABLE PARALLEL SAFE AS 'SELECT coalesce($1, $2)';

-- ---------------------------------------------------------------------------
-- HEX(): two digits per byte of a string or BLOB (a literal is one); a
-- number rounded to a 64-bit integer, negative ones in two's complement
-- (HEX(-1) = 'FFFFFFFFFFFFFFFF'); a BIT column as a number.  UNHEX(): the
-- bytes, NULL with warning 1411 for a character that is not a digit.
-- ---------------------------------------------------------------------------
CREATE FUNCTION mysql.hex(text) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(bytea) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(int8) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_int8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(int4) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hex($1::pg_catalog.int8)';
CREATE FUNCTION mysql.hex(int2) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS 'SELECT mysql.hex($1::pg_catalog.int8)';
CREATE FUNCTION mysql.hex(numeric) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(float8) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_float8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(varbit) RETURNS text
    AS '$libdir/aux_mysql', 'mys_hex_varbit' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.hex(bool) RETURNS text
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT CASE WHEN $1 THEN '1' ELSE '0' END $$;
CREATE FUNCTION mysql.unhex(text) RETURNS bytea
    AS '$libdir/aux_mysql', 'mys_unhex' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.unhex(bytea) RETURNS bytea
    AS '$libdir/aux_mysql', 'mys_unhex' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.unhex(int8) RETURNS bytea
    LANGUAGE sql STABLE STRICT PARALLEL SAFE AS 'SELECT mysql.unhex($1::pg_catalog.text)';
