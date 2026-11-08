-- Hand-written 1.9 objects. Integrate before the generated SHOW definitions.
-- Installing these objects does NOT recover old discarded generation expressions
-- or rewrite existing generators. See tools/review_fix_migration.sql.
CREATE OR REPLACE FUNCTION mysql.generated_value(anyelement)
RETURNS anyelement LANGUAGE sql IMMUTABLE PARALLEL SAFE AS 'SELECT $1';

CREATE OR REPLACE FUNCTION mysql.generated_varchar(pg_catalog.text, pg_catalog.int4, pg_catalog.bool)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_generated_varchar'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION mysql.generated_numeric(pg_catalog.numeric, pg_catalog.int4)
RETURNS pg_catalog.numeric
AS '$libdir/aux_mysql', 'mys_generated_numeric'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;

CREATE OR REPLACE FUNCTION mysql.column_default(pg_catalog.oid, pg_catalog.int4)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_column_default'
LANGUAGE C STABLE STRICT;

CREATE OR REPLACE FUNCTION mysql.show_create_view_impl(pg_catalog.text, pg_catalog.text)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_show_create_view'
LANGUAGE C STABLE STRICT;
