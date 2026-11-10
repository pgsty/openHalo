#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Pending FK authority belongs to the child owner, including pg_restore.

Requires isolated MYSQL_PORT/PGPORT and PG superuser credentials; creates and
removes two fixture roles/schemas. PGBIN selects matching dump/restore binaries.
MySQL 5.7 has no equivalent REFERENCES enforcement: this is a PG ACL boundary.
"""
import os
import pathlib
import subprocess
import tempfile
import uuid

import psycopg2
import pymysql


def run():
    tag = 'route_acl_' + uuid.uuid4().hex[:10]
    child, parent = tag + '_c', tag + '_p'
    password = 'Route.fixture.123'
    pg = psycopg2.connect(dbname=os.getenv('PGDATABASE', 'postgres'))
    pg.autocommit = True
    clients = []
    count = 0

    def sql(conn, statement):
        with conn.cursor() as q:
            q.execute(statement)
            return tuple(q.fetchall()) if q.description else ()

    def equal(actual, expected):
        nonlocal count
        assert actual == expected, (actual, expected)
        count += 1

    def rejects(conn, statement, code):
        try:
            sql(conn, statement)
        except pymysql.MySQLError as e:
            equal(e.args[0], code)
        else:
            raise AssertionError(statement)

    def ddl(conn, statement, warnings=0):
        sql(conn, statement)
        equal(tuple((w[0], w[1]) for w in sql(conn, 'SHOW WARNINGS')),
              (('Warning', 1142),) * warnings)

    def state(bound):
        equal(sql(pg, f"SELECT count(*) FROM pg_constraint WHERE connamespace='{child}'::regnamespace AND contype='f'"),
              ((2 if bound else 0,),))
        equal(sql(pg, f"SELECT count(*) FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid "
                  f"WHERE c.relnamespace='{child}'::regnamespace AND t.tgfoid='mysql.pending_fk_guard()'::regprocedure"),
              ((0 if bound else 2,),))
        equal(sql(c, f'SELECT id,a,b,HEX(note) FROM {child}.c ORDER BY id'),
              ((7, 77, 88, '610062'),))

    def roundtrip():
        env = os.environ.copy()
        pgbin = pathlib.Path(env['PGBIN'])
        with tempfile.TemporaryDirectory(prefix='route-fk-acl-') as work:
            dump = str(pathlib.Path(work) / 'child.dump')
            subprocess.run([str(pgbin / 'pg_dump'), '-Fc', '-n', child, '-f', dump,
                            env.get('PGDATABASE', 'postgres')], env=env, check=True)
            sql(pg, f'DROP SCHEMA {child} CASCADE')
            restored = subprocess.run([str(pgbin / 'pg_restore'), '--verbose', '--exit-on-error',
                '-d', env.get('PGDATABASE', 'postgres'), dump], env=env,
                capture_output=True, text=True)
            assert restored.returncode == 0, restored.stderr
            return restored.stderr

    try:
        for role in (child, parent):
            sql(pg, f"CREATE ROLE {role} LOGIN PASSWORD '{password}'")
            sql(pg, f'CREATE SCHEMA {role} AUTHORIZATION {role}')
            sql(pg, f'GRANT USAGE ON SCHEMA mysql TO {role}')
            conn = pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
                port=int(os.environ['MYSQL_PORT']), user=role, password=password,
                charset='utf8mb4', autocommit=True)
            clients.append(conn)
        c, p = clients
        sql(pg, f'GRANT USAGE ON SCHEMA {parent} TO {child}')
        sql(c, 'SET FOREIGN_KEY_CHECKS=0')
        ddl(c, f'CREATE TABLE {child}.c(id INT,a INT,b INT,note BLOB,CONSTRAINT f_explicit '
            f'FOREIGN KEY(a,b) REFERENCES {parent}.p(a,b))')
        # Implicit referenced primary-key columns must also all be authorized.
        ddl(c, f'CREATE TABLE {child}.ci(a INT,b INT,CONSTRAINT f_implicit '
            f'FOREIGN KEY(a,b) REFERENCES {parent}.p)')
        sql(c, f'INSERT INTO {child}.c VALUES(7,77,88,0x610062)')
        sql(c, 'SET FOREIGN_KEY_CHECKS=1')
        sql(pg, f'REVOKE USAGE ON SCHEMA {parent} FROM {child}')
        ddl(p, f'CREATE TABLE {parent}.p(a INT,b INT,PRIMARY KEY(a,b))', 2)
        state(False)
        sql(p, f'INSERT INTO {parent}.p VALUES(1,2)')
        # Neither an existing nor a missing key can be probed via an unbound FK.
        for values in ('1,2', '91,92'):
            rejects(c, f'INSERT INTO {child}.ci VALUES({values})', 1452)
        ddl(p, f'DELETE FROM {parent}.p')
        sql(pg, f'GRANT REFERENCES ON {parent}.p TO {child}')
        ddl(p, f'ALTER TABLE {parent}.p ADD COLUMN no_usage INT', 2)
        state(False)
        sql(pg, f'GRANT USAGE ON SCHEMA {parent} TO {child}')
        sql(pg, f'REVOKE REFERENCES ON {parent}.p FROM {child}')
        ddl(p, f'ALTER TABLE {parent}.p ADD COLUMN no_refs INT', 2)
        sql(pg, f'GRANT REFERENCES(a) ON {parent}.p TO {child}')
        ddl(p, f'ALTER TABLE {parent}.p ADD COLUMN partial_refs INT', 2)
        state(False)
        sql(pg, f'GRANT REFERENCES(b) ON {parent}.p TO {child}')
        ddl(p, f'ALTER TABLE {parent}.p ADD COLUMN all_refs INT')
        state(True)
        sql(p, f'INSERT INTO {parent}.p(a,b) VALUES(1,2)')
        ddl(c, f'INSERT INTO {child}.ci VALUES(1,2)')
        rejects(p, f'DELETE FROM {parent}.p', 1451)
        sql(c, f'DELETE FROM {child}.ci')
        # Revoked column grants and a rebuilt parent never inherit old authority.
        sql(pg, f'REVOKE REFERENCES(a,b) ON {parent}.p FROM {child}')
        sql(p, 'SET FOREIGN_KEY_CHECKS=0')
        ddl(p, f'DROP TABLE {parent}.p')
        ddl(p, f'CREATE TABLE {parent}.p(a INT,b INT,PRIMARY KEY(a,b))', 2)
        sql(p, 'SET FOREIGN_KEY_CHECKS=1')
        state(False)
        # Superuser restore must preserve pending metadata and orphan bytes,
        # without substituting the restoring user's REFERENCES privileges.
        message = roundtrip()
        equal(message.count('remains pending:'), 2)
        state(False)
        equal(sql(pg, f"SELECT DISTINCT pg_get_userbyid(relowner) FROM pg_class "
                      f"WHERE relnamespace='{child}'::regnamespace AND relkind='r'"), ((child,),))
        # Column-only grants also work when restoration triggers the binding.
        sql(pg, f'GRANT REFERENCES(a,b) ON {parent}.p TO {child}')
        equal(roundtrip().count('remains pending:'), 0)
        state(True)
        sql(p, f'INSERT INTO {parent}.p VALUES(3,4)')
        ddl(c, f'INSERT INTO {child}.ci VALUES(3,4)')
        rejects(c, f'INSERT INTO {child}.ci VALUES(3,5)', 1452)
        rejects(p, f'DELETE FROM {parent}.p', 1451)
        # Same-owner pending references retain ordinary automatic binding.
        sql(c, 'SET FOREIGN_KEY_CHECKS=0')
        ddl(c, f'CREATE TABLE {child}.own_child(a INT,FOREIGN KEY(a) REFERENCES {child}.own_parent(a))')
        ddl(c, f'CREATE TABLE {child}.own_parent(a INT PRIMARY KEY)')
        sql(c, 'SET FOREIGN_KEY_CHECKS=1')
        ddl(c, f'INSERT INTO {child}.own_parent VALUES(1)')
        ddl(c, f'INSERT INTO {child}.own_child VALUES(1)')
        rejects(c, f'INSERT INTO {child}.own_child VALUES(2)', 1452)
        print(f'PASS pending FK owner ACLs: {count} assertions; two roles, revocation, rebuild, dump/restore')
    finally:
        for conn in clients:
            conn.close()
        for role in (child, parent):
            sql(pg, f'DROP SCHEMA IF EXISTS {role} CASCADE')
        for role in (child, parent):
            if sql(pg, f"SELECT 1 FROM pg_roles WHERE rolname='{role}'"):
                sql(pg, f'DROP OWNED BY {role}')
                sql(pg, f'DROP ROLE {role}')
        pg.close()


if __name__ == '__main__':
    run()
