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

CREATE TYPE mysql.text_string;
CREATE FUNCTION mysql.text_string_in(cstring, oid, int4) RETURNS mysql.text_string
    AS '$libdir/aux_mysql', 'mys_text_string_in' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
CREATE FUNCTION mysql.text_string_out(mysql.text_string) RETURNS cstring
    AS 'byteaout' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.text_string_recv(internal, oid, int4) RETURNS mysql.text_string
    AS '$libdir/aux_mysql', 'mys_text_string_recv' LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
CREATE FUNCTION mysql.text_string_send(mysql.text_string) RETURNS bytea
    AS 'byteasend' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.text_string_typmodin(cstring[]) RETURNS int4
    AS '$libdir/aux_mysql', 'mys_text_string_typmodin' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.text_string_typmodout(int4) RETURNS cstring
    AS '$libdir/aux_mysql', 'mys_binary_typmodout' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE TYPE mysql.text_string (INPUT=mysql.text_string_in, OUTPUT=mysql.text_string_out,
    RECEIVE=mysql.text_string_recv, SEND=mysql.text_string_send,
    TYPMOD_IN=mysql.text_string_typmodin, TYPMOD_OUT=mysql.text_string_typmodout,
    LIKE=pg_catalog.bytea, COLLATABLE=true, CATEGORY='U');
COMMENT ON TYPE mysql.text_string IS 'MySQL character string, including U+0000; PG external text format uses bytea escaping';

DO $casts$
DECLARE t text; src text; cfun text;
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n', 'varbinary_n', 'text_string'] LOOP
        cfun := CASE t WHEN 'text_string' THEN 'mys_text_string_coerce' ELSE 'mys_binary_coerce' END;
        EXECUTE format('CREATE CAST (mysql.%I AS bytea) WITHOUT FUNCTION AS IMPLICIT', t);
        FOREACH src IN ARRAY ARRAY['bytea', 'text', 'varchar', 'mysql.hexlit',
                                   'mysql.binary_n', 'mysql.varbinary_n', 'mysql.text_string'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%1$I(%2$s, int4, bool) RETURNS mysql.%3$I
                AS '$libdir/aux_mysql', %4$L LANGUAGE C STABLE STRICT PARALLEL RESTRICTED$f$,
                t, src, t, cfun);
            EXECUTE format('CREATE CAST (%1$s AS mysql.%2$I) WITH FUNCTION mysql.%2$I(%1$s, int4, bool) AS ASSIGNMENT', src, t);
        END LOOP;
        -- Explicit only: native PG text cannot contain NUL or invalid encoding.
        EXECUTE format($f$CREATE FUNCTION mysql.%1$I(mysql.%2$I) RETURNS text
            AS '$libdir/aux_mysql', 'mys_hexlit_text' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$, t||'_text',t);
        EXECUTE format('CREATE CAST (mysql.%1$I AS text) WITH FUNCTION mysql.%2$I(mysql.%1$I)', t,t||'_text');
    END LOOP;
END $casts$;

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
-- dominates character; text_string dominates PG text. No bytea -> text cast.
CREATE FUNCTION mysql.concat_one("any") RETURNS text
    AS '$libdir/aux_mysql', 'mys_concat_one' LANGUAGE C STABLE STRICT PARALLEL SAFE;
DO $concat$
DECLARE t text; u text; rt text;
BEGIN
    FOREACH t IN ARRAY ARRAY['text','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n','mysql.text_string'] LOOP
        rt := CASE WHEN t='text' THEN 'text' WHEN t='mysql.text_string' THEN t ELSE 'bytea' END;
        EXECUTE format($f$CREATE FUNCTION mysql.concat_one(%s) RETURNS %s
            AS '$libdir/aux_mysql', 'mys_concat_one' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t,rt);
    END LOOP;
    FOREACH t IN ARRAY ARRAY['text','bytea','mysql.text_string'] LOOP
        FOREACH u IN ARRAY ARRAY['text','bytea','mysql.text_string'] LOOP
            rt := CASE WHEN 'bytea' IN (t,u) THEN 'bytea'
                       WHEN 'mysql.text_string' IN (t,u) THEN 'mysql.text_string' ELSE 'text' END;
            EXECUTE format($f$CREATE FUNCTION mysql.concat_pair(%s,%s) RETURNS %s
                AS 'byteacat' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t,u,rt);
        END LOOP;
    END LOOP;
END $concat$;

DO $functions$
DECLARE t text; rt text; fn text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n','mysql.text_string'] LOOP
        rt := CASE WHEN t='mysql.text_string' THEN t ELSE 'bytea' END;
        FOREACH fn IN ARRAY ARRAY['left','right'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,int4) RETURNS %s
                AS '$libdir/aux_mysql', %L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,
                fn,t,rt,CASE WHEN t='mysql.text_string' THEN 'mys_text_string_' ELSE 'mys_binary_' END||fn);
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
            IF t='mysql.text_string' AND fn IN ('char_length','character_length') THEN
                EXECUTE format($f$CREATE FUNCTION mysql.%I(%s) RETURNS int4
                    AS '$libdir/aux_mysql', 'mys_text_string_length' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t);
            ELSE
                EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s) RETURNS int4
                    AS 'byteaoctetlen' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t);
            END IF;
        END LOOP;
        IF t <> 'bytea' THEN
            EXECUTE format($f$CREATE FUNCTION mysql.hex(%s) RETURNS text
                AS '$libdir/aux_mysql', 'mys_hex_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t);
        END IF;
    END LOOP;
END $functions$;

-- Full-byte ordering for fixed/variable binary; character comparison uses
-- its collation and a length-aware comparator. Keep sortsupport disabled.
DO $ops$
DECLARE t text; c text[]; fn text;
BEGIN
    FOREACH t IN ARRAY ARRAY['binary_n','varbinary_n','text_string'] LOOP
        FOREACH c SLICE 1 IN ARRAY ARRAY[
            ['=','eq','byteaeq','=','eqsel','eqjoinsel'],['<>','ne','byteane','<>','neqsel','neqjoinsel'],
            ['<','lt','bytealt','>','scalarltsel','scalarltjoinsel'],['<=','le','byteale','>=','scalarlesel','scalarlejoinsel'],
            ['>','gt','byteagt','<','scalargtsel','scalargtjoinsel'],['>=','ge','byteage','<=','scalargesel','scalargejoinsel']] LOOP
            IF t='text_string' THEN
                EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS bool
                    AS '$libdir/aux_mysql', %L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,
                    t||'_'||c[2],t,t,'mys_text_string_'||c[2]);
            ELSE
                EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS bool
                    AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_'||c[2],t,t,c[3]);
            END IF;
            EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=mysql.%I, RIGHTARG=mysql.%I, FUNCTION=mysql.%I, COMMUTATOR=OPERATOR(mysql.%s), RESTRICT=%s, JOIN=%s%s)',c[1],t,t,t||'_'||c[2],c[4],c[5],c[6],CASE WHEN c[1]='=' THEN ', HASHES, MERGES' ELSE '' END);
        END LOOP;
        IF t='text_string' THEN
            EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS int4
                AS '$libdir/aux_mysql', 'mys_text_string_cmp' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_cmp',t,t);
        ELSE
            EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS int4
                AS 'byteacmp' LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,t||'_cmp',t,t);
        END IF;
        EXECUTE format('CREATE OPERATOR CLASS mysql.%I DEFAULT FOR TYPE mysql.%I USING btree %s AS
            OPERATOR 1 mysql.<, OPERATOR 2 mysql.<=, OPERATOR 3 mysql.=,
            OPERATOR 4 mysql.>=, OPERATOR 5 mysql.>, FUNCTION 1 mysql.%I(mysql.%I,mysql.%I)',t||'_ops',t,CASE WHEN t='text_string' THEN '' ELSE 'FAMILY pg_catalog.bytea_ops' END,t||'_cmp',t,t);
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
CREATE FUNCTION mysql.text_string_hash(mysql.text_string) RETURNS int4
    AS '$libdir/aux_mysql', 'mys_text_string_hash' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.text_string_hash_extended(mysql.text_string,int8) RETURNS int8
    AS '$libdir/aux_mysql', 'mys_text_string_hash' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR CLASS mysql.text_string_hash_ops DEFAULT FOR TYPE mysql.text_string USING hash AS
    OPERATOR 1 mysql.=, FUNCTION 1 mysql.text_string_hash(mysql.text_string),
    FUNCTION 2 mysql.text_string_hash_extended(mysql.text_string,int8);

DO $more$
DECLARE t text; fn text; rt text;
BEGIN
    FOREACH t IN ARRAY ARRAY['bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n','mysql.text_string'] LOOP
        rt := CASE WHEN t='mysql.text_string' THEN t ELSE 'bytea' END;
        -- REPLACE's character result is checked at the boundary as well:
        -- arbitrary binary substitutions cannot manufacture invalid TEXT.
        IF t <> 'mysql.text_string' THEN
            EXECUTE format($f$CREATE FUNCTION mysql.replace(%s,"any","any") RETURNS bytea
                AS '$libdir/aux_mysql', 'mys_string_replace' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,t);
        END IF;
        FOREACH fn IN ARRAY ARRAY['btrim','ltrim','rtrim'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.%I(%s) RETURNS %s
                AS '$libdir/aux_mysql', 'mys_string_trim' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
            IF t <> 'mysql.text_string' THEN
                EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,"any") RETURNS %s
                    AS '$libdir/aux_mysql', 'mys_string_trim' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
            END IF;
        END LOOP;
        FOREACH fn IN ARRAY ARRAY['lower','upper','lcase','ucase'] LOOP
            IF t='mysql.text_string' THEN
                EXECUTE format($f$CREATE FUNCTION mysql.%I(%s) RETURNS %s
                    AS '$libdir/aux_mysql', 'mys_text_string_case' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t,rt);
            ELSE
                EXECUTE format($f$CREATE OR REPLACE FUNCTION mysql.%I(%s) RETURNS bytea
                    AS '$libdir/aux_mysql', 'mys_raw_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,t);
            END IF;
        END LOOP;
    END LOOP;
END $more$;

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

-- Common character contexts keep the new TEXT type, including comparisons
-- to existing VARCHAR/TEXT columns. Binary comparisons continue to use bytes.
DO $character$
DECLARE c text[]; cond text; a text; b text;
BEGIN
    FOREACH c SLICE 1 IN ARRAY ARRAY[
        ['=','eq'],['<>','ne'],['<','lt'],['<=','le'],['>','gt'],['>=','ge']] LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.%I(mysql.text_string,text) RETURNS bool
            LANGUAGE sql STABLE STRICT PARALLEL SAFE
            AS %L$f$, 'text_string_'||c[2]||'_text',
            format('SELECT $1 OPERATOR(mysql.%s) $2::mysql.text_string',c[1]));
        EXECUTE format($f$CREATE FUNCTION mysql.%I(text,mysql.text_string) RETURNS bool
            LANGUAGE sql STABLE STRICT PARALLEL SAFE
            AS %L$f$, 'text_'||c[2]||'_text_string',
            format('SELECT $1::mysql.text_string OPERATOR(mysql.%s) $2',c[1]));
        EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=mysql.text_string,RIGHTARG=text,FUNCTION=mysql.%I)',c[1],'text_string_'||c[2]||'_text');
        EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=text,RIGHTARG=mysql.text_string,FUNCTION=mysql.%I)',c[1],'text_'||c[2]||'_text_string');
    END LOOP;
    FOREACH a IN ARRAY ARRAY['mysql.text_string','text'] LOOP
        FOREACH b IN ARRAY ARRAY['mysql.text_string','text'] LOOP
            IF a='text' AND b='text' THEN CONTINUE; END IF;
            EXECUTE format($f$CREATE FUNCTION mysql.ifnull(%s,%s) RETURNS mysql.text_string
                LANGUAGE sql STABLE PARALLEL SAFE
                AS 'SELECT coalesce($1::mysql.text_string,$2::mysql.text_string)'$f$,a,b);
            FOREACH cond IN ARRAY ARRAY['bool','int2','int4','int8','numeric','float8','text'] LOOP
                EXECUTE format($f$CREATE FUNCTION mysql.if(%s,%s,%s) RETURNS mysql.text_string
                    LANGUAGE sql STABLE PARALLEL RESTRICTED
                    AS 'SELECT CASE WHEN mysql.binary_truth($1) THEN $2::mysql.text_string ELSE $3::mysql.text_string END'$f$,cond,a,b);
            END LOOP;
        END LOOP;
    END LOOP;
    FOREACH a IN ARRAY ARRAY['mysql.text_string','text'] LOOP
        FOREACH b IN ARRAY ARRAY['mysql.text_string','text'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.replace(mysql.text_string,%s,%s) RETURNS mysql.text_string
                AS '$libdir/aux_mysql','mys_string_replace' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,a,b);
        END LOOP;
    END LOOP;
END $character$;

-- Exact text_string overloads avoid both unknown-argument ambiguity and
-- accidental bytea matching after the implicit binary-compatible cast.
DO $string_search$
DECLARE a text; b text; c text[]; fn text; impl text;
BEGIN
    FOREACH a IN ARRAY ARRAY['text','mysql.text_string','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
        FOREACH b IN ARRAY ARRAY['text','mysql.text_string','bytea','mysql.hexlit','mysql.binary_n','mysql.varbinary_n'] LOOP
            IF a='text' AND b='text' THEN CONTINUE; END IF;
            EXECUTE format($f$CREATE FUNCTION mysql.locate(%s,%s) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,a,b);
            EXECUTE format($f$CREATE FUNCTION mysql.locate(%s,%s,int4) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE PARALLEL SAFE$f$,a,b);
            EXECUTE format($f$CREATE FUNCTION mysql.instr(%s,%s) RETURNS int4
                AS '$libdir/aux_mysql','mys_string_instr' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,a,b);
            IF a<>'mysql.text_string' AND b<>'mysql.text_string' THEN CONTINUE; END IF;
            FOREACH c SLICE 1 IN ARRAY ARRAY[
                ['~','regex'],['~*','iregex'],['!~','nregex'],['!~*','niregex'],
                ['~~','like'],['!~~','nlike']] LOOP
                fn := replace(a,'mysql.','') || '_' || c[2] || '_' || replace(b,'mysql.','');
                IF c[2] IN ('like','nlike') THEN
                    -- PG18's LIKE engine walks explicit byte/character
                    -- lengths (including ICU). It never converts to C strings.
                    impl := CASE WHEN a IN ('text','mysql.text_string') AND b IN ('text','mysql.text_string')
                                 THEN 'text' ELSE 'bytea' END || c[2];
                    EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,%s) RETURNS bool
                        AS %L LANGUAGE internal IMMUTABLE STRICT PARALLEL SAFE$f$,fn,a,b,impl);
                ELSE
                    EXECUTE format($f$CREATE FUNCTION mysql.%I(%s,%s) RETURNS bool
                        AS '$libdir/aux_mysql',%L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE$f$,fn,a,b,'mys_string_'||c[2]);
                END IF;
                EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=%s,RIGHTARG=%s,FUNCTION=mysql.%I)',c[1],a,b,fn);
            END LOOP;
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
