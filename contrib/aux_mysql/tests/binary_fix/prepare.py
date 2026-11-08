import os,pathlib,sys
base=pathlib.Path(__file__).resolve().parent
lib=sys.argv[1]
# Minimal 1.8 dependencies used by hexlit SQL bodies (the stock PG instance
# does not have the rest of aux_mysql). New 1.9 code is tested unmodified.
s='''DROP SCHEMA IF EXISTS mysql CASCADE;
DROP SCHEMA IF EXISTS review_fix_binary CASCADE;
CREATE SCHEMA mysql;
CREATE DOMAIN mysql.blob AS bytea;
CREATE DOMAIN mysql.binary AS bytea;
CREATE DOMAIN mysql.varbinary AS bytea;
SET search_path=mysql,pg_catalog;
CREATE FUNCTION mysql.str_to_double(text) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT $1::float8';
CREATE FUNCTION mysql.mys_div_float8(float8,float8) RETURNS float8 LANGUAGE sql IMMUTABLE AS 'SELECT $1 / NULLIF($2,0)';
CREATE FUNCTION mysql.mys_mod_numeric(numeric,numeric) RETURNS numeric LANGUAGE sql IMMUTABLE AS 'SELECT mod($1,NULLIF($2,0))';
CREATE FUNCTION mysql.mys_div_numeric(numeric,numeric) RETURNS numeric LANGUAGE sql IMMUTABLE AS 'SELECT $1 / NULLIF($2,0)';
CREATE FUNCTION mysql.mys_div_int8(int8,int8) RETURNS numeric LANGUAGE sql IMMUTABLE AS 'SELECT $1::numeric / NULLIF($2,0)';
CREATE FUNCTION mysql.int_div(numeric,numeric) RETURNS int8 LANGUAGE sql IMMUTABLE AS 'SELECT trunc($1 / NULLIF($2,0))::int8';
BEGIN;
'''
for name in ['aux_mysql_binary.sql','aux_mysql_fix_binary.sql']:
    for line in (base.parent.parent/'sql'/name).read_text().splitlines():
        if not line.startswith('\\echo'):
            s+=line+'\n'
s+="""CREATE FUNCTION mysql.get_lock("any","any") RETURNS int8 AS '$libdir/aux_mysql','mys_get_lock' LANGUAGE C VOLATILE;
CREATE FUNCTION mysql.release_lock("any") RETURNS int8 AS '$libdir/aux_mysql','mys_release_lock' LANGUAGE C VOLATILE;
CREATE FUNCTION mysql.is_free_lock("any") RETURNS int8 AS '$libdir/aux_mysql','mys_is_free_lock' LANGUAGE C VOLATILE;
CREATE FUNCTION mysql.release_all_locks() RETURNS int8 AS '$libdir/aux_mysql','mys_release_all_locks' LANGUAGE C VOLATILE;
"""
s+="""CREATE FUNCTION mysql.locate(text,text) RETURNS int4 AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION mysql.locate(text,text,int4) RETURNS int4 AS '$libdir/aux_mysql','mys_string_locate' LANGUAGE C IMMUTABLE;
CREATE FUNCTION mysql.instr(text,text) RETURNS int4 AS '$libdir/aux_mysql','mys_string_instr' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION mysql.replace(text,text,text) RETURNS text AS 'replace_text' LANGUAGE internal IMMUTABLE STRICT;
CREATE FUNCTION mysql.if(int4,text,text) RETURNS text LANGUAGE sql AS 'SELECT CASE WHEN $1<>0 THEN $2 ELSE $3 END';
CREATE FUNCTION mysql.ifnull(text,text) RETURNS text LANGUAGE sql AS 'SELECT coalesce($1,$2)';
"""
s+="CREATE FUNCTION mysql.review_fix_value(text, bool DEFAULT false) RETURNS text AS '$libdir/aux_mysql', 'review_fix_value' LANGUAGE C VOLATILE;\nCOMMIT;\n"
s=s.replace('$libdir/aux_mysql',lib)
(pathlib.Path(os.environ['BINARY_FIX_BUILD_DIR'])/'setup.sql').write_text(s)
