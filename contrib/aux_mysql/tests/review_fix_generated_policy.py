#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Generated policy and execution-time assignment checks; unique disposable schemas.
MYSQL_PWD=... python3 review_fix_generated_policy.py --ports 3792 3793 --password ...
Native legacy fixtures use port+2000 (override with --pg-ports). Port 3771 is the
MySQL 5.7 reference and skips native fixtures and explicitly accepted OH extras.
No global settings, library installs, server restarts or catalog writes.
"""
import argparse
import json
import os
import sys
import uuid

import pymysql


def run(port, pgport, args):
    schema = 'review_fix_gen_policy_' + uuid.uuid4().hex[:10]
    c = pymysql.connect(host=args.host, port=port, user=args.user,
                        password=args.password, autocommit=True, charset='utf8mb4')
    q = c.cursor()
    pg = None
    failures = []
    reference = port in args.reference_ports

    def sql(s, params=None):
        q.execute(s, params)
        return q.fetchall()

    def rejected(s, codes=(3102, 3763)):
        try:
            sql(s)
        except pymysql.MySQLError as e:
            assert e.args[0] in codes, (s, e.args)
        else:
            raise AssertionError(('accepted forbidden expression', s))

    def warn(code):
        rows = sql('SHOW WARNINGS')
        assert any(r[0] == 'Warning' and r[1] == code for r in rows), rows

    def reject_create_alter():
        sql("SET @policy_value=7")
        sql('CREATE TABLE alter_bad(v int,s varchar(8))')
        for i, expr in enumerate(('RAND()', 'IFNULL(RAND(),v)',
                'CASE WHEN v>0 THEN v ELSE RAND() END',
                'CONCAT(s,NOW())', 'IFNULL(CONNECTION_ID(),v)',
                '@policy_value', '@@auto_increment_increment', 'CURRENT_DATE')):
            codes = (3102, 3763, 1033) if reference and expr.startswith('@@') else (3102, 3763)
            rejected('CREATE TABLE bad%d(v int,s varchar(8),g varchar(64) AS(%s) STORED)' % (i, expr), codes)
            rejected('ALTER TABLE alter_bad ADD COLUMN g varchar(64) AS(%s) STORED' % expr, codes)
        # A word in an ordinary string is not a current-time expression.
        sql("CREATE TABLE word(v int,g varchar(12) AS(CONCAT('now',v)) STORED)")
        sql('INSERT INTO word(v) VALUES(1)')
        assert sql('SELECT g FROM word') == (('now1',),)

    def deterministic():
        sql('CREATE TABLE good(v int,s varchar(8),d datetime,'
            'g varchar(32) AS(CONCAT(IFNULL(s,\'x\'),v)) STORED,'
            'n int AS(v DIV 2) STORED,'
            'f varchar(32) AS(DATE_FORMAT(d,\'%Y-%m-%d\')) STORED)')
        sql("INSERT INTO good(v,s,d) VALUES(5,NULL,'2024-02-03 04:05:06'),(8,'q','2020-01-02')")
        assert sql('SELECT g,n,f FROM good ORDER BY v') == (('x5',2,'2024-02-03'),('q8',4,'2020-01-02'))
        sql('CREATE TABLE good_like LIKE good')
        sql("INSERT INTO good_like(v,s,d) VALUES(6,'a','2024-01-01')")
        assert sql('SELECT g,n,f FROM good_like') == (('a6',3,'2024-01-01'),)
        sql('ALTER TABLE good ADD COLUMN n2 int AS(IFNULL(v,0)+1) STORED')
        assert sql('SELECT n2 FROM good ORDER BY v') == ((6,), (9,))
        sql('CREATE TABLE bytes(b varbinary(8),s varchar(16),'
            'h varchar(16) AS(HEX(b)) STORED,u blob AS(UNHEX(s)) STORED)')
        sql("INSERT INTO bytes(b,s) VALUES(0x410042,'FF0041')")
        assert sql('SELECT h,HEX(u) FROM bytes') == (('410042','FF0041'),)

    def writer_mode():
        for name, decl, value, result, code in (
                ('num', 'decimal(2,0)', '999', 99, 1264),
                ('str', 'varchar(2)', "'abcd'", 'ab', 1265)):
            sql("SET sql_mode=''")
            sql('CREATE TABLE row_%s(v %s,g %s AS(v) STORED)' % (name, 'int' if name=='num' else 'varchar(8)', decl))
            sql("PREPARE row_insert FROM 'INSERT INTO row_%s(v) VALUES(?)'" % name)
            sql('SET @value='+value)
            for _ in range(6):
                sql('EXECUTE row_insert USING @value')
                warn(code)
            sql("SET sql_mode='STRICT_TRANS_TABLES'")
            rejected('EXECUTE row_insert USING @value', (code, 1406))
            sql("PREPARE row_ignore FROM 'INSERT IGNORE INTO row_%s(v) VALUES(?)'" % name)
            sql('EXECUTE row_ignore USING @value')
            warn(code)
            assert sql('SELECT count(*),min(g),max(g) FROM row_'+name) == ((7, result, result),)
            sql('DEALLOCATE PREPARE row_insert')
            sql('DEALLOCATE PREPARE row_ignore')

    def constant_mode():
        for name, decl, value, result, code in (
                ('num', 'decimal(2,0)', '999', 99, 1264),
                ('str', 'varchar(2)', "'abcd'", 'ab', 1265)):
            # DDL must not execute row assignment, under either writer mode.
            sql("SET sql_mode='STRICT_TRANS_TABLES'")
            sql('CREATE TABLE const_%s(v int,g %s AS(%s) STORED)' % (name, decl, value))
            sql("SET sql_mode=''")
            sql("PREPARE const_insert FROM 'INSERT INTO const_%s(v) VALUES(?)'" % name)
            sql('SET @value=1')
            for _ in range(6):
                sql('EXECUTE const_insert USING @value')
                warn(code)
            sql("SET sql_mode='STRICT_TRANS_TABLES'")
            rejected('EXECUTE const_insert USING @value', (code, 1406))
            sql("PREPARE const_ignore FROM 'INSERT IGNORE INTO const_%s(v) VALUES(?)'" % name)
            sql('EXECUTE const_ignore USING @value')
            warn(code)
            assert sql('SELECT count(*),min(g),max(g) FROM const_'+name) == ((7, result, result),)
            ddl = sql('SHOW CREATE TABLE const_'+name)[0][1]
            assert 'tableoid' not in ddl and 'aux_mysql_gen_' not in ddl and 'generated_value' not in ddl, ddl
            sql('CREATE TABLE const_copy_%s LIKE const_%s' % (name, name))
            sql('INSERT IGNORE INTO const_copy_%s(v) VALUES(1)' % name)
            warn(code)
            assert sql('SELECT g FROM const_copy_'+name) == ((result,),)
            sql('DEALLOCATE PREPARE const_insert')
            sql('DEALLOCATE PREPARE const_ignore')
        sql('CREATE TABLE null_constant(v int,g varchar(2) AS(NULL) STORED)')
        sql('INSERT INTO null_constant(v) VALUES(1)')
        assert sql('SELECT g FROM null_constant') == ((None,),)

    def native_legacy():
        nonlocal pg
        if reference:
            return
        import psycopg2
        from psycopg2 import sql as psql
        pg = psycopg2.connect(host=args.host, port=pgport, user=args.user,
                              password=args.password, dbname='postgres')
        pg.autocommit = True
        p = pg.cursor()
        p.execute(psql.SQL('SET search_path={},mysql,pg_catalog').format(psql.Identifier(schema)))
        p.execute('CREATE TABLE external_data(v int)')
        p.execute('INSERT INTO external_data VALUES(99)')
        p.execute("CREATE FUNCTION disguised(int) RETURNS int LANGUAGE sql IMMUTABLE RETURN $1 + pg_catalog.pg_backend_pid()")
        p.execute("CREATE FUNCTION rand(int) RETURNS int LANGUAGE sql IMMUTABLE RETURN $1")
        for name in ('disguised', 'rand'):
            rejected('CREATE TABLE udf_%s(v int,g int AS(%s.%s(v)) STORED)' % (name, schema, name))
        for name, body in (('rand', 'pg_catalog.random()+v'),
                           ('now', 'extract(epoch from pg_catalog.now())+v'),
                           ('conn', 'pg_catalog.pg_backend_pid()+v'),
                           ('date', 'extract(day from CURRENT_DATE)+v')):
            p.execute('CREATE FUNCTION aux_mysql_gen_legacy_%s(v int) RETURNS float8 LANGUAGE sql IMMUTABLE RETURN %s' % (name, body))
            p.execute('CREATE TABLE legacy_%s(v int,g float8 GENERATED ALWAYS AS(aux_mysql_gen_legacy_%s(v)) STORED)' % (name, name))
            rejected('CREATE TABLE copy_%s LIKE legacy_%s' % (name, name))
            rejected('ALTER TABLE legacy_%s ADD COLUMN extra int' % name)
            # Removal/replacement is a supported migration repair, not blocked.
            sql('ALTER TABLE legacy_%s MODIFY COLUMN g int AS(v+1) STORED' % name)
            sql('INSERT INTO legacy_%s(v) VALUES(4)' % name)
            assert sql('SELECT g FROM legacy_'+name) == ((5,),)
        p.execute('CREATE FUNCTION aux_mysql_gen_external(v int) RETURNS int LANGUAGE sql IMMUTABLE BEGIN ATOMIC SELECT external_data.v FROM external_data; END')
        p.execute('CREATE TABLE legacy_external(v int,g int GENERATED ALWAYS AS(aux_mysql_gen_external(v)) STORED)')
        rejected('CREATE TABLE copy_external LIKE legacy_external')
        sql('ALTER TABLE legacy_external DROP COLUMN g')
        # Quoted special temporal input must not disappear into a Const during
        # analysis, including the shipped text DATE_FORMAT SQL wrapper.
        rejected("CREATE TABLE special(v int,g varchar(40) AS(DATE_FORMAT('now','%Y')) STORED)")

    def retained_extras():
        if reference:
            return
        sql('CREATE TABLE extras(v int AUTO_INCREMENT PRIMARY KEY,g int AS(v+1) VIRTUAL)')
        sql('INSERT INTO extras(v) VALUES(8)')
        assert sql('SELECT g FROM extras') == ((9,),)
        sql('CREATE TABLE virtual_pk(v int,g int AS(v+1) VIRTUAL PRIMARY KEY)')
        sql('INSERT INTO virtual_pk(v) VALUES(1)')
        assert sql('SELECT g FROM virtual_pk') == ((2,),)

    try:
        sql('CREATE DATABASE '+schema)
        sql('USE '+schema)
        for case in (reject_create_alter, deterministic, writer_mode, constant_mode, native_legacy, retained_extras):
            try:
                case()
                print(json.dumps({'port':port,'case':case.__name__,'pass':True}), flush=True)
            except Exception as e:
                failures.append(case.__name__)
                print(json.dumps({'port':port,'case':case.__name__,'pass':False,'error':str(e)}), flush=True)
    finally:
        if pg:
            pg.close()
        sql('DROP DATABASE IF EXISTS '+schema)
        c.close()
    return failures


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--ports', type=int, nargs='+', default=[3792,3793])
    p.add_argument('--pg-ports', type=int, nargs='+')
    p.add_argument('--reference-ports', type=int, nargs='+', default=[3771])
    p.add_argument('--user', default='dbuser_mysql')
    p.add_argument('--password', default=os.getenv('MYSQL_PWD',''))
    args = p.parse_args()
    if args.pg_ports and len(args.pg_ports) != len(args.ports):
        p.error('--pg-ports must match --ports')
    result = [run(port, args.pg_ports[i] if args.pg_ports else port+2000, args)
              for i, port in enumerate(args.ports)]
    sys.exit(1 if any(result) else 0)
