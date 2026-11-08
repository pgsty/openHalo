-- Hand-written 1.8 -> 1.9 fragment. Append after the 1.8 binary objects.
-- Do not drop the old binary/varbinary domains: existing dependencies survive.
-- Extension scripts must use PostgreSQL concatenation, not MySQL's || (OR).
SET LOCAL search_path = pg_catalog, mysql, public;

DO $types$
DECLARE t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n', 'varbinary_n'] LOOP
        EXECUTE format('CREATE TYPE mysql.%I', t);
        EXECUTE format($f$CREATE FUNCTION mysql.%1$I(cstring, oid, int4) RETURNS mysql.%2$I
            AS '$libdir/aux_mysql', 'mys_binary_in' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED$f$, t || '_in', t);
        EXECUTE format($f$CREATE FUNCTION mysql.%1$I(mysql.%2$I) RETURNS cstring
            AS 'byteaout' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, t || '_out', t);
        EXECUTE format($f$CREATE FUNCTION mysql.%1$I(internal, oid, int4) RETURNS mysql.%2$I
            AS '$libdir/aux_mysql', 'mys_binary_recv' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED$f$, t || '_recv', t);
        EXECUTE format($f$CREATE FUNCTION mysql.%1$I(mysql.%2$I) RETURNS bytea
            AS 'byteasend' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$, t || '_send', t);
        EXECUTE format($f$CREATE FUNCTION mysql.%I(cstring[]) RETURNS int4
            AS '$libdir/aux_mysql', 'mys_binary_typmodin' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$, t || '_typmodin');
        EXECUTE format($f$CREATE FUNCTION mysql.%I(int4) RETURNS cstring
            AS '$libdir/aux_mysql', 'mys_binary_typmodout' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$, t || '_typmodout');
        EXECUTE format('CREATE TYPE mysql.%1$I (INPUT=mysql.%2$I, OUTPUT=mysql.%3$I,
            RECEIVE=mysql.%4$I, SEND=mysql.%5$I, TYPMOD_IN=mysql.%6$I, TYPMOD_OUT=mysql.%7$I,
            LIKE=pg_catalog.bytea, CATEGORY=''U'')', t,t||'_in',t||'_out',t||'_recv',t||'_send',t||'_typmodin',t||'_typmodout');
    END LOOP;
END $types$;

DO $casts$
DECLARE t text; src text; dst text; cfun text;
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n', 'varbinary_n'] LOOP
        cfun := 'mys_binary_coerce';
        EXECUTE format('CREATE CAST (mysql.%I AS bytea) WITHOUT FUNCTION AS IMPLICIT', t);
        FOREACH src IN ARRAY ARRAY['bytea', 'text', 'varchar', 'bpchar', 'mysql.hexlit',
                                   'mysql.binary_n', 'mysql.varbinary_n'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%1$I(%2$s, int4, bool) RETURNS mysql.%3$I
                AS '$libdir/aux_mysql', %4$L LANGUAGE C STABLE STRICT PARALLEL RESTRICTED$f$,
                t, src, t, cfun);
            EXECUTE format('CREATE CAST (%1$s AS mysql.%2$I) WITH FUNCTION mysql.%2$I(%1$s, int4, bool) AS ASSIGNMENT', src, t);
        END LOOP;
        -- Assignment casts prevent CoerceViaIO from exposing byteaout's
        -- escape syntax. Keep these out of implicit operator resolution.
        FOREACH dst IN ARRAY ARRAY['text','varchar','bpchar'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%1$I(mysql.%2$I) RETURNS %3$s
                AS '$libdir/aux_mysql', 'mys_hexlit_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,
                t||'_'||dst,t,dst);
            EXECUTE format('CREATE CAST (mysql.%1$I AS %2$s) WITH FUNCTION mysql.%3$I(mysql.%1$I) AS ASSIGNMENT',
                t,dst,t||'_'||dst);
        END LOOP;
    END LOOP;
END $casts$;

-- Only the MySQL CAST/CONVERT grammar calls these overloads. In particular,
-- the native PG bytea -> int8 cast must keep its binary-integer semantics.
CREATE FUNCTION mysql.cast_numeric_source(anyelement,int4) RETURNS anyelement
    AS '$libdir/aux_mysql','mys_cast_source_identity' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_numeric_source(text,int4) RETURNS text
    AS '$libdir/aux_mysql','mys_cast_source_identity' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_numeric_source(mysql.hexlit,int4) RETURNS mysql.hexlit
    AS '$libdir/aux_mysql','mys_cast_source_identity' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
DO $numeric_sources$
DECLARE t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.binary_n','mysql.varbinary_n'] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.cast_numeric_source(%s,int4) RETURNS numeric
            AS '$libdir/aux_mysql','mys_cast_binary_number' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED$f$,t);
    END LOOP;
END $numeric_sources$;

CREATE FUNCTION mysql.hexlit_cmp_support(internal) RETURNS internal
    AS '$libdir/aux_mysql', 'mys_hexlit_cmp_support' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
DO $support$
DECLARE t text; op text;
BEGIN
    FOREACH t IN ARRAY ARRAY['int2','int4','int8'] LOOP
        FOREACH op IN ARRAY ARRAY['eq','ne','lt','le','gt','ge'] LOOP
            EXECUTE format('ALTER FUNCTION mysql.%I(%s,mysql.hexlit) SUPPORT mysql.hexlit_cmp_support',t||'_'||op||'_hexlit',t);
            EXECUTE format('ALTER FUNCTION mysql.%I(mysql.hexlit,%s) SUPPORT mysql.hexlit_cmp_support','hexlit_'||op||'_'||t,t);
        END LOOP;
    END LOOP;
END $support$;

-- Type resolution takes place at every concat_one/concat_pair node. Binary
-- dominates character. No bytea -> text cast.
CREATE FUNCTION mysql.concat_one("any") RETURNS text
    AS '$libdir/aux_mysql', 'mys_concat_one' LANGUAGE C STABLE STRICT PARALLEL SAFE;
DO $concat$
DECLARE t text; u text; rt text;
BEGIN
    FOREACH t IN ARRAY ARRAY['text','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        rt := CASE WHEN t='text' THEN 'text' ELSE 'bytea' END;
        EXECUTE format($f$CREATE FUNCTION mysql.concat_one(%s) RETURNS %s
            AS '$libdir/aux_mysql', 'mys_concat_one' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t,rt);
    END LOOP;
    FOREACH t IN ARRAY ARRAY['text','bytea'] LOOP
        FOREACH u IN ARRAY ARRAY['text','bytea'] LOOP
            rt := CASE WHEN 'bytea' IN (t,u) THEN 'bytea' ELSE 'text' END;
            EXECUTE format($f$CREATE FUNCTION mysql.concat_pair(%s,%s) RETURNS %s
                AS 'byteacat' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t,u,rt);
        END LOOP;
    END LOOP;
END $concat$;

DO $functions$
DECLARE t text; rt text; fn text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        rt := 'bytea';
        FOREACH fn IN ARRAY ARRAY['left','right'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,int4) RETURNS %s
                AS '$libdir/aux_mysql', %L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,
                fn,t,rt,'mys_binary_'||fn);
        END LOOP;
        FOREACH fn IN ARRAY ARRAY['substr','substring'] LOOP
            EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s,int4,int4) RETURNS %s
                AS '$libdir/aux_mysql', 'mys_string_substr' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
            EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s,int4) RETURNS %s
                AS '$libdir/aux_mysql', 'mys_string_substr' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
        END LOOP;
        EXECUTE format($f$CREATE FUNCTION mysql.ascii(%s) RETURNS int4
            AS '$libdir/aux_mysql', 'mys_binary_ascii' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t);
        FOREACH fn IN ARRAY ARRAY['length','octet_length','char_length','character_length'] LOOP
                EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s) RETURNS int4
                    AS 'byteaoctetlen' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t);
        END LOOP;
        IF t <> 'bytea' THEN
            EXECUTE format($f$CREATE FUNCTION mysql.hex(%s) RETURNS text
                AS '$libdir/aux_mysql', 'mys_hex_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t);
        END IF;
    END LOOP;
END $functions$;

-- Full-byte ordering for fixed/variable binary.
DO $ops$
DECLARE t text; c text[];
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n','varbinary_n'] LOOP
        FOREACH c SLICE 1 IN ARRAY ARRAY[
            ['=','eq','byteaeq','=','eqsel','eqjoinsel'],['<>','ne','byteane','<>','neqsel','neqjoinsel'],
            ['<','lt','bytealt','>','scalarltsel','scalarltjoinsel'],['<=','le','byteale','>=','scalarlesel','scalarlejoinsel'],
            ['>','gt','byteagt','<','scalargtsel','scalargtjoinsel'],['>=','ge','byteage','<=','scalargesel','scalargejoinsel']] LOOP
                EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS bool
                    AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_'||c[2],t,t,c[3]);
            EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=mysql.%I, RIGHTARG=mysql.%I, FUNCTION=mysql.%I, COMMUTATOR=OPERATOR(mysql.%s), RESTRICT=%s, JOIN=%s%s)',c[1],t,t,t||'_'||c[2],c[4],c[5],c[6],CASE WHEN c[1]='=' THEN ', HASHES, MERGES' ELSE '' END);
        END LOOP;
            EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS int4
                AS 'byteacmp' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_cmp',t,t);
        EXECUTE format('CREATE OPERATOR CLASS mysql.%I DEFAULT FOR TYPE mysql.%I USING btree %s AS
            OPERATOR 1 mysql.<, OPERATOR 2 mysql.<=, OPERATOR 3 mysql.=,
            OPERATOR 4 mysql.>=, OPERATOR 5 mysql.>, FUNCTION 1 mysql.%I(mysql.%I,mysql.%I)',t||'_ops',t,'FAMILY pg_catalog.bytea_ops',t||'_cmp',t,t);
    END LOOP;
END $ops$;

-- A binary-compatible cast lets a bytea search key use these indexes, but
-- btree still asks for comparator (index input type, search-key type).
-- Supply every mixed pair; same-type members were registered above.
DO $binary_cross_cmp$
DECLARE a text; b text; fn text;
BEGIN
    FOREACH a IN ARRAY ARRAY['bytea','mysql.binary_n','mysql.varbinary_n'] LOOP
        FOREACH b IN ARRAY ARRAY['bytea','mysql.binary_n','mysql.varbinary_n'] LOOP
            IF a=b THEN CONTINUE; END IF;
            fn := replace(a,'mysql.','') || '_cmp_' || replace(b,'mysql.','');
            EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,%s) RETURNS int4
                AS 'byteacmp' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,fn,a,b);
            EXECUTE format('ALTER OPERATOR FAMILY pg_catalog.bytea_ops USING btree
                ADD FUNCTION 1 (%s,%s) mysql.%I(%s,%s)',a,b,fn,a,b);
        END LOOP;
    END LOOP;
END $binary_cross_cmp$;

CREATE FUNCTION mysql.raw_bytes("any") RETURNS bytea
    AS '$libdir/aux_mysql', 'mys_raw_bytes' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.binary_truth("any") RETURNS bool
    AS '$libdir/aux_mysql', 'mys_binary_truth' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
DO $more$
DECLARE t text; fn text; rt text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        rt := 'bytea';
            EXECUTE format($f$CREATE FUNCTION mysql.replace(%s,"any","any") RETURNS bytea
                AS '$libdir/aux_mysql', 'mys_string_replace' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t);
        FOREACH fn IN ARRAY ARRAY['btrim','ltrim','rtrim'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%I(%s) RETURNS %s
                AS '$libdir/aux_mysql', 'mys_string_trim' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
                EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,"any") RETURNS %s
                    AS '$libdir/aux_mysql', 'mys_string_trim' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
        END LOOP;
        FOREACH fn IN ARRAY ARRAY['lower','upper','lcase','ucase'] LOOP
                EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s) RETURNS bytea
                    AS '$libdir/aux_mysql', 'mys_raw_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t);
        END LOOP;
    END LOOP;
END $more$;

-- MySQL REPLACE is case-sensitive even under a case-insensitive collation.
CREATE FUNCTION mysql.replace(text,text,text) RETURNS text
    AS '$libdir/aux_mysql', 'mys_string_replace' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

DO $binary_hash$
DECLARE t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n','varbinary_n'] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I) RETURNS int4
            AS 'hashvarlena' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_hash',t);
        EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,int8) RETURNS int8
            AS 'hashvarlenaextended' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_hash_extended',t);
        EXECUTE format('CREATE OPERATOR CLASS mysql.%I DEFAULT FOR TYPE mysql.%I USING hash
            FAMILY pg_catalog.bytea_ops AS OPERATOR 1 mysql.=,
            FUNCTION 1 mysql.%I(mysql.%I), FUNCTION 2 mysql.%I(mysql.%I,int8)',
            t||'_hash_ops',t,t||'_hash',t,t||'_hash_extended',t);
    END LOOP;
END $binary_hash$;

DO $string_search$
DECLARE a text; b text;
BEGIN
    FOREACH a IN ARRAY ARRAY['text','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        FOREACH b IN ARRAY ARRAY['text','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
            IF a='text' AND b='text' THEN CONTINUE; END IF;
            EXECUTE format($f$CREATE FUNCTION mysql.locate(%s,%s) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,a,b);
            EXECUTE format($f$CREATE FUNCTION mysql.locate(%s,%s,int4) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE PARALLEL SAFE$f$,a,b);
            EXECUTE format($f$CREATE FUNCTION mysql.instr(%s,%s) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_instr' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,a,b);
        END LOOP;
    END LOOP;
END $string_search$;


-- Typed result helpers used by the MySQL raw-parser lowering of CONCAT_WS
-- and ELT. The NULL first argument fixes the result OID before its parent
-- expression is analyzed; bytea data never masquerades as native PG text.
CREATE FUNCTION mysql.string_result(anyelement, "any") RETURNS anyelement
    AS '$libdir/aux_mysql', 'mys_string_result' LANGUAGE C STABLE PARALLEL SAFE;
CREATE FUNCTION mysql.concat_ws_bytes(VARIADIC "any") RETURNS bytea
    AS '$libdir/aux_mysql', 'mys_concat_ws_bytes' LANGUAGE C STABLE PARALLEL SAFE;
CREATE FUNCTION mysql.elt_index("any") RETURNS int4
    AS '$libdir/aux_mysql', 'mys_elt_index' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;

-- Used only by the MySQL grammar; native PostgreSQL CAST is unchanged.
CREATE FUNCTION mysql.cast_char("any",int4) RETURNS text
    AS '$libdir/aux_mysql','mys_cast_char' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.convert_using("any",text) RETURNS text
    AS '$libdir/aux_mysql','mys_convert_using' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- GROUP_CONCAT arrays preserve their element payload and binary result type.
-- Include the old bytea domains: upgraded application columns keep those OIDs.
DO $group_concat$
DECLARE t text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.blob','mysql.binary','mysql.varbinary',
                             'mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.array_to_string_for_mysql(%s[],text) RETURNS bytea
            AS '$libdir/aux_mysql','mys_group_concat_binary' LANGUAGE C STABLE STRICT PARALLEL SAFE$f$,t);
    END LOOP;
END $group_concat$;

-- CHAR arguments observe the writer/session PAD_CHAR_TO_FULL_LENGTH setting.
CREATE FUNCTION mysql.hex(bpchar) RETURNS text
    AS '$libdir/aux_mysql','mys_hex_bytes' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.array_to_string_for_mysql(bpchar[],text) RETURNS text
    AS '$libdir/aux_mysql','mys_group_concat_binary' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.concat_one(bpchar) RETURNS text
    AS '$libdir/aux_mysql','mys_concat_one' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.array_to_string_for_mysql(text[],text) RETURNS text
    AS '$libdir/aux_mysql','mys_group_concat_binary' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.array_to_string_for_mysql(varchar[],text) RETURNS text
    AS '$libdir/aux_mysql','mys_group_concat_binary' LANGUAGE C STABLE STRICT PARALLEL SAFE;
