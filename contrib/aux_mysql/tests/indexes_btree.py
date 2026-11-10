#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Integrated 1.9 index routing, ORDER BY/LIMIT and prefix concurrency checks.

Needs pymysql and psycopg2. MYSQL_PORT/PGPORT default to 3790/5790;
MYSQL_HOST/PGHOST and MYSQL_USER/MYSQL_PASSWORD override the connection.
Creates one unique review_fix_* schema, drops it in finally, changes no
global settings and writes no artifacts. Run inside ohdev with python3 stdin.
"""
import concurrent.futures
import json
import os
import time

import psycopg2
import pymysql

schema = 'review_fix_indexplan_' + format(time.time_ns(), 'x')
user = os.getenv('MYSQL_USER', 'dbuser_mysql')
password = os.getenv('MYSQL_PASSWORD', 'Mysql.123')


def connect():
    return pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
        port=int(os.getenv('MYSQL_PORT', '3790')), user=user, password=password,
        charset='utf8mb4', autocommit=True, read_timeout=15, write_timeout=15)


def query(conn, sql, args=None):
    with conn.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchall() if cur.description else cur.rowcount


def duplicate(conn, sql):
    try:
        query(conn, sql)
    except pymysql.MySQLError as e:
        assert e.args[0] == 1062, e
    else:
        raise AssertionError('duplicate accepted: ' + sql)


def nodes(plan):
    yield plan
    for child in plan.get('Plans', []):
        yield from nodes(child)


mysql = connect()
pg = psycopg2.connect(host=os.getenv('PGHOST', '127.0.0.1'),
    port=int(os.getenv('PGPORT', '5790')), user=user, password=password,
    dbname=os.getenv('PGDATABASE', 'postgres'), connect_timeout=5)
pg.autocommit = True
try:
    query(mysql, 'CREATE DATABASE ' + schema)
    query(mysql, 'USE ' + schema)
    cases = [
        ('ordered', 'a varchar(64),b int,KEY ab(a,b)', 'btree'),
        ('inline_unique', 'a varchar(32) UNIQUE', 'btree'),
        ('inline_pk', 'a char(32) PRIMARY KEY', 'btree'),
        ('shortbinary', 'a binary(255),b varbinary(255),UNIQUE KEY ab(a,b)', 'btree'),
        ('inline_binary', 'a varbinary(255) UNIQUE', 'btree'),
        ('longfull', 'a varchar(700),KEY a(a)', 'mysql_prefix'),
        ('inline_long', 'a varchar(700) UNIQUE', 'mysql_prefix'),
        ('compoundwide', 'a varchar(300),b varchar(300),KEY ab(a,b)', 'mysql_prefix'),
        ('binarywide', 'a varbinary(1024),b varbinary(1024),KEY ab(a,b)', 'mysql_prefix'),
        ('declared', 'a varchar(32),b int,UNIQUE KEY ab(a(3),b)', 'mysql_prefix'),
        ('wholeprefix', 'a varchar(32),UNIQUE KEY a(a(32))', 'mysql_prefix'),
    ]
    for table, columns, expected in cases:
        query(mysql, f'CREATE TABLE {table}({columns}) COLLATE=utf8mb4_general_ci')
        with pg.cursor() as cur:
            cur.execute('''SELECT am.amname FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
                JOIN pg_am am ON am.oid=c.relam WHERE i.indrelid=%s::regclass''',
                (schema + '.' + table,))
            assert cur.fetchall() == [(expected,)], (table, expected)
    for table, ddl in (
        ('alterkey', 'ALTER TABLE alterkey ADD KEY ab(a,b)'),
        ('createindex', 'CREATE INDEX ab ON createindex(a,b)'),
    ):
        query(mysql, f'CREATE TABLE {table}(a varchar(64),b int)')
        query(mysql, ddl)
    query(mysql, 'CREATE TABLE addcolumn(b int)')
    query(mysql, 'ALTER TABLE addcolumn ADD COLUMN a varchar(32) UNIQUE')
    with pg.cursor() as cur:
        for table in ('alterkey', 'createindex', 'addcolumn'):
            cur.execute('''SELECT am.amname FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
                JOIN pg_am am ON am.oid=c.relam WHERE i.indrelid=%s::regclass''', (schema + '.' + table,))
            assert cur.fetchall() == [('btree',)], table
    query(mysql, "INSERT INTO inline_unique VALUES ('e')")
    duplicate(mysql, "INSERT INTO inline_unique VALUES ('É  ')")
    print('PASS whole compound byte budgets, inline/ALTER constraints and native btree GENERAL_CI/PAD SPACE', flush=True)

    with mysql.cursor() as cur:
        cur.executemany('INSERT INTO ordered VALUES (%s,%s)', [(f'key{i:06d}', i) for i in range(3000)])
    with pg.cursor() as cur:
        cur.execute('ANALYZE ' + schema + '.ordered')
        cur.execute('EXPLAIN (FORMAT JSON, COSTS OFF) SELECT a,b FROM ' + schema + '.ordered ORDER BY a,b LIMIT 10')
        plan = cur.fetchone()[0][0]['Plan']
        kinds = {n['Node Type'] for n in nodes(plan)}
        assert kinds.intersection({'Index Scan', 'Index Only Scan'}), plan
        assert not kinds.intersection({'Sort', 'Incremental Sort', 'Seq Scan'}), plan
        cur.execute('SELECT bool_and(pg_index_column_has_property(i.indexrelid,1,\'returnable\')) '
                    'FROM pg_index i WHERE i.indrelid=%s::regclass', (schema + '.ordered',))
        assert cur.fetchone() == (True,)
    print('PASS short full-key ORDER BY/LIMIT uses an ordered index without Sort; index-only capability retained',
          json.dumps(plan), flush=True)

    other = connect()
    try:
        query(other, 'USE ' + schema)
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            query(mysql, "BEGIN;" )
            query(mysql, "INSERT INTO declared VALUES ('held',1)")
            for value, suffix in (('free', 1), ('held', 2)):
                future = pool.submit(query, other, 'INSERT INTO declared VALUES (%s,%s)', (value, suffix))
                future.result(timeout=3)  # holder has not committed
            query(mysql, 'COMMIT')
            query(mysql, 'BEGIN')
            query(mysql, "INSERT INTO declared VALUES ('dupa',1)")
            future = pool.submit(query, other, "INSERT INTO declared VALUES ('DUPb',1)")
            time.sleep(0.2)
            assert not future.done(), 'equal logical prefixes must wait'
            query(mysql, 'COMMIT')
            try:
                future.result(timeout=3)
            except pymysql.MySQLError as e:
                assert e.args[0] == 1062, e
            else:
                raise AssertionError('concurrent prefix duplicate accepted')
    finally:
        query(mysql, 'ROLLBACK')
        other.close()
    print('PASS explicit-prefix disjoint keys proceed before commit; equal keys wait and reject', flush=True)
finally:
    query(mysql, 'ROLLBACK')
    query(mysql, 'DROP DATABASE IF EXISTS ' + schema)
    mysql.close()
    pg.close()
