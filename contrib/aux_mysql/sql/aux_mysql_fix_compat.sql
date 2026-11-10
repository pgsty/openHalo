-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Final 1.9 release corrections, shared by fresh installs and upgrades.
SET LOCAL search_path = pg_catalog, mysql, public;

-- Keep the generic array identity: a bool[] overload alone makes integer[]
-- resolution ambiguous against the existing polymorphic function.
CREATE OR REPLACE FUNCTION mysql.array_to_string_for_mysql(anyarray,text) RETURNS text
    AS '$libdir/aux_mysql','mys_group_concat_binary' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- FIND_IN_SET uses the caller's collation, but unlike ordinary equality it
-- must not pad comma-separated items with spaces. Check both directions,
-- including the bigint overload (e.g. FIND_IN_SET(1, '1 ')).
CREATE OR REPLACE FUNCTION mysql.find_in_set(text,text) RETURNS integer
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$
    SELECT coalesce(min(pos), 0)::integer
    FROM pg_catalog.unnest(pg_catalog.string_to_array($2, ',')) WITH ORDINALITY AS items(item,pos)
    WHERE item = $1
      AND pg_catalog.octet_length(item) - pg_catalog.octet_length(pg_catalog.rtrim(item, ' '))
        = pg_catalog.octet_length($1) - pg_catalog.octet_length(pg_catalog.rtrim($1, ' '))
$$;
CREATE OR REPLACE FUNCTION mysql.find_in_set(bigint,text) RETURNS integer
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$
    SELECT mysql.find_in_set($1::text, $2)
$$;

-- Preserve CHAR's logical value before combining it with VARCHAR. TEXT
-- pairs also give unknown string literals an unambiguous preferred type;
-- adding only CHAR/VARCHAR pairs breaks IFNULL(varchar_column, '').
-- Generate the same condition types as existing IF, including bool.
DO $conditional_strings$
DECLARE t text; pair text[];
BEGIN
    FOREACH pair SLICE 1 IN ARRAY ARRAY[['bpchar','varchar'],['varchar','bpchar'],
        ['bpchar','text'],['text','bpchar'],['varchar','text'],['text','varchar']]
    LOOP
        EXECUTE format($f$CREATE FUNCTION mysql.ifnull(%s,%s) RETURNS varchar
            LANGUAGE sql STABLE PARALLEL SAFE AS 'SELECT coalesce(mysql.concat_one($1)::varchar, mysql.concat_one($2)::varchar)'$f$,pair[1],pair[2]);
    END LOOP;
    FOR t IN SELECT DISTINCT pg_catalog.format_type(p.proargtypes[0], NULL)
        FROM pg_catalog.pg_proc p
        WHERE p.pronamespace = 'mysql'::regnamespace AND p.proname = 'if'
          AND p.pronargs = 3 AND p.proargtypes[1] = 'bpchar'::regtype
          AND p.proargtypes[2] = 'bpchar'::regtype
    LOOP
        FOREACH pair SLICE 1 IN ARRAY ARRAY[['bpchar','varchar'],['varchar','bpchar'],
            ['bpchar','text'],['text','bpchar'],['varchar','text'],['text','varchar']]
        LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.if(%s,%s,%s) RETURNS varchar
                LANGUAGE sql STABLE PARALLEL SAFE AS 'SELECT CASE WHEN mysql.binary_truth($1) THEN mysql.concat_one($2)::varchar ELSE mysql.concat_one($3)::varchar END'$f$,
                t,pair[1],pair[2]);
        END LOOP;
        EXECUTE format($f$CREATE FUNCTION mysql.if(%s,bool,bool) RETURNS bool
            LANGUAGE sql STABLE PARALLEL SAFE AS 'SELECT CASE WHEN mysql.binary_truth($1) THEN $2 ELSE $3 END'$f$,t);
    END LOOP;
END $conditional_strings$;

-- MySQL booleans are the integers 1 and 0. PG has no boolean casts to
-- numeric, floating point or binary strings, so a predicate, IF() or
-- TRUE/FALSE assigned to DECIMAL, FLOAT/DOUBLE, BINARY/VARBINARY or BLOB
-- failed. Assignment casts, like aux_mysql.sql's int2/int8 ones, also serve CAST,
-- DEFAULT and generated columns; they are not implicit, so function and
-- operator resolution is unchanged. Domains (DOUBLE, REAL, BLOB, ...) use
-- their base type's cast.
CREATE FUNCTION mysql.cast_boolean_to_numeric(pg_catalog.bool) RETURNS pg_catalog.numeric
    AS '$libdir/aux_mysql','mys_bool_numeric' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_boolean_to_float4(pg_catalog.bool) RETURNS pg_catalog.float4
    AS '$libdir/aux_mysql','mys_bool_float4' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_boolean_to_float8(pg_catalog.bool) RETURNS pg_catalog.float8
    AS '$libdir/aux_mysql','mys_bool_float8' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_boolean_to_bytea(pg_catalog.bool) RETURNS pg_catalog.bytea
    AS '$libdir/aux_mysql','mys_bool_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_boolean_to_binary_n(pg_catalog.bool) RETURNS mysql.binary_n
    AS '$libdir/aux_mysql','mys_bool_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.cast_boolean_to_varbinary_n(pg_catalog.bool) RETURNS mysql.varbinary_n
    AS '$libdir/aux_mysql','mys_bool_bytes' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
DO $bool_casts$
DECLARE c text[];
BEGIN
    FOREACH c SLICE 1 IN ARRAY ARRAY[['pg_catalog.numeric','numeric'],
        ['pg_catalog.float4','float4'], ['pg_catalog.float8','float8'],
        ['pg_catalog.bytea','bytea'], ['mysql.binary_n','binary_n'],
        ['mysql.varbinary_n','varbinary_n']]
    LOOP
        IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_cast
                       WHERE castsource = 'pg_catalog.bool'::regtype AND casttarget = c[1]::regtype) THEN
            EXECUTE format('CREATE CAST (pg_catalog.bool AS %s) WITH FUNCTION mysql.cast_boolean_to_%s(pg_catalog.bool) AS ASSIGNMENT',
                           c[1], c[2]);
        ELSE
            RAISE NOTICE 'aux_mysql: keeping the existing cast (pg_catalog.bool AS %)', c[1];
        END IF;
    END LOOP;
END $bool_casts$;

-- Keep the one-argument identity for clients and pre-upgrade SQL catalogs.
CREATE OR REPLACE FUNCTION mysql.set_system_names_variable(text) RETURNS void
    AS '$libdir/aux_mysql','mys_set_names' LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;
CREATE FUNCTION mysql.set_system_names_variable(text,text) RETURNS void
    AS '$libdir/aux_mysql','mys_set_names' LANGUAGE C VOLATILE PARALLEL UNSAFE;
CREATE FUNCTION mysql.set_system_charset_variable(text) RETURNS void
    AS '$libdir/aux_mysql','mys_set_charset' LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE;
