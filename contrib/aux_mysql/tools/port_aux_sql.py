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

# User-level locks: openHalo's GET_LOCK() and RELEASE_LOCK() returned 1
# without locking anything, so two sessions both "held" a lock and code that
# relies on it for mutual exclusion (schedulers, migrations) silently lost it.
# mys_lock.c implements them on session-level advisory locks.  The arguments
# are "any", as MySQL accepts any type: GET_LOCK(123, 1.5), GET_LOCK(x, '10').
# MySQL returns BIGINT.  Part of the 1.7 -> 1.8 update below.
lock_stubs = (('get_lock(text, int)', 'return 1;'), ('release_lock(text)', 'return 1;'),
              ('is_free_lock(text)', 'return 1;'), ('is_used_lock(text)', 'return null;'))
lock_definitions = r'''-- MySQL user-level locks (mys_lock.c)
CREATE OR REPLACE FUNCTION mysql.get_lock(pg_catalog."any", pg_catalog."any")
RETURNS pg_catalog.int8
AS '$libdir/aux_mysql', 'mys_get_lock'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION mysql.release_lock(pg_catalog."any")
RETURNS pg_catalog.int8
AS '$libdir/aux_mysql', 'mys_release_lock'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION mysql.is_free_lock(pg_catalog."any")
RETURNS pg_catalog.int8
AS '$libdir/aux_mysql', 'mys_is_free_lock'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION mysql.is_used_lock(pg_catalog."any")
RETURNS pg_catalog.int8
AS '$libdir/aux_mysql', 'mys_is_used_lock'
LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION mysql.release_all_locks()
RETURNS pg_catalog.int8
AS '$libdir/aux_mysql', 'mys_release_all_locks'
LANGUAGE C VOLATILE;
'''
stub_text = ''.join('''CREATE OR REPLACE FUNCTION mysql.%s
RETURNS int
LANGUAGE plpgsql
AS $function$
begin
    %s
end;
$function$;

''' % stub for stub in lock_stubs)
rep(stub_text, lock_definitions + '\n')

# Key names: MySQL names a key per table, PostgreSQL an index per schema, so
# aux_mysql 1.8 names a key's index "<key>$<8 hex digits>" (mys_rewrite.c).
# SHOW INDEX, SHOW CREATE TABLE and information_schema read the MySQL name
# from mysql.index_key_name(), which also replaces the guess that an index
# named *_pkey is the PRIMARY KEY.  information_schema.STATISTICS was an empty
# table; it lists the key parts as SHOW INDEX does.  SHOW CREATE TABLE puts a
# space between the key name and its columns, as MySQL does.  These objects
# form the 1.7 -> 1.8 update.
name_helpers = r'''-- the MySQL name of an index: PRIMARY, or its name without a "$<hex>" key suffix
CREATE OR REPLACE FUNCTION mysql.index_key_name(pg_catalog.oid)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_sql_index_key_name'
LANGUAGE C STABLE STRICT;
'''
name_definitions = []
old_key = ("(select case when right(cl.relname, 5) = '_pkey' then 'PRIMARY' else cl.relname end "
           "from pg_class cl where (cl.oid = records.indexrelid))::varchar(256) as key_name")
new_key = 'mysql.index_key_name(records.indexrelid)::varchar(256) as key_name'
for name in ('show_table_indexs', 'show_table_indexs_for_8', 'show_create_table_impl'):
    prefix = 'CREATE OR REPLACE FUNCTION mysql.%s(' % name
    assert s.count(prefix) == 1, prefix
    start = s.index(prefix)
    end = s.index('STABLE LANGUAGE plpgsql;', start) + len('STABLE LANGUAGE plpgsql;')
    f = s[start:end]
    if name == 'show_create_table_impl':
        for old, new, n in (
                ('(select cl.relname from pg_class cl where cl.oid = ind.indexrelid) as indname',
                 'mysql.index_key_name(ind.indexrelid) as indname', 1),
                ("records.indname, '`(', columnNames", "records.indname, '` (', columnNames", 2)):
            assert f.count(old) == n, (name, old)
            f = f.replace(old, new)
    else:
        assert f.count(old_key) == 1, name
        f = f.replace(old_key, new_key)
        # Index_type in MySQL's spelling (BTREE, HASH), as information_schema
        old_type = ('(select am.amname from pg_am am where am.oid = (select cl.relam from pg_class cl '
                    'where cl.oid = records.indexrelid))::varchar(256) as Index_type')
        assert f.count(old_type) == 1, name
        f = f.replace(old_type, 'pg_catalog.upper(' + old_type.replace('::varchar(256) as Index_type', ')')
                      + '::varchar(256) as Index_type')
    s = s[:start] + f + s[end:]
    name_definitions.append(f)

prefix = 'drop view if exists mys_informa_schema.indexs cascade;\ncreate view mys_informa_schema.indexs as\n'
terminator = "where (cons.contype = 'f');"
assert s.count(prefix) == 1
start = s.index(prefix)
end = s.index(terminator, start) + len(terminator)
f = s[start:end]
old = ("    (select case when right(cl_sub.relname, 5) = '_pkey' then 'PRIMARY' else cl_sub.relname end\n"
       "        from pg_class cl_sub\n"
       "        where (cl_sub.oid = ind.indexrelid)\n"
       "        )::varchar(256) as key_name,\n")
assert f.count(old) == 1
f = f.replace(old, '    mysql.index_key_name(ind.indexrelid)::varchar(256) as key_name,\n')
old = ('    (select am.amname\n'
       '        from pg_am am, pg_class cl_sub\n'
       '        where am.oid = cl_sub.relam and cl_sub.oid = ind.indexrelid\n'
       '        )::varchar(256) as Index_type,\n')
assert f.count(old) == 1
f = f.replace(old, old.replace('(select am.amname', 'pg_catalog.upper((select am.amname')
              .replace(')::varchar(256) as Index_type', '))::varchar(256) as Index_type'))
s = s[:start] + f + s[end:]
name_definitions.append(f.replace(prefix, 'CREATE OR REPLACE VIEW mys_informa_schema.indexs AS\n'))

statistics_table = '''create table mys_informa_schema.statistics(
    TABLE_CATALOG varchar(256),
    TABLE_SCHEMA varchar(256),
    TABLE_NAME varchar(256),
    NON_UNIQUE bigint,
    INDEX_SCHEMA varchar(256),
    INDEX_NAME varchar(256),
    SEQ_IN_INDEX bigint,
    COLUMN_NAME varchar(256),
    "collation" varchar(256),
    CARDINALITY bigint,
    SUB_PART bigint,
    PACKED varchar(256),
    NULLABLE varchar(256),
    INDEX_TYPE varchar(256),
    COMMENT varchar(256),
    INDEX_COMMENT varchar(256)
);
'''
statistics_view = '''-- the key parts of the tables' indexes, as SHOW INDEX shows them
create view mys_informa_schema.statistics as
select
    'def'::varchar(256) as TABLE_CATALOG,
    ns.nspname::varchar(256) as TABLE_SCHEMA,
    tc.relname::varchar(256) as TABLE_NAME,
    (case when ind.indisunique then 0 else 1 end)::bigint as NON_UNIQUE,
    ns.nspname::varchar(256) as INDEX_SCHEMA,
    mysql.index_key_name(ind.indexrelid)::varchar(256) as INDEX_NAME,
    k.seq::bigint as SEQ_IN_INDEX,
    k.column_name::varchar(256) as COLUMN_NAME,
    'A'::varchar(256) as "collation",
    0::bigint as CARDINALITY,
    k.sub_part::bigint as SUB_PART,
    NULL::varchar(256) as PACKED,
    (case when att.attnotnull then '' else 'YES' end)::varchar(256) as NULLABLE,
    pg_catalog.upper(am.amname)::varchar(256) as INDEX_TYPE,
    ''::varchar(256) as COMMENT,
    ''::varchar(256) as INDEX_COMMENT
from pg_catalog.pg_index ind
    join pg_catalog.pg_class tc on tc.oid = ind.indrelid
    join pg_catalog.pg_namespace ns on ns.oid = tc.relnamespace
    join pg_catalog.pg_class ic on ic.oid = ind.indexrelid
    join pg_catalog.pg_am am on am.oid = ic.relam
    cross join lateral mysql.index_key_parts(ind.indexrelid) k
    join pg_catalog.pg_attribute att on att.attrelid = ind.indrelid and att.attname = k.column_name
where tc.oid >= 16384 and tc.relkind in ('r', 'p');
'''
rep(statistics_table, statistics_view)

# Generated columns (col type AS (expr) [VIRTUAL | STORED], stored by
# mys_rewrite.c): SHOW COLUMNS, SHOW FULL COLUMNS and information_schema.COLUMNS
# printed the generation expression as the column default, SHOW CREATE TABLE
# as a DEFAULT clause (a replay error, or an ordinary column).  Show them as
# MySQL does: no default, Extra VIRTUAL / STORED GENERATED (mysql.column_extra;
# mysqldump leaves these columns out of its INSERTs), GENERATION_EXPRESSION and
# GENERATED ALWAYS AS (expr) VIRTUAL | STORED.  The same definitions form the
# 1.7 -> 1.8 update script.
generated_helpers = r'''-- the expression of a generated column as MySQL shows it; NULL for another column
CREATE OR REPLACE FUNCTION mysql.column_generation(pg_catalog.oid, pg_catalog.int4)
RETURNS pg_catalog.text
AS '$libdir/aux_mysql', 'mys_column_generation'
LANGUAGE C STABLE STRICT;
-- a generated column read by another one: its name and its value, the
-- expression it is computed by (PostgreSQL computes generated columns from
-- the row as written); the planner reduces it to the value
CREATE OR REPLACE FUNCTION mysql.generated_column(pg_catalog.text, anyelement)
RETURNS anyelement
LANGUAGE sql IMMUTABLE PARALLEL SAFE
AS 'SELECT $2';
'''
generated_definitions = []
attrdef = 'from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid)'
no_default = (attrdef, attrdef[:-1] + " and att.attgenerated = '')")
for prefix, terminator, edits in (
        ('CREATE OR REPLACE VIEW mys_informa_schema.columns AS', 'order by att.attnum;', (
            no_default,
            ("    ''::text as GENERATION_EXPRESSION,",
             "    (case when att.attgenerated <> '' then mysql.column_generation(att.attrelid, att.attnum)\n"
             "          else '' end)::text as GENERATION_EXPRESSION,"))),
        ('CREATE OR REPLACE FUNCTION mysql.show_table_columns(', 'IMMUTABLE LANGUAGE plpgsql;', (no_default,)),
        ('CREATE OR REPLACE FUNCTION mysql.show_table_full_columns(', 'IMMUTABLE LANGUAGE plpgsql;', (no_default,)),
        ('CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(', 'STABLE LANGUAGE plpgsql;', (
            ('where ad.adrelid = tabOid and ad.adnum = att.attnum), tabOid)) as column_default,',
             "where ad.adrelid = tabOid and ad.adnum = att.attnum and att.attgenerated = ''), tabOid)) as column_default,\n"
             "                mysql.column_generation(att.attrelid, att.attnum) as generation,"),
            ("            ret := pg_catalog.concat(ret, ' ', records.column_type);\n",
             "            ret := pg_catalog.concat(ret, ' ', records.column_type);\n"
             "            if (records.generation is not null) then\n"
             "                ret := pg_catalog.concat(ret, ' GENERATED ALWAYS AS (', records.generation, ') ',\n"
             "                    pg_catalog.split_part(records.extra, ' ', 1));\n"
             "            end if;\n"),
            ("                if (records.is_nullable != 'NO') then\n",
             "                if (records.is_nullable != 'NO' and records.generation is null) then\n")))):
    assert s.count(prefix) == 1, prefix
    start = s.index(prefix)
    end = s.index(terminator, start) + len(terminator)
    f = s[start:end]
    for old, new in edits:
        assert f.count(old) == 1, (prefix, old)
        f = f.replace(old, new)
    s = s[:start] + f + s[end:]
    generated_definitions.append(f)

prelude = r'''-- aux_mysql 1.8: openHalo's aux_mysql SQL layer, ported to PostgreSQL 18
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


out, ncasts = cast_re.subn(guard_cast, prelude + '\n' + decimal_helpers + key_helpers + name_helpers +
                          generated_helpers + s)
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

# 1.7 -> 1.8 (with sql/aux_mysql_binary.sql, see Makefile): the lock
# functions change their argument and result types, so the stubs are dropped
# (nothing in the extension depends on them); the key name and generated
# column objects above are replaced in place (the generated column ones last:
# they include the key name changes); the empty information_schema.STATISTICS
# table becomes the view (a user view on it makes the update fail rather than
# being dropped).
upgrade = r"""-- Generated by tools/port_aux_sql.py -- do not edit by hand.
-- The generated part of the 1.7 -> 1.8 update; the Makefile appends sql/aux_mysql_binary.sql.
-- aux_mysql 1.7 -> 1.8: GET_LOCK(), RELEASE_LOCK(), IS_FREE_LOCK(), IS_USED_LOCK() and
-- RELEASE_ALL_LOCKS() lock for real (the 1.7 functions were stubs that always succeeded);
-- MySQL key names per table and generated columns in SHOW and information_schema.
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.8'" to load this file. \quit
SET LOCAL search_path = mysql, pg_catalog, public;
SET LOCAL check_function_bodies = off;

""" + ''.join('DROP FUNCTION IF EXISTS mysql.%s;\n' % name for name, _ in lock_stubs) + '\n' + lock_definitions + \
    '\n' + name_helpers + '\n' + '\n\n'.join(name_definitions) + '\n\n' + \
    'DROP TABLE mys_informa_schema.statistics;\n' + statistics_view + \
    'GRANT ALL PRIVILEGES ON mys_informa_schema.statistics TO public;\n\n' + \
    generated_helpers + '\n' + '\n\n'.join(generated_definitions) + '\n'
upgrade_path = Path(dst).with_name('aux_mysql_update18.sql')
upgrade_path.write_text(upgrade, encoding='utf-8')
print("extension update written to", upgrade_path)
