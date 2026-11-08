#!/usr/bin/env python3
"""Exercise native and MySQL logical backups of the integrated SQL features.

Uses a unique schema and an external temporary directory. PG* / MYSQL_* select
an isolated instance; PGBIN selects its matching pg_dump/pg_restore binaries.
MYSQLDUMP and MYSQL_CLIENT select Oracle MySQL clients (5.7+). MariaDB 10.5
mysqldump includes generated values even when dumping a MySQL 5.7 source.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import uuid

import psycopg2
from psycopg2 import sql
import pymysql


schema = 'review_fix_dump_' + uuid.uuid4().hex[:10]
user = os.getenv('MYSQL_USER', 'dbuser_mysql')
password = os.getenv('MYSQL_PASSWORD', 'Mysql.123')
host = os.getenv('MYSQL_HOST', '127.0.0.1')
port = int(os.getenv('MYSQL_PORT', '3792'))
pgbin = Path(os.environ['PGBIN'])
mysqldump = os.getenv('MYSQLDUMP', 'mysqldump')
mysql_client = os.getenv('MYSQL_CLIENT', 'mysql')
version = subprocess.check_output([mysqldump, '--version'], text=True)
if 'MariaDB' in version:
    raise RuntimeError('Set MYSQLDUMP to Oracle MySQL 5.7+; MariaDB 10.5 dumps generated values')
pg = psycopg2.connect(dbname=os.getenv('PGDATABASE', 'postgres'))
pg.autocommit = True
m = pymysql.connect(host=host, port=port, user=user, password=password,
                    charset='utf8mb4', autocommit=True)


def query(statement):
    with m.cursor() as q:
        q.execute(statement)
        return q.fetchall()


def snapshot():
    return (
        query('SELECT id,HEX(b),HEX(t),HEX(g),v FROM data ORDER BY id'),
        query('SELECT * FROM view_data ORDER BY id'),
        query('SELECT k,v FROM prefix_data ORDER BY k'),
        tuple((r[2], r[4], r[7]) for r in query('SHOW INDEX FROM prefix_data')),
        query('SELECT id,pid FROM child ORDER BY id'),
        query('SELECT id,pid FROM pending_child ORDER BY id'),
    )


def verify():
    assert snapshot() == expected, (snapshot(), expected)
    try:
        query("INSERT INTO prefix_data VALUES('abcZ',99)")
    except pymysql.MySQLError as e:
        assert e.args[0] == 1062, e.args
    else:
        raise AssertionError('restored prefix PRIMARY KEY accepted a duplicate')
    query('INSERT INTO data(id,t) VALUES(3,0x610062)')
    assert query('SELECT HEX(b),HEX(t),HEX(g) FROM data WHERE id=3') == (
        ('4142', '610062', '414200'),)
    query('DELETE FROM data WHERE id=3')
    for table in ('child', 'pending_child'):
        try:
            query('INSERT INTO ' + table + ' VALUES(3,999)')
        except pymysql.MySQLError as e:
            assert e.args[0] == 1452, e.args
        else:
            raise AssertionError('restored FK accepted a new orphan in ' + table)
    query('INSERT INTO parent VALUES(1)')
    query('INSERT INTO child VALUES(4,1)')
    query('DELETE FROM parent WHERE id=1')
    assert query('SELECT COUNT(*) FROM child WHERE id=4') == ((0,),)
    query('CREATE TABLE missing_parent(id INT PRIMARY KEY)')
    query('INSERT INTO missing_parent VALUES(999)')
    query('DELETE FROM missing_parent WHERE id=999')
    assert query('SELECT COUNT(*) FROM pending_child') == ((0,),)
    query('SET FOREIGN_KEY_CHECKS=0')
    query('DROP TABLE missing_parent')
    query('INSERT INTO pending_child VALUES(1,999)')
    query('SET FOREIGN_KEY_CHECKS=1')
    assert snapshot() == expected


try:
    assert query('SELECT @@GLOBAL.gtid_mode,@@GLOBAL.gtid_executed,@@GLOBAL.gtid_purged') == (
        ('OFF', '', ''),)
    assert query("SHOW GLOBAL VARIABLES LIKE 'gtid_mode'") == (('gtid_mode', 'OFF'),)
    assert query("SHOW SESSION VARIABLES LIKE 'gtid_mode'") == (('gtid_mode', 'OFF'),)
    query('CREATE DATABASE ' + schema)
    query('USE ' + schema)
    query("CREATE TABLE data(id INT PRIMARY KEY,b VARBINARY(8) DEFAULT 0x4142,"
          "t TEXT,g BLOB AS(CONCAT(b,0x00)) STORED,v DECIMAL(10,3) DEFAULT 1.234)")
    query("INSERT INTO data(id,b,t) VALUES(1,0x00ff27,0x610062),(2,0x4142,'é')")
    query('CREATE VIEW view_data AS SELECT id,HEX(g) AS bytes FROM data')
    with pg.cursor() as q:
        q.execute(sql.SQL('ALTER VIEW {}.view_data RENAME COLUMN bytes TO renamed_bytes').format(
            sql.Identifier(schema)))
    assert '`renamed_bytes`' in query('SHOW CREATE VIEW view_data')[0][1]
    query('DROP TABLE IF EXISTS view_data')
    assert any(row[1] == 1051 for row in query('SHOW WARNINGS'))
    query('DROP VIEW IF EXISTS data')
    assert any(row[1] == 1051 for row in query('SHOW WARNINGS'))
    query('CREATE TABLE prefix_data(k VARCHAR(2000),v INT,PRIMARY KEY(k(3)))')
    query("INSERT INTO prefix_data VALUES('abc1',1),('def2',2)")
    query('SET FOREIGN_KEY_CHECKS=0')
    query('CREATE TABLE parent(id INT PRIMARY KEY)')
    query('CREATE TABLE child(id INT PRIMARY KEY,pid INT,CONSTRAINT fk_child '
          'FOREIGN KEY(pid) REFERENCES parent(id) ON DELETE CASCADE)')
    query('CREATE TABLE pending_child(id INT PRIMARY KEY,pid INT,CONSTRAINT fk_pending '
          'FOREIGN KEY(pid) REFERENCES missing_parent(id) ON DELETE CASCADE)')
    query('INSERT INTO child VALUES(1,999)')
    query('INSERT INTO pending_child VALUES(1,999)')
    query('SET FOREIGN_KEY_CHECKS=1')
    expected = snapshot()
    env = os.environ.copy()
    with tempfile.TemporaryDirectory(prefix='aux-mysql-dump-') as folder:
        dump = Path(folder) / 'native.dump'
        subprocess.run([str(pgbin / 'pg_dump'), '-Fc', '-n', schema,
                        '-f', str(dump), os.getenv('PGDATABASE', 'postgres')], check=True, env=env)
        with pg.cursor() as q:
            q.execute(sql.SQL('DROP SCHEMA {} CASCADE').format(sql.Identifier(schema)))
        subprocess.run([str(pgbin / 'pg_restore'), '-e', '-d', os.getenv('PGDATABASE', 'postgres'),
                        str(dump)], check=True, env=env)
        query('USE ' + schema)
        verify()
        print('PASS native pg_dump/pg_restore: bytes, generated values, prefix primary, pending FK and orphans')

        env['MYSQL_PWD'] = password
        dump = Path(folder) / 'mysql.sql'
        client = ['-h', host, '-P', str(port), '-u', user]
        with dump.open('wb') as stream:
            subprocess.run([mysqldump, *client, '--no-tablespaces', '--skip-lock-tables',
                            '--hex-blob', schema], stdout=stream, check=True, env=env)
        with pg.cursor() as q:
            q.execute(sql.SQL('DROP SCHEMA {} CASCADE').format(sql.Identifier(schema)))
            q.execute(sql.SQL('CREATE SCHEMA {} AUTHORIZATION {}').format(
                sql.Identifier(schema), sql.Identifier(user)))
        with dump.open('rb') as stream:
            subprocess.run([mysql_client, *client, schema], stdin=stream, check=True, env=env)
        query('USE ' + schema)
        verify()
        print('PASS mysqldump/mysql replay: tables, view, binary/NUL, pending FK, orphans and cascades')
        if os.getenv('MYSQL_REFERENCE_PORT'):
            original = m
            reference_port = int(os.environ['MYSQL_REFERENCE_PORT'])
            m = pymysql.connect(host=host, port=reference_port, user=user,
                                password=password, charset='utf8mb4', autocommit=True)
            try:
                query('CREATE DATABASE ' + schema)
                query('USE ' + schema)
                reference_client = ['-h', host, '-P', str(reference_port), '-u', user]
                with dump.open('rb') as stream:
                    subprocess.run([mysql_client, *reference_client, schema], stdin=stream,
                                   check=True, env=env)
                verify()
                print('PASS MySQL 5.7 reference restore of the same unmodified dump')
            finally:
                query('DROP DATABASE IF EXISTS ' + schema)
                m.close()
                m = original
finally:
    m.close()
    with pg.cursor() as q:
        q.execute(sql.SQL('DROP SCHEMA IF EXISTS {} CASCADE').format(sql.Identifier(schema)))
    pg.close()
