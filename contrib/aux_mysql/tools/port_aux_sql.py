#!/usr/bin/env python3
"""
Build the aux_mysql extension script from openHalo's aux_mysql 1.1 -> 1.5
scripts, patched for stock PostgreSQL 18.

    port_aux_sql.py <openhalo/contrib/aux_mysql> sql/aux_mysql.sql

The Makefile appends sql/aux_mysql_additions.sql (hand-written objects that
are not part of openHalo) to build the final extension script.

openHalo installed aux_mysql with its *modified* PostgreSQL parser and
analyzer (standard_parserengine_auxiliary = on), which silently accepted a
few non-PostgreSQL constructs.  Each fix below documents what stock PG18
needs instead.
"""
import os
import sys
from pathlib import Path

src_dir, dst = sys.argv[1], sys.argv[2]
parts = ['aux_mysql--1.1.sql', 'aux_mysql--1.1--1.2.sql',
         'aux_mysql--1.2--1.3.sql', 'aux_mysql--1.3--1.4.sql',
         'aux_mysql--1.4--1.5.sql']

body = []
for p in parts:
    text = open(os.path.join(src_dir, p), encoding='utf-8', errors='surrogateescape').read()
    lines = [l for l in text.split('\n') if not l.startswith('\\echo')]
    body.append('\n-- ==== from %s ====\n' % p + '\n'.join(lines))
s = '\n'.join(body)


def rep(old, new, count=1):
    global s
    n = s.count(old)
    if count is not None and n != count:
        sys.exit("expected %d match(es), got %d for: %r" % (count, n, old[:100]))
    s = s.replace(old, new)


# C functions live in aux_mysql.so (protocol + functions) instead of mysm.so
s = s.replace("'$libdir/mysm'", "'$libdir/aux_mysql'")
s = s.replace('MODULE_PATHNAME', '$libdir/aux_mysql')

# PG18 has built-in bytea -> int2/int4/int8 casts (new in 18); keep those.
for t in ('int8', 'int4', 'int2'):
    rep('create cast(pg_catalog.bytea as pg_catalog.%s) with function mysql.bytea2%s(pg_catalog.bytea) as assignment;' % (t, t),
        '-- PG18 has a built-in bytea -> %s cast\n-- create cast(pg_catalog.bytea as pg_catalog.%s) with function mysql.bytea2%s(pg_catalog.bytea) as assignment;' % (t, t, t))

# backquoted identifiers are MySQL syntax, not valid in PL/pgSQL bodies
rep('mysql.`extract`($1, $2::timestamp)', 'mysql."extract"($1, $2::timestamp)')

# backslash-escaped quotes need standard_conforming_strings = off
rep("(attname = \\'%s\\')',", "(attname = ''%s'')',")
rep("ret := pg_catalog.concat(ret, ' COMMENT \\'', records.column_comment, '\\'');",
    "ret := pg_catalog.concat(ret, ' COMMENT ''', records.column_comment, '''');")

# IFNULL(numeric, text) yields a string in MySQL; the body already returns text
rep('''CREATE or replace FUNCTION mysql.ifnull(p1 numeric, p2 text)
RETURNS numeric IMMUTABLE AS $$''', '''CREATE or replace FUNCTION mysql.ifnull(p1 numeric, p2 text)
RETURNS text IMMUTABLE AS $$''')

# openHalo's auxiliary PG parser took "..." as a string literal (MySQL
# semantics) even inside PL/pgSQL; stock PG reads it as an identifier.
for v, n in (('PRI', 2), ('UNI', 2), ('MUL', 4)):
    rep('ret := "%s";' % v, "ret := '%s';" % v, n)

# MySQL's sys schema lives in mys_sys (the grammar maps sys.* there); the
# extension's own "sys" schema was vestigial.  IvorySQL and Babelfish own a
# schema named sys, and an extension script must not adopt an existing one.
rep("""CREATE SCHEMA IF NOT EXISTS sys;
GRANT USAGE ON SCHEMA sys TO PUBLIC;
""", """-- (no "sys" schema: MySQL's sys is mys_sys, see port_aux_sql.py)
""")
rep("GRANT ALL PRIVILEGES ON ALL tables IN SCHEMA sys TO public;\n", "", 4)
rep("DROP TABLE IF EXISTS sys.innodb_lock_waits;", "DROP TABLE IF EXISTS mys_sys.innodb_lock_waits;")

# these read per-statement session state and must not be constant-folded
rep("""CREATE OR REPLACE FUNCTION mysql.row_count()
RETURNS int8
AS '$libdir/aux_mysql', 'rowCount'
LANGUAGE C STRICT IMMUTABLE;""", """CREATE OR REPLACE FUNCTION mysql.row_count()
RETURNS int8
AS '$libdir/aux_mysql', 'rowCount'
LANGUAGE C STRICT VOLATILE;""", None)

# PG18 identity and trigger metadata replaces PG14's modified defaults.
# FLOAT now uses float4, while DOUBLE/FLOAT(25..53) use float8.  Do not
# export float8 as FLOAT: replay would silently narrow the stored values.
rep("when mysql.left(ty.typname, 5)='float' then 'float'",
    "when ty.typname='float8' then 'double'\n"
    "            when mysql.left(ty.typname, 5)='float' then 'float'", 7)
import re
s, n = re.subn(r"\(mysql\.get_column_extra\(mysql\.amend_def_val_for_internal\(pg_get_expr\(\(select ad\.adbin from pg_catalog\.pg_attrdef ad where ad\.adnum = att\.attnum and ad\.adrelid = att\.attrelid\), (?:att\.attrelid|tabOid)\)\)\)\)",
               "mysql.column_extra(att.attrelid, att.attnum)", s)
assert n == 3, n
start = s.index('CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(')
end = s.index('IMMUTABLE LANGUAGE plpgsql;', start) + len('IMMUTABLE LANGUAGE plpgsql;')
f = s[start:end]
f = f.replace("'\\n", "E'\\n").replace("',\\n", "E',\\n")
f = f.replace('select att.attname as column_name,',
              'select att.attidentity as identity, mysql.column_extra(att.attrelid, att.attnum) as extra, att.attname as column_name,')
f = f.replace('where att.atttypid = ty.oid',
              'where coalesce(mysql.column_domain(att.attrelid, att.attnum), att.atttypid) = ty.oid')
f = f.replace("(cons.contype = 'c') and (cons.conrelid = tabOid)",
              "(cons.contype = 'c') and (cons.conrelid = tabOid) and cons.conname not like 'aux_mysql_domain_%'")
f = f.replace('if (records.column_default is not null) then', '''if (records.identity != '') then
                ret := pg_catalog.concat(ret, ' AUTO_INCREMENT');
                BEGIN
                    execute format('select last_value + case when is_called then 1 else 0 end from %s',
                        pg_get_serial_sequence(format('%I.%I', schName, tabName), records.column_name)) into autoInc;
                EXCEPTION WHEN insufficient_privilege THEN
                    autoInc := NULL;
                END;
            elsif (records.column_default is not null) then''')
f = f.replace('if (records.column_collation is not null) then', '''if (position('on update' in records.extra) > 0) then
                ret := pg_catalog.concat(ret, ' ', substring(records.extra from position('on update' in records.extra)));
            end if;
            if (records.column_collation is not null) then''')
f = f.replace('autoInc := autoInc + 1;', '-- identity metadata already supplies the next value')
f = f.replace("    tableComment := '';", '''    ret := pg_catalog.concat(ret, ' COLLATE=', coalesce((select label from pg_catalog.pg_seclabel
        where classoid = 'pg_class'::regclass and objoid = tabOid and objsubid = 0 and provider = 'aux_mysql'), 'case_insensitive'));
    tableComment := '';''')
f = f.replace("records.column_comment, '''');", "replace(records.column_comment, '''', ''''''), '''');")
f = f.replace("tableComment, '''');", "replace(tableComment, '''', ''''''), '''');")
f = f.replace('IMMUTABLE LANGUAGE plpgsql;', 'STABLE LANGUAGE plpgsql;')
s = s[:start] + f + s[end:]

# As in MySQL, a new connection (and RESET CONNECTION, SET ... = DEFAULT)
# takes GLOBAL foreign_key_checks.
rep("values('foreign_key_checks', '1', '1', 0, false, true, null, null, null);",
    "values('foreign_key_checks', '1', '1', 0, true, true, null, null, null);")

# DECIMAL(p,s): SHOW COLUMNS, SHOW FULL COLUMNS, SHOW CREATE TABLE and
# information_schema.COLUMNS printed every numeric as a bare DECIMAL, which
# MySQL (and aux_mysql) read back as DECIMAL(10,0): a dump and reload changed
# the column's rounding and range.  Print the typmod; numeric without one MySQL
# can represent (unbounded, negative scale, ...) prints as the widest MySQL
# DECIMAL with a warning rather than failing the query.  DATA_TYPE and routine metadata keep
# 'decimal'.  The same definitions form the 1.6 -> 1.7 update script.
decimal_helpers = r'''-- MySQL DECIMAL(p,s) of a numeric typmod; DECIMAL(65,30) when MySQL has none.
CREATE OR REPLACE FUNCTION mysql.decimal_ps(pg_catalog.int4)
RETURNS pg_catalog.int4[]
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE SET search_path = pg_catalog
AS $$
SELECT CASE WHEN p BETWEEN 1 AND 65 AND s BETWEEN 0 AND 30 AND s <= p
            THEN ARRAY[p, s] ELSE ARRAY[65, 30] END
FROM (SELECT (($1 - 4) >> 16) & 65535 AS p, ((($1 - 4) & 2047) # 1024) - 1024 AS s) AS t
$$;

-- the column type; DECIMAL(65,30) comes with a warning (SHOW WARNINGS): values
-- with more than 35 integer or 30 fractional digits would not survive a replay
CREATE OR REPLACE FUNCTION mysql.decimal_column_type(pg_catalog.int4)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_decimal_column_type'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;
'''
key_definitions = []
# Prefix key parts: SHOW INDEX and SHOW CREATE TABLE listed index columns by
# attnum and skipped expressions, so a prefix key part, which aux_mysql indexes
# as left(col, n) / substr(col, 1, n), vanished (a key of such parts alone
# printed as "KEY `k`(``)", a replay error), columns came in table order, not
# key order, and Sub_part was always NULL.  Read the key parts, in order, from
# mysql.index_key_parts().  Part of the 1.6 -> 1.7 update below.
key_helpers = r"""-- the MySQL key parts of an index, in order: a column, or a prefix col(n)
CREATE OR REPLACE FUNCTION mysql.index_key_parts(pg_catalog.oid, OUT seq pg_catalog.int4,
    OUT column_name pg_catalog.text, OUT sub_part pg_catalog.int4)
RETURNS SETOF record
AS '$libdir/aux_mysql', 'mys_index_key_parts'
LANGUAGE C STABLE STRICT;
"""
for name in ('show_table_indexs', 'show_table_indexs_for_8'):
    prefix = 'CREATE OR REPLACE FUNCTION mysql.%s(' % name
    assert s.count(prefix) == 1, prefix
    start = s.index(prefix)
    end = s.index('IMMUTABLE LANGUAGE plpgsql;', start) + len('IMMUTABLE LANGUAGE plpgsql;')
    f = s[start:end]
    for old, new in (
            ("        keys := string_to_array(records.indkey::text, ' ');\n"
             "        FOREACH element IN ARRAY keys LOOP\n", ''),
            ('(mysql.get_seq_in_index(att.attnum::pg_catalog.text, records.indkey))::int as Seq_in_index',
             'k.seq::int as Seq_in_index'),
            ('\n                    NULL::int as Sub_part', '\n                    k.sub_part::int as Sub_part'),
            ('                from pg_catalog.pg_attribute att\n'
             '                where att.attrelid = tabOid and att.attnum = element\n',
             '                from mysql.index_key_parts(records.indexrelid) k\n'
             '                    join pg_catalog.pg_attribute att on att.attrelid = tabOid and att.attname = k.column_name\n'
             '                order by k.seq\n'),
            ('            END LOOP;\n        END LOOP;\n    END LOOP;', '            END LOOP;\n    END LOOP;'),
            ('IMMUTABLE LANGUAGE plpgsql;', 'STABLE LANGUAGE plpgsql;')):
        assert f.count(old) == 1, (name, old)
        f = f.replace(old, new)
    s = s[:start] + f + s[end:]
    key_definitions.append(f)

start = s.index('CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(')
end = s.index('STABLE LANGUAGE plpgsql;', start) + len('STABLE LANGUAGE plpgsql;')
f = s[start:end]
for old, new in (
        ('        select ind.indrelid, ind.indkey, ind.indnatts,',
         '        select ind.indexrelid, ind.indrelid, ind.indkey, ind.indnatts,'),
        ("""        ret := pg_catalog.concat(ret, E',\\n  ');
        columnNums := pg_catalog.replace(records.indkey::pg_catalog.text, ' '::pg_catalog.text, ','::pg_catalog.text);
""", """        select pg_catalog.string_agg(pg_catalog.concat('`', k.column_name, '`',
                case when k.sub_part is not null then pg_catalog.concat('(', k.sub_part, ')') end), ',' order by k.seq)
            from mysql.index_key_parts(records.indexrelid) k into columnNames;
        continue when columnNames is null;
        ret := pg_catalog.concat(ret, E',\\n  ');
"""),
        ("""        execute format('select pg_catalog.string_agg(attname, ''`,`'') from pg_attribute where attrelid = %s and attnum in (%s)',
                       records.indrelid, columnNums)
            into columnNames;
""", ''),
        ("'PRIMARY KEY (`', columnNames, '`)'", "'PRIMARY KEY (', columnNames, ')'"),
        ("'UNIQUE KEY `', records.indname, '`(`', columnNames, '`)'", "'UNIQUE KEY `', records.indname, '`(', columnNames, ')'"),
        ("'KEY `', records.indname, '`(`', columnNames, '`)'", "'KEY `', records.indname, '`(', columnNames, ')'")):
    assert f.count(old) == 1, old
    f = f.replace(old, new)
s = s[:start] + f + s[end:]

decimal_definitions = []
old_type = "when ty.typname='numeric' then 'decimal'"
new_type = "when ty.oid='pg_catalog.numeric'::pg_catalog.regtype then mysql.decimal_column_type(att.atttypmod)"
for prefix, terminator in (
        ('CREATE OR REPLACE VIEW mys_informa_schema.columns AS', 'order by att.attnum;'),
        ('CREATE OR REPLACE FUNCTION mysql.show_table_columns(', 'IMMUTABLE LANGUAGE plpgsql;'),
        ('CREATE OR REPLACE FUNCTION mysql.show_table_full_columns(', 'IMMUTABLE LANGUAGE plpgsql;'),
        ('CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(', 'STABLE LANGUAGE plpgsql;')):
    assert s.count(prefix) == 1, prefix
    start = s.index(prefix)
    end = s.index(terminator, start) + len(terminator)
    f = s[start:end]
    if 'VIEW' in prefix:
        # the first match is DATA_TYPE, the second COLUMN_TYPE
        assert f.count(old_type) == 2
        pos = f.rindex(old_type)
        f = f[:pos] + new_type + f[pos + len(old_type):]
        for field, i in (('NUMERIC_PRECISION', 1), ('NUMERIC_SCALE', 2)):
            assert f.count('NULL::bigint as ' + field) == 1
            f = f.replace('NULL::bigint as ' + field,
                          "(case when att.atttypid='pg_catalog.numeric'::pg_catalog.regtype "
                          "then (mysql.decimal_ps(att.atttypmod))[%d] end)::bigint as %s" % (i, field))
    else:
        assert f.count(old_type) == 1
        f = f.replace(old_type, new_type)
    f = '\n'.join(line.rstrip() for line in f.split('\n'))
    s = s[:start] + f + s[end:]
    decimal_definitions.append(f)

prelude = r'''-- aux_mysql 1.7: openHalo's aux_mysql SQL layer, ported to PostgreSQL 18
-- Generated by tools/port_aux_sql.py -- do not edit by hand.
-- Hand-written objects go to sql/aux_mysql_additions.sql.
\echo Use "CREATE EXTENSION aux_mysql" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS mysql;
CREATE FUNCTION mysql.column_extra(oid, integer) RETURNS text
    AS '$libdir/aux_mysql', 'mys_column_extra' LANGUAGE C STRICT STABLE;
-- A domain guard keeps the original integer type when identity uses its base.
CREATE FUNCTION mysql.column_domain(oid, integer) RETURNS oid
    LANGUAGE sql STRICT STABLE SET search_path = pg_catalog AS $$
    SELECT d.refobjid FROM pg_constraint c JOIN pg_depend d
      ON d.classid = 'pg_constraint'::regclass AND d.objid = c.oid
      AND d.refclassid = 'pg_type'::regclass
      JOIN pg_type t ON t.oid = d.refobjid AND t.typtype = 'd'
    WHERE c.conrelid = $1 AND c.conkey = ARRAY[$2]::int2[]
      AND c.contype = 'c' AND c.conname LIKE 'aux_mysql_domain_%'
    LIMIT 1 $$;
-- unqualified references inside the aux_mysql objects resolve to mysql first
SET LOCAL search_path = mysql, pg_catalog, public;
-- aux_mysql has forward references between SQL functions (as in a pg_dump)
SET LOCAL check_function_bodies = off;

-- openHalo's analyzer resolved DATE(text) as a cast; stock PG18 has only
-- date(timestamp[tz]) functions, so provide the text variant explicitly.
CREATE OR REPLACE FUNCTION pg_catalog.date(text) RETURNS date
    LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE AS $$ SELECT $1::date $$;

-- openHalo allowed TIME -> TIMESTAMP coercion (MySQL uses the current date)
CREATE OR REPLACE FUNCTION mysql.time_to_timestamp(time) RETURNS timestamp
    LANGUAGE sql STABLE STRICT PARALLEL SAFE AS $$ SELECT current_date + $1 $$;
CREATE CAST (time AS timestamp) WITH FUNCTION mysql.time_to_timestamp(time) AS ASSIGNMENT;
'''
# Casts are database-global, not per schema: another compatibility layer in
# the same database (IvorySQL's Oracle mode defines varchar -> bigint, for
# instance) may own one already, and CREATE CAST would make the whole
# extension fail.  Create each cast only if no such cast exists yet.
import re
cast_re = re.compile(r'(?im)^([ \t]*)(create cast\s*\(\s*([^()]+?)\s+as\s+([^()]+?)\s*\).*?;)[ \t]*$')


def guard_cast(m):
    indent, stmt, src, dst = m.groups()
    q = lambda t: "'" + t.replace("'", "''") + "'"
    return ("%sDO $cast$ BEGIN\n"
            "    IF NOT EXISTS (SELECT 1 FROM pg_cast WHERE castsource = %s::regtype AND casttarget = %s::regtype) THEN\n"
            "        %s\n"
            "    ELSE\n"
            "        RAISE NOTICE 'aux_mysql: keeping the existing cast (%s AS %s)';\n"
            "    END IF;\n"
            "END $cast$;" % (indent, q(src), q(dst), stmt, src.replace("'", "''"), dst.replace("'", "''")))


out, ncasts = cast_re.subn(guard_cast, prelude + '\n' + decimal_helpers + key_helpers + s)
if ncasts < 40:
    sys.exit("expected the aux_mysql casts, found %d" % ncasts)
print("guarded %d casts" % ncasts)
open(dst, 'w', encoding='utf-8', errors='surrogateescape').write(out + '\n')
print("extension script written to", dst)

# 1.6 -> 1.7: only the DECIMAL and key part objects above, replaced in place (OIDs, ACLs
# and dependent user views survive); never the initial script's DROPs.
upgrade = r'''-- Generated by tools/port_aux_sql.py -- do not edit by hand.
-- aux_mysql 1.6 -> 1.7: DECIMAL(p,s) and prefix key parts in SHOW and information_schema.
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.7'" to load this file. \quit
SET LOCAL search_path = mysql, pg_catalog, public;
SET LOCAL check_function_bodies = off;

''' + decimal_helpers + '\n' + key_helpers + '\n' + '\n\n'.join(decimal_definitions + key_definitions) + '\n'
upgrade_path = Path(dst).with_name('aux_mysql--1.6--1.7.sql')
upgrade_path.write_text(upgrade, encoding='utf-8')
print("extension update written to", upgrade_path)
