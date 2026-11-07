
-- ==== aux_mysql additions (hand-written, not part of openHalo) ====

CREATE FUNCTION mysql.show_diagnostics(boolean)
    RETURNS TABLE ("Level" text, "Code" integer, "Message" text)
    AS '$libdir/aux_mysql', 'mys_show_diagnostics'
    LANGUAGE C STRICT VOLATILE;

-- openHalo answered @@version in its protocol adapter from the
-- mysql.halo_mysql_version setting; sysvar_core.c does the same, the row
-- only makes the variable known to the system variable machinery.
INSERT INTO mys_informa_schema.base_variables
    (variable_name, def_value, conf_value, sess_global_type, sess_def_val_from_global, is_read_write)
VALUES ('version', coalesce(current_setting('mysql.halo_mysql_version', true), '5.7.32-log'),
        coalesce(current_setting('mysql.halo_mysql_version', true), '5.7.32-log'), 0, true, false)
ON CONFLICT (variable_name) DO NOTHING;
CREATE OR REPLACE FUNCTION mysql.connection_id() RETURNS bigint
    LANGUAGE sql STABLE PARALLEL SAFE AS $$ SELECT pg_catalog.pg_backend_pid()::bigint $$;

-- caching_sha2_password fast-auth cache (shared memory).  MySQL clears it
-- with FLUSH PRIVILEGES; entries also expire by themselves whenever the
-- role's password verifier changes.
CREATE FUNCTION mysql.flush_auth_cache() RETURNS bigint
    AS '$libdir/aux_mysql', 'mys_flush_auth_cache'
    LANGUAGE C VOLATILE STRICT;
REVOKE ALL ON FUNCTION mysql.flush_auth_cache() FROM PUBLIC;

-- ---------------------------------------------------------------------------
-- MySQL arithmetic (mys_semantics.c)
--   "/" on exact numbers yields DECIMAL: int / int has scale 4 (5/2 = 2.5000),
--   decimal / x has the dividend's scale + 4; float operands give DOUBLE.
--   Division and modulo by 0 yield NULL with warning 1365 (an error for
--   INSERT / UPDATE in strict mode with ERROR_FOR_DIVISION_BY_ZERO).
--   DIV is integer division (the grammar maps it to mysql.int_div()).
-- Only MySQL sessions see these: schema mysql precedes pg_catalog on their
-- search_path.  An operator is needed for every exact signature that
-- pg_catalog has, since an exact match is looked up first.
-- ---------------------------------------------------------------------------
CREATE FUNCTION mysql.mys_div_int8(int8, int8) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_div_int8' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.mys_div_numeric(numeric, numeric) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_div_numeric' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.mys_div_float8(float8, float8) RETURNS float8
    AS '$libdir/aux_mysql', 'mys_div_float8' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.mys_mod_int8(int8, int8) RETURNS int8
    AS '$libdir/aux_mysql', 'mys_mod_int8' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.mys_mod_numeric(numeric, numeric) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_mod_numeric' LANGUAGE C STABLE STRICT PARALLEL SAFE;

DO $do$
DECLARE
    l text;
    r text;
BEGIN
    FOREACH l IN ARRAY ARRAY['int2', 'int4', 'int8'] LOOP
        FOREACH r IN ARRAY ARRAY['int2', 'int4', 'int8'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.div_%1$s_%2$s(%1$s, %2$s) RETURNS numeric
                LANGUAGE sql STABLE STRICT PARALLEL SAFE AS 'SELECT mysql.mys_div_int8($1, $2)'$f$, l, r);
            EXECUTE format('CREATE OPERATOR mysql./ (FUNCTION = mysql.div_%1$s_%2$s, LEFTARG = %1$s, RIGHTARG = %2$s)', l, r);
            EXECUTE format($f$CREATE FUNCTION mysql.mod_%1$s_%2$s(%1$s, %2$s) RETURNS int8
                LANGUAGE sql STABLE STRICT PARALLEL SAFE AS 'SELECT mysql.mys_mod_int8($1, $2)'$f$, l, r);
            EXECUTE format('CREATE OPERATOR mysql.%% (FUNCTION = mysql.mod_%1$s_%2$s, LEFTARG = %1$s, RIGHTARG = %2$s)', l, r);
        END LOOP;
    END LOOP;
    FOREACH l IN ARRAY ARRAY['float4', 'float8'] LOOP
        FOREACH r IN ARRAY ARRAY['float4', 'float8'] LOOP
            EXECUTE format($f$CREATE FUNCTION mysql.div_%1$s_%2$s(%1$s, %2$s) RETURNS float8
                LANGUAGE sql STABLE STRICT PARALLEL SAFE AS 'SELECT mysql.mys_div_float8($1, $2)'$f$, l, r);
            EXECUTE format('CREATE OPERATOR mysql./ (FUNCTION = mysql.div_%1$s_%2$s, LEFTARG = %1$s, RIGHTARG = %2$s)', l, r);
        END LOOP;
    END LOOP;
END
$do$;

CREATE OPERATOR mysql./ (FUNCTION = mysql.mys_div_numeric, LEFTARG = numeric, RIGHTARG = numeric);
CREATE OPERATOR mysql.% (FUNCTION = mysql.mys_mod_numeric, LEFTARG = numeric, RIGHTARG = numeric);
CREATE FUNCTION mysql.mod(numeric, numeric) RETURNS numeric
    LANGUAGE sql STABLE STRICT PARALLEL SAFE AS 'SELECT mysql.mys_mod_numeric($1, $2)';

-- (aux_mysql already has MOD() for integers returning NULL for 0)

-- DIV: integer part of the DECIMAL quotient, NULL for 0
CREATE FUNCTION mysql.int_div(numeric, numeric) RETURNS int8
    LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT trunc(mysql.mys_div_numeric($1, $2))::int8';

-- aux_mysql's string "/" returned bigint ('5'/'2' = 2); MySQL converts
-- strings to DOUBLE ('5'/'2' = 2.5)
DO $do$
DECLARE
    sig text;
BEGIN
    FOREACH sig IN ARRAY ARRAY['text,text', 'char,char', 'varchar,varchar', 'text,bigint',
                               'char,bigint', 'varchar,bigint', 'bigint,text', 'bigint,varchar',
                               'bigint,char'] LOOP
        EXECUTE format('DROP OPERATOR mysql./ (%s)', sig);
    END LOOP;
END
$do$;
CREATE FUNCTION mysql.str_to_double(text) RETURNS float8
    LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.convert_text_to_digit_text_for_mysql($1)::float8';
CREATE FUNCTION mysql.div_text_text(text, text) RETURNS float8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.mys_div_float8(mysql.str_to_double($1), mysql.str_to_double($2))';
CREATE FUNCTION mysql.div_text_float8(text, float8) RETURNS float8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.mys_div_float8(mysql.str_to_double($1), $2)';
CREATE FUNCTION mysql.div_float8_text(float8, text) RETURNS float8 LANGUAGE sql STABLE STRICT PARALLEL SAFE
    AS 'SELECT mysql.mys_div_float8($1, mysql.str_to_double($2))';
CREATE OPERATOR mysql./ (FUNCTION = mysql.div_text_text, LEFTARG = text, RIGHTARG = text);
CREATE OPERATOR mysql./ (FUNCTION = mysql.div_text_float8, LEFTARG = text, RIGHTARG = float8);
CREATE OPERATOR mysql./ (FUNCTION = mysql.div_float8_text, LEFTARG = float8, RIGHTARG = text);

-- ---------------------------------------------------------------------------
-- Collations.  mys_rewrite.c maps COLLATE *_ci to mysql.case_insensitive
-- (ICU, nondeterministic) and other collations (utf8mb4_bin, ...) to
-- mysql.case_sensitive: code point order, like *_bin.  String columns
-- without COLLATE get mysql.default_collation (case_insensitive), or
-- case_sensitive when the table's COLLATE= is not a _ci collation.
-- ---------------------------------------------------------------------------
CREATE COLLATION mysql.case_sensitive FROM "C";

-- ---------------------------------------------------------------------------
-- Row triggers created by MySQL DDL (mys_utility.c)
-- ---------------------------------------------------------------------------
-- ON UPDATE CURRENT_TIMESTAMP; arguments: attribute numbers of the columns
CREATE FUNCTION mysql.on_update_current_timestamp() RETURNS trigger
    AS '$libdir/aux_mysql', 'mys_on_update_trigger' LANGUAGE C;
-- AUTO_INCREMENT: explicit values beyond the counter move it forward
CREATE FUNCTION mysql.auto_increment_advance() RETURNS trigger
    AS '$libdir/aux_mysql', 'mys_autoinc_trigger' LANGUAGE C;

-- ---------------------------------------------------------------------------
-- Non-strict sql_mode (no STRICT_TRANS_TABLES / STRICT_ALL_TABLES): literal
-- values of INSERT / UPDATE are converted leniently, with a warning, as in
-- MySQL ('12abc' -> 12, 300 into TINYINT -> 127, 'toolong' into VARCHAR(3)
-- -> 'too').  mys_rewrite.c inserts these calls.
-- ---------------------------------------------------------------------------
CREATE FUNCTION mysql.lenient_int(text, int8, int8) RETURNS int8
    AS '$libdir/aux_mysql', 'mys_lenient_int' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.lenient_numeric(text, int4) RETURNS numeric
    AS '$libdir/aux_mysql', 'mys_lenient_numeric' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.lenient_float8(text) RETURNS float8
    AS '$libdir/aux_mysql', 'mys_lenient_float8' LANGUAGE C STABLE STRICT PARALLEL SAFE;
CREATE FUNCTION mysql.lenient_varchar(text, int4) RETURNS text
    AS '$libdir/aux_mysql', 'mys_lenient_varchar' LANGUAGE C STABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- mysql.getenv() (aux_mysql 1.4, openHalo issue 10) is only needed while the
-- extension script fills @@datadir; it would expose the server's environment
-- to every MySQL user.
-- ---------------------------------------------------------------------------
REVOKE ALL ON FUNCTION mysql.getenv(text) FROM PUBLIC;
