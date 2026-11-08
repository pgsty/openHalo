#!/usr/bin/env python3
"""Wire regression for an integrated 1.9 server or MySQL 5.7 reference.

Needs pymysql. MYSQL_HOST/PORT/USER/PASSWORD select the server. Uses only a
unique review_fix_* schema and session state, and drops the schema on exit.
Example: docker exec -i -e MYSQL_PORT=3790 ohdev python3 < tests/indexes_mysql.py
"""
import hashlib
import os
import time
import pymysql

schema = 'review_fix_indexes_' + format(time.time_ns(), 'x')
c = pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
    port=int(os.getenv('MYSQL_PORT', '3790')), user=os.getenv('MYSQL_USER', 'dbuser_mysql'),
    password=os.getenv('MYSQL_PASSWORD', 'Mysql.123'), charset='utf8mb4',
    autocommit=True, binary_prefix=True)

def q(sql, args=None):
    with c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchall()

def duplicate(sql, args=None):
    try: q(sql, args)
    except pymysql.MySQLError as e: assert e.args[0] == 1062, e
    else: raise AssertionError('duplicate accepted: ' + sql)

a = bytes.fromhex('d131dd02c5e6eec4693d9a0698aff95c2fcab58712467eab4004583eb8fb7f8955ad340609f4b30283e488832571415a085125e8f7cdc99fd91dbdf280373c5bd8823e3156348f5bae6dacd436c919c6dd53e2b487da03fd02396306d248cda0e99f33420f577ee8ce54b67080a80d1ec69821bcb6a8839396f9652b6ff72a70')
b = bytes.fromhex('d131dd02c5e6eec4693d9a0698aff95c2fcab50712467eab4004583eb8fb7f8955ad340609f4b30283e4888325f1415a085125e8f7cdc99fd91dbd7280373c5bd8823e3156348f5bae6dacd436c919c6dd53e23487da03fd02396306d248cda0e99f33420f577ee8ce54b67080280d1ec69821bcb6a8839396f965ab6ff72a70')
assert hashlib.md5(a).hexdigest() == hashlib.md5(b).hexdigest() == '79054025255fb1a26e4bc422aef54eb4'
try:
    q('CREATE DATABASE ' + schema)
    q('USE ' + schema)
    q('CREATE TABLE shortkey(a varchar(20),b int,UNIQUE KEY u(a(3),b)) COLLATE=utf8mb4_general_ci')
    q("INSERT INTO shortkey VALUES ('abc1',1),('abc2',2)")
    duplicate("INSERT INTO shortkey VALUES ('ABC9',1)")
    q('CREATE TABLE pk(a varchar(20),b int,PRIMARY KEY(a(3),b)) COLLATE=utf8mb4_general_ci')
    q("INSERT INTO pk VALUES ('abc1',1)")
    duplicate("INSERT INTO pk VALUES ('abc2',1)")
    for table in ('pk', 'shortkey'):
        keys = q('SHOW INDEX FROM ' + table)
        assert len(keys) == 2 and [r[7] for r in keys] == [3, None], keys
        assert {r[10].upper() for r in keys} == {'BTREE'}, keys
    assert {r[2] for r in q('SHOW INDEX FROM pk')} == {'PRIMARY'}
    q('CREATE TABLE pk_copy LIKE pk')
    assert {r[2] for r in q('SHOW INDEX FROM pk_copy')} == {'PRIMARY'}
    q("INSERT INTO pk_copy VALUES ('abc1',1)")
    duplicate("INSERT INTO pk_copy VALUES ('abc2',1)")
    print('PASS short/primary prefix uniqueness, PRIMARY/BTREE metadata and CREATE TABLE LIKE')

    q('CREATE TABLE collision(k blob,value int,UNIQUE KEY u(k(3000)))')
    q('INSERT INTO collision VALUES (%s,1),(%s,2)', (a,b))
    assert q('SELECT count(*) FROM collision') == ((2,),)
    q('INSERT INTO collision VALUES (%s,7) ON DUPLICATE KEY UPDATE value=VALUES(value)', (b,))
    assert q('SELECT value FROM collision ORDER BY value') == ((1,),(7,))
    q('REPLACE INTO collision VALUES (%s,8)', (b,))
    assert q('SELECT value FROM collision ORDER BY value') == ((1,),(8,))
    duplicate('INSERT INTO collision VALUES (%s,9)', (a,))
    print('PASS Wang MD5 collision survives INSERT, ON DUPLICATE and true REPLACE')

    q('CREATE TABLE ci(a varchar(20),UNIQUE KEY u(a)) COLLATE=utf8mb4_general_ci')
    q("INSERT INTO ci VALUES ('e')")
    for value in ('é', 'E ', 'e   '): duplicate('INSERT INTO ci VALUES (%s)', (value,))
    q('CREATE TABLE planparts(a text,b int,KEY ab(a(10),b)) COLLATE=utf8mb4_general_ci')
    q("INSERT INTO planparts VALUES ('éx',1),('ex',2),('e ',1),('e',1)")
    keys=q('SHOW INDEX FROM planparts')
    assert len(keys)==2 and [r[4] for r in keys]==['a','b'] and [r[7] for r in keys]==[10,None],keys
    assert {r[10].upper() for r in keys}=={'BTREE'},keys
    assert q("SELECT count(*) FROM planparts WHERE a LIKE 'E%' AND b=1")==((3,),)
    assert q("SELECT count(*) FROM planparts WHERE a LIKE 'e' AND b=1")==((1,),)
    print('PASS GENERAL_CI, PAD SPACE, LIKE and complete composite KEY')
finally:
    q('DROP DATABASE IF EXISTS ' + schema)
    c.close()
