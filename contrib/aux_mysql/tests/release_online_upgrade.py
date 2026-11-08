#!/usr/bin/env python3
"""Keep a MySQL session open while another backend upgrades aux_mysql 1.8→1.9.

Requires an otherwise idle disposable cluster, its historical 1.6 install SQL,
and --allow-listener-reload. Temporarily changes mysql.database, restores its
original setting and drops only the database created here. PG*/MYSQL_* select
the same cluster as the other release suites.
"""
import argparse
import os
import time
import uuid

import psycopg2
from psycopg2 import sql
import pymysql


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-listener-reload', action='store_true', required=True)
parser.parse_args()
name = 'review_fix_online_' + uuid.uuid4().hex[:10]
admin = psycopg2.connect(dbname=os.getenv('PGDATABASE', 'postgres'))
admin.autocommit = True
old = None
wire = None
created = changed = False


def pg_query(connection, statement, params=None):
    with connection.cursor() as q:
        q.execute(statement, params)
        return q.fetchall() if q.description else None


def query(statement):
    with wire.cursor() as q:
        q.execute(statement)
        return q.fetchall()


def connect_database(expected):
    deadline = time.monotonic() + 10
    while True:
        c = pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
                            port=int(os.getenv('MYSQL_PORT', '3792')),
                            user=os.getenv('MYSQL_USER', 'dbuser_mysql'),
                            password=os.getenv('MYSQL_PASSWORD', 'Mysql.123'),
                            autocommit=True, charset='utf8mb4')
        with c.cursor() as q:
            q.execute('SELECT current_database()')
            actual = q.fetchone()[0]
        if actual == expected:
            return c
        c.close()
        if time.monotonic() >= deadline:
            raise AssertionError(('listener database reload timed out', actual, expected))
        time.sleep(0.05)


try:
    original = pg_query(admin, 'SHOW mysql.database')[0][0]
    override = pg_query(admin, """SELECT setting FROM pg_file_settings
        WHERE name='mysql.database' AND sourcefile LIKE '%/postgresql.auto.conf'
        AND applied ORDER BY seqno DESC LIMIT 1""")
    pg_query(admin, sql.SQL('CREATE DATABASE {}').format(sql.Identifier(name)))
    created = True
    old = psycopg2.connect(dbname=name)
    old.autocommit = True
    pg_query(old, "CREATE EXTENSION aux_mysql VERSION '1.6'")
    pg_query(old, "ALTER EXTENSION aux_mysql UPDATE TO '1.8'")
    pg_query(admin, 'ALTER SYSTEM SET mysql.database = %s', (name,))
    changed = True
    pg_query(admin, 'SELECT pg_reload_conf()')
    wire = connect_database(name)
    query('CREATE DATABASE app')
    query('USE app')

    # Before SQL upgrade, the new binary must not call missing 1.9 helpers.
    query('CREATE TABLE old_data(id INT PRIMARY KEY,b BINARY(3),s TEXT)')
    query("INSERT INTO old_data VALUES(1,0x41,'abc')")
    assert query('SELECT id,HEX(b),s FROM old_data') == ((1, '41', 'abc'),)
    query('CREATE TABLE old_generated(v INT,g INT AS(v+1) STORED)')
    query('INSERT INTO old_generated(v) VALUES(3)')
    assert query('SELECT g FROM old_generated') == ((4,),)
    query('CREATE TABLE old_key(v VARCHAR(16),UNIQUE KEY u(v))')
    query("INSERT INTO old_key VALUES('abc')")
    print('PASS new binaries before SQL upgrade: legacy bytes, generated expression and index')

    pg_query(old, "ALTER EXTENSION aux_mysql UPDATE TO '1.9'")
    assert pg_query(old, "SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [('1.9',)]
    # Reuse exactly the same wire connection; invalidations must refresh caches.
    assert query('SELECT id,HEX(b),s FROM old_data') == ((1, '41', 'abc'),)
    query('CREATE TABLE new_data(id INT PRIMARY KEY,b BINARY(3),s TEXT,g BLOB AS(CONCAT(b,0x00)) STORED)')
    query('INSERT INTO new_data(id,b,s) VALUES(1,0x41,0x610162)')
    assert query('SELECT HEX(b),HEX(s),HEX(g) FROM new_data') == (
        ('410000', '610162', '41000000'),)
    query('CREATE TABLE new_key(v VARCHAR(16),PRIMARY KEY(v(2)))')
    query("INSERT INTO new_key VALUES('Ab1')")
    try:
        query("INSERT INTO new_key VALUES('ab2')")
    except pymysql.MySQLError as e:
        assert e.args[0] == 1062, e.args
    else:
        raise AssertionError('same-session upgrade failed to adopt true prefix/general_ci semantics')
    query("SET sql_mode=''")
    query('CREATE TABLE new_generated(v INT,g DECIMAL(2,0) AS(v) STORED)')
    query('INSERT INTO new_generated(v) VALUES(999)')
    assert any(r[1] == 1264 for r in query('SHOW WARNINGS'))
    assert query('SELECT g FROM new_generated') == ((99,),)
    print('PASS same-session 1.8→1.9: invalidation refresh, padded binary, native TEXT, prefix and generated coercion')
finally:
    if wire:
        wire.close()
    if changed:
        if override:
            pg_query(admin, 'ALTER SYSTEM SET mysql.database = %s', (override[0][0],))
        else:
            pg_query(admin, 'ALTER SYSTEM RESET mysql.database')
        pg_query(admin, 'SELECT pg_reload_conf()')
        restored = connect_database(original)
        restored.close()
    if old:
        old.close()
    if created:
        pg_query(admin, sql.SQL('DROP DATABASE {}').format(sql.Identifier(name)))
    admin.close()
