#!/usr/bin/env python3
"""Compare a real 1.6 -> 1.7 -> 1.8 -> 1.9 update with a fresh 1.9 database.

PG* must target a disposable cluster with the historical 1.6 installation
script available. The test creates and drops only its own random databases.
"""
import hashlib
import json
import os
import uuid

import psycopg2
from psycopg2 import sql


prefix = 'review_fix_upgrade_' + uuid.uuid4().hex[:8]
names = [prefix + '_old', prefix + '_new']
admin = psycopg2.connect(dbname=os.getenv('PGDATABASE', 'postgres'))
admin.autocommit = True
connections = []
created = []


def query(c, statement, values=None):
    with c.cursor() as q:
        q.execute(statement, values)
        return q.fetchall() if q.description else None


def catalog(c):
    query(c, 'SET search_path=pg_catalog')
    members = query(c, """SELECT x.type,x.schema,x.name,x.identity
        FROM pg_depend d CROSS JOIN LATERAL pg_identify_object(d.classid,d.objid,d.objsubid) x
        WHERE d.refclassid='pg_extension'::regclass AND d.refobjid=
        (SELECT oid FROM pg_extension WHERE extname='aux_mysql') AND d.deptype='e'
        ORDER BY x.type,x.schema,x.name,x.identity""")
    functions = query(c, """SELECT p.oid::regprocedure::text,pg_get_functiondef(p.oid)
        FROM pg_proc p JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
        WHERE d.refclassid='pg_extension'::regclass AND d.refobjid=
        (SELECT oid FROM pg_extension WHERE extname='aux_mysql') AND d.deptype='e'
        AND p.prokind IN ('f','p') ORDER BY 1""")
    views = query(c, """SELECT n.nspname,c.relname,pg_get_viewdef(c.oid,false)
        FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
        JOIN pg_depend d ON d.objid=c.oid AND d.classid='pg_class'::regclass
        WHERE d.refclassid='pg_extension'::regclass AND d.refobjid=
        (SELECT oid FROM pg_extension WHERE extname='aux_mysql') AND d.deptype='e'
        AND c.relkind='v' ORDER BY 1,2""")
    return {'members': members, 'functions': functions, 'views': views}


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
    a, b = catalog(old), catalog(fresh)
    for key in a:
        if a[key] != b[key]:
            only_old = sorted(set(a[key]) - set(b[key]), key=repr)
            only_new = sorted(set(b[key]) - set(a[key]), key=repr)
            raise AssertionError((key, 'upgrade-only', only_old[:5], 'fresh-only', only_new[:5]))
    digest = hashlib.sha256(json.dumps(a, ensure_ascii=True, sort_keys=True).encode()).hexdigest()
    print('PASS upgrade/fresh equality:', {k: len(v) for k, v in a.items()}, 'sha256=' + digest)
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
