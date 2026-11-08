#!/usr/bin/env python3
"""Compare a real 1.6 -> 1.7 -> 1.8 -> 1.9 update with a fresh 1.9 database.

PG* must target a disposable cluster with the historical 1.6 installation
script available. The test creates and drops only its own random databases.
"""
import os
import sys
import uuid

import psycopg2
from psycopg2 import sql
sys.dont_write_bytecode = True
from upgrade_catalog import query, compare


prefix = 'review_fix_upgrade_' + uuid.uuid4().hex[:8]
names = [prefix + '_old', prefix + '_new']
admin = psycopg2.connect(dbname=os.getenv('PGDATABASE', 'postgres'))
admin.autocommit = True
connections = []
created = []


try:
    for name in names:
        query(admin, sql.SQL('CREATE DATABASE {}').format(sql.Identifier(name)))
        created.append(name)
        c = psycopg2.connect(dbname=name)
        c.autocommit = True
        connections.append(c)
    old, fresh = connections
    query(old, "CREATE EXTENSION aux_mysql VERSION '1.6'")
    query(old, """CREATE SCHEMA legacy;
        CREATE TABLE legacy.parent(id int PRIMARY KEY);
        CREATE TABLE legacy.child(id int CONSTRAINT old_fk REFERENCES legacy.parent NOT ENFORCED);
        INSERT INTO legacy.child VALUES(999);
        CREATE TABLE legacy.old_types(id int,b mysql.binary);
        CREATE INDEX old_index ON legacy.old_types(id);
        INSERT INTO legacy.old_types VALUES(1,'\\x4142'::bytea);
        CREATE FUNCTION legacy.stored_upsert(integer) RETURNS void LANGUAGE sql
        BEGIN ATOMIC
          INSERT INTO legacy.parent VALUES($1) ON CONFLICT(id) DO NOTHING;
        END""")
    for version in ('1.7', '1.8', '1.9'):
        query(old, 'ALTER EXTENSION aux_mysql UPDATE TO %s', (version,))
    query(fresh, "CREATE EXTENSION aux_mysql VERSION '1.9'")
    compare(old,fresh,label="1.6→1.7→1.8→1.9",legacy_extra=True)
    assert ':allIndexes' not in query(old, "SELECT prosqlbody::text FROM pg_proc WHERE oid='legacy.stored_upsert(integer)'::regprocedure")[0][0]
    query(old, 'SELECT legacy.stored_upsert(1)')
    query(old, 'SELECT legacy.stored_upsert(1)')
    assert query(old, 'SELECT count(*) FROM legacy.parent') == [(1,)]
    assert query(old, "SELECT mysql.index_key_name('legacy.old_index'::regclass)") == [('old_index',)]
    assert query(old, "SELECT pg_catalog.encode(b::bytea,'hex') FROM legacy.old_types") == [('4142',)]
    assert query(old, "SELECT conenforced FROM pg_constraint WHERE conname='old_fk'") == [(False,)]

    # An orphaned legacy FK must fail real enforcement and remain unchanged.
    try:
        query(old, 'ALTER TABLE legacy.child ALTER CONSTRAINT old_fk ENFORCED')
    except psycopg2.errors.ForeignKeyViolation:
        pass
    else:
        raise AssertionError('legacy orphan was silently marked enforced')
    assert query(old, "SELECT conenforced FROM pg_constraint WHERE conname='old_fk'") == [(False,)]
    query(old, 'DELETE FROM legacy.child')
    query(old, 'ALTER TABLE legacy.child ALTER CONSTRAINT old_fk ENFORCED')
    assert query(old, "SELECT conenforced,convalidated FROM pg_constraint WHERE conname='old_fk'") == [(True, True)]
    print('PASS legacy objects: stored ON CONFLICT, unsuffixed index, binary bytes, orphan-safe FK enforcement')
finally:
    for c in connections:
        c.close()
    for name in created:
        query(admin, sql.SQL('DROP DATABASE {}').format(sql.Identifier(name)))
    admin.close()
