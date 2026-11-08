-- aux_mysql 1.9 integration fragment. Append after the generated SQL and the
-- binary type definitions. Compile src/mys_prefix.c and call mys_prefix_init()
-- from _PG_init. Do not change existing collations in place: old indexes must
-- continue using their original comparison rules until explicitly migrated.
CREATE COLLATION mysql.utf8mb4_general_ci
    (provider = icu, locale = 'und-u-ks-level1', deterministic = false);
COMMENT ON COLLATION mysql.utf8mb4_general_ci IS
    'MySQL 5.7 simple general_ci weights and PAD SPACE; aux_mysql comparison methods';
CREATE COLLATION mysql.utf8mb4_bin
    (provider = icu, locale = 'und', deterministic = false);
COMMENT ON COLLATION mysql.utf8mb4_bin IS
    'MySQL UTF-8 codepoint order and PAD SPACE; aux_mysql comparison methods';

CREATE FUNCTION mysql.prefix_handler(internal) RETURNS index_am_handler
    AS '$libdir/aux_mysql', 'mys_prefix_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD mysql_prefix TYPE INDEX HANDLER mysql.prefix_handler;
CREATE FUNCTION mysql.prefix_options(internal) RETURNS void
    AS '$libdir/aux_mysql', 'mys_prefix_options' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION mysql.prefix_like(text,text) RETURNS boolean
    AS '$libdir/aux_mysql', 'mys_prefix_like' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.prefix_not_like(text,text) RETURNS boolean
    AS '$libdir/aux_mysql', 'mys_prefix_not_like' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.prefix_like(bpchar,text) RETURNS boolean
    AS '$libdir/aux_mysql', 'mys_prefix_char_like' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.prefix_not_like(bpchar,text) RETURNS boolean
    AS '$libdir/aux_mysql', 'mys_prefix_char_not_like' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OPERATOR mysql.~#~ (LEFTARG=text, RIGHTARG=text, FUNCTION=mysql.prefix_like,
    RESTRICT=likesel, JOIN=likejoinsel);
CREATE OPERATOR mysql.!~#~ (LEFTARG=text, RIGHTARG=text, FUNCTION=mysql.prefix_not_like,
    RESTRICT=nlikesel, JOIN=nlikejoinsel);
CREATE OPERATOR mysql.~#~ (LEFTARG=bpchar, RIGHTARG=text, FUNCTION=mysql.prefix_like,
    RESTRICT=likesel, JOIN=likejoinsel);
CREATE OPERATOR mysql.!~#~ (LEFTARG=bpchar, RIGHTARG=text, FUNCTION=mysql.prefix_not_like,
    RESTRICT=nlikesel, JOIN=nlikejoinsel);

-- Register an independently defined, bytea-layout binary type after its own
-- btree operators exist. The binary worker can call this for each new type.
-- Ordinary scalar opclasses are copied as independent families; nothing is
-- added to PostgreSQL's existing operator classes or families.
CREATE FUNCTION mysql.register_prefix_type(t regtype, string_storage boolean)
RETURNS void LANGUAGE plpgsql SET search_path = pg_catalog AS $body$
DECLARE
    c record; p record; rhs regtype;
    statements text[] := '{}'; family_name text;
BEGIN
    SELECT opc.* INTO STRICT c FROM pg_opclass opc JOIN pg_am am ON am.oid=opc.opcmethod
        WHERE am.amname='btree' AND opc.opcdefault AND opc.opcintype=t;
    -- Builtin OIDs are stable; extension type OIDs change on restore. Explicit
    -- opclass options make pg_dump print the name, so user-type names must be
    -- stable across databases. This digest names catalog objects only; it is
    -- never stored as a key or used to decide data equality.
    IF t::oid < 16384 THEN
        family_name := 'prefix_' || t::oid;
    ELSE
        SELECT 'prefix_' || md5(format('%I.%I', ns.nspname, typ.typname))
            INTO family_name FROM pg_type typ JOIN pg_namespace ns ON ns.oid=typ.typnamespace
            WHERE typ.oid=t;
    END IF;
    IF EXISTS (SELECT FROM pg_opclass opc JOIN pg_am am ON am.oid=opc.opcmethod
               WHERE am.amname='mysql_prefix' AND opc.opcintype=t) THEN RETURN; END IF;
    -- regoperator includes argument types; assemble names without that suffix.
    SELECT array_agg(format('OPERATOR %s %I.%s (%s, %s)', a.amopstrategy,
                           ns.nspname, op.oprname, t, t) ORDER BY a.amopstrategy)
        INTO statements FROM pg_amop a JOIN pg_operator op ON op.oid=a.amopopr
        JOIN pg_namespace ns ON ns.oid=op.oprnamespace
        WHERE a.amopfamily=c.opcfamily AND a.amoplefttype=t AND a.amoprighttype=t
              AND a.amopstrategy BETWEEN 1 AND 5;
    IF string_storage THEN
        EXECUTE format('CREATE FUNCTION mysql.%I(%s,%s) RETURNS int4 '
                       'AS %L,%L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE',
                       family_name || '_cmp', t, t, '$libdir/aux_mysql', 'mys_prefix_compare');
        statements := array_append(statements, format('FUNCTION 1 mysql.%I(%s,%s)', family_name || '_cmp', t, t));
        statements := array_append(statements, 'FUNCTION 5 mysql.prefix_options(internal)');
        statements := array_append(statements, 'STORAGE bytea');
    ELSE
        SELECT * INTO STRICT p FROM pg_amproc WHERE amprocfamily=c.opcfamily
            AND amproclefttype=t AND amprocrighttype=t AND amprocnum=1;
        statements := array_append(statements, format('FUNCTION 1 %s', p.amproc::regprocedure));
    END IF;
    EXECUTE format('CREATE OPERATOR CLASS mysql.%I DEFAULT FOR TYPE %s USING mysql_prefix AS %s',
                   family_name, t, array_to_string(statements, ', '));
    IF t IN ('text'::regtype, 'bpchar'::regtype) THEN
        EXECUTE format('ALTER OPERATOR FAMILY mysql.%I USING mysql_prefix ADD '
                       'OPERATOR 6 mysql.~#~ (%s,text), OPERATOR 7 pg_catalog.~~ (%s,text)',
                       family_name, t, t);
    ELSIF string_storage AND (SELECT typcollation<>0 FROM pg_type WHERE oid=t) THEN
        -- The binary worker's text_string is collatable bytea-layout data,
        -- including embedded NUL. Both parameters stay length-delimited.
        FOREACH rhs IN ARRAY ARRAY[t, 'text'::regtype] LOOP
            EXECUTE format('CREATE FUNCTION mysql.prefix_like(%s,%s) RETURNS boolean '
                'AS %L,%L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE', t, rhs,
                '$libdir/aux_mysql', 'mys_prefix_like');
            EXECUTE format('CREATE FUNCTION mysql.prefix_not_like(%s,%s) RETURNS boolean '
                'AS %L,%L LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE', t, rhs,
                '$libdir/aux_mysql', 'mys_prefix_not_like');
            EXECUTE format('CREATE OPERATOR mysql.~#~ (LEFTARG=%s,RIGHTARG=%s,FUNCTION=mysql.prefix_like)',t,rhs);
            EXECUTE format('CREATE OPERATOR mysql.!~#~ (LEFTARG=%s,RIGHTARG=%s,FUNCTION=mysql.prefix_not_like)',t,rhs);
            EXECUTE format('ALTER OPERATOR FAMILY mysql.%I USING mysql_prefix ADD OPERATOR 6 mysql.~#~ (%s,%s)',family_name,t,rhs);
        END LOOP;
    END IF;
END $body$;

-- Integration order: binary types first, then this fragment. Optional here
-- so the AM can also be tested independently against unmodified PostgreSQL.
DO $body$
DECLARE name text; t regtype;
BEGIN
    FOREACH name IN ARRAY ARRAY['mysql.binary_n','mysql.varbinary_n','mysql.text_string'] LOOP
        t := to_regtype(name);
        IF t IS NOT NULL THEN PERFORM mysql.register_prefix_type(t, true); END IF;
    END LOOP;
END $body$;

CREATE OR REPLACE FUNCTION mysql.index_key_parts(pg_catalog.oid,
    OUT seq pg_catalog.int4, OUT column_name pg_catalog.text, OUT sub_part pg_catalog.int4)
RETURNS SETOF record AS '$libdir/aux_mysql', 'mys_index_key_parts19'
LANGUAGE C STABLE STRICT;
REVOKE ALL ON FUNCTION mysql.register_prefix_type(regtype, boolean) FROM PUBLIC;
CREATE FUNCTION mysql.register_prefix_binary(t regtype) RETURNS void
    LANGUAGE sql SET search_path=pg_catalog AS 'SELECT mysql.register_prefix_type(t, true)';
REVOKE ALL ON FUNCTION mysql.register_prefix_binary(regtype) FROM PUBLIC;

SELECT mysql.register_prefix_type('text', true);
SELECT mysql.register_prefix_type('bpchar', true);
SELECT mysql.register_prefix_type('bytea', true);
DO $body$
DECLARE t regtype;
BEGIN
    FOREACH t IN ARRAY ARRAY['bool'::regtype, 'int2'::regtype, 'int4'::regtype,
        'int8'::regtype, 'float4'::regtype, 'float8'::regtype, 'numeric'::regtype,
        'date'::regtype, 'time'::regtype, 'timestamp'::regtype, 'timestamptz'::regtype,
        'interval'::regtype, 'oid'::regtype, 'uuid'::regtype, 'bit'::regtype, 'varbit'::regtype]
    LOOP PERFORM mysql.register_prefix_type(t, false); END LOOP;
END $body$;
