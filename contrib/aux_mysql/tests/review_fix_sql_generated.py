#!/usr/bin/env python3
"""Run after the parent's 1.9 build/upgrade. Uses only isolated review_fix_* DBs.

MYSQL_PWD=... python3 tests/review_fix_sql_generated.py --ports 3790 3791 3771
Needs PyMySQL. Port 3771 is the MySQL 5.7 reference. Never installs SQL objects,
changes GLOBAL settings, or restarts a server. Each case reports independently.
"""
import argparse
import json
import os
import re
import sys
import uuid

import pymysql


def run(port, args):
    db = 'review_fix_sql_' + uuid.uuid4().hex[:12]
    other = db + '_like'
    c = pymysql.connect(host=args.host, port=port, user=args.user,
                        password=args.password, charset='utf8mb4', autocommit=True)
    q = c.cursor()
    failures = []

    def sql(s):
        q.execute(s)
        return q.fetchall()

    def error(s, code):
        try:
            sql(s)
        except pymysql.MySQLError as e:
            assert e.args[0] == code, (s, e.args)
        else:
            raise AssertionError(('expected error', code, s))

    def warning():
        rows = sql('SHOW WARNINGS')
        assert any(r[0] == 'Warning' and r[1] == 1265 for r in rows), rows

    def defaults():
        sql("CREATE TABLE defaults(b VARBINARY(8) DEFAULT 0x4142, z VARBINARY(8) DEFAULT 0x00275c)")
        ddl = sql('SHOW CREATE TABLE defaults')[0][1]
        sql(ddl.replace('CREATE TABLE `defaults`', 'CREATE TABLE `defaults_copy`', 1))
        sql('INSERT INTO defaults() VALUES()')
        sql('INSERT INTO defaults_copy() VALUES()')
        assert sql('SELECT HEX(b),HEX(z) FROM defaults_copy') == (('4142', '00275C'),)
        assert sql('SELECT HEX(b),HEX(z) FROM defaults') == sql('SELECT HEX(b),HEX(z) FROM defaults_copy')

    def views():
        sql('CREATE TABLE view_base(a int,b int)')
        sql('INSERT INTO view_base VALUES(2,3)')
        sql("CREATE VIEW v AS SELECT a+b AS total,'a,b+c;d' AS literal FROM view_base WHERE a>1")
        for statement in ('SHOW CREATE TABLE v', 'SHOW CREATE VIEW v'):
            rows = sql(statement)
            assert [x[0] for x in q.description] == ['View', 'Create View', 'character_set_client', 'collation_connection']
            assert all(x[1] == 253 for x in q.description), q.description
            assert len(rows[0]) == 4
        ddl = sql('SHOW CREATE VIEW v')[0][1]
        sql(re.sub(r'VIEW `v` AS', 'VIEW `v_copy` AS', ddl, count=1))
        assert sql('SELECT * FROM v_copy') == ((5, 'a,b+c;d'),)

    def constraints():
        sql('CREATE TABLE parent(a int,b int,PRIMARY KEY(b,a),UNIQUE KEY u(a))')
        sql('CREATE TABLE child(x int,y int,CONSTRAINT fk FOREIGN KEY(y,x) REFERENCES parent(b,a) ON UPDATE CASCADE ON DELETE SET NULL)')
        q.execute("SELECT CONSTRAINT_NAME,CONSTRAINT_TYPE FROM information_schema.table_constraints WHERE TABLE_SCHEMA=%s AND TABLE_NAME='parent' ORDER BY CONSTRAINT_NAME", (db,))
        assert q.fetchall() == (('PRIMARY', 'PRIMARY KEY'), ('u', 'UNIQUE'))
        q.execute("SELECT COLUMN_NAME,ORDINAL_POSITION,POSITION_IN_UNIQUE_CONSTRAINT,REFERENCED_TABLE_SCHEMA,REFERENCED_COLUMN_NAME FROM information_schema.key_column_usage WHERE TABLE_SCHEMA=%s AND TABLE_NAME='child' ORDER BY ORDINAL_POSITION", (db,))
        assert q.fetchall() == (('y', 1, 1, db, 'b'), ('x', 2, 2, db, 'a'))
        q.execute("SELECT UNIQUE_CONSTRAINT_SCHEMA,UNIQUE_CONSTRAINT_NAME,UPDATE_RULE,DELETE_RULE,REFERENCED_TABLE_NAME FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA=%s AND TABLE_NAME='child'", (db,))
        assert q.fetchall() == ((db, 'PRIMARY', 'CASCADE', 'SET NULL', 'parent'),)
        sql('ALTER TABLE child DROP FOREIGN KEY fk')
        q.execute("SELECT COUNT(*) FROM information_schema.table_constraints WHERE TABLE_SCHEMA=%s AND TABLE_NAME='child'", (db,))
        assert q.fetchone()[0] == 0

    def recompute():
        sql('CREATE TABLE recompute(v int,g int UNIQUE)')
        sql('INSERT INTO recompute VALUES(2,99),(3,98)')
        sql('CREATE VIEW gen_dependent AS SELECT g FROM recompute')
        sql('ALTER TABLE recompute MODIFY g int GENERATED ALWAYS AS(v*2) STORED')
        assert sql('SELECT g FROM recompute ORDER BY v') == ((4,), (6,))
        assert sql('SELECT g FROM gen_dependent ORDER BY g') == ((4,), (6,))
        sql('INSERT INTO recompute(v) VALUES(4)')
        assert sql('SELECT g FROM recompute WHERE v=4') == ((8,),)
        sql('CREATE TABLE collision(v int,g int UNIQUE)')
        sql('INSERT INTO collision VALUES(1,10),(1,20)')
        error('ALTER TABLE collision MODIFY g int AS(v) STORED', 1062)
        assert sql('SELECT g FROM collision ORDER BY g') == ((10,), (20,))
        sql('INSERT INTO collision(v,g) VALUES(2,30)')
        sql('CREATE TABLE old_type(v int,g varchar(32))')
        sql("INSERT INTO old_type VALUES(3,'123')")
        sql('ALTER TABLE old_type MODIFY g int AS(v*3) STORED')
        assert sql('SELECT g FROM old_type') == ((9,),)

    def coercion():
        sql("SET SESSION sql_mode=''")
        sql('CREATE TABLE truncation(v varchar(8),g varchar(2) AS(v) STORED)')
        sql("INSERT INTO truncation(v) VALUES('abcd')")
        warning()
        assert sql('SELECT g FROM truncation') == (('ab',),)
        sql("INSERT INTO truncation(v) VALUES('你好世界')")
        warning()
        assert sql("SELECT g FROM truncation WHERE v='你好世界'") == (('你好',),)
        sql("SET SESSION sql_mode='STRICT_TRANS_TABLES'")
        error("INSERT INTO truncation(v) VALUES('abcd')", 1265)
        error("INSERT INTO truncation(v) VALUES('ab  ')", 1265)
        sql("SET SESSION sql_mode=''")
        sql("UPDATE truncation SET v='xyz'")
        warning()
        assert sql('SELECT DISTINCT g FROM truncation') == (('xy',),)

    def generated_binary():
        sql('CREATE TABLE bin(b BLOB,g BLOB AS(CONCAT(b,0x00)) STORED)')
        sql('INSERT INTO bin(b) VALUES(0x4142),(0xff)')
        assert sql('SELECT HEX(g) FROM bin ORDER BY HEX(b)') == (('414200',), ('FF00',))

    def dependencies():
        sql('CREATE TABLE chain(v int,g int GENERATED ALWAYS AS(v+1) STORED,h int AS(g+3) STORED)')
        sql('INSERT INTO chain(v) VALUES(2)')
        sql('ALTER TABLE chain MODIFY g int AS(v+2) STORED')
        assert sql('SELECT g,h FROM chain') == ((4, 7),)
        sql('ALTER TABLE chain MODIFY v bigint')
        sql('UPDATE chain SET v=5')
        assert sql('SELECT g,h FROM chain') == ((7, 10),)
        error('ALTER TABLE chain DROP g', 3108)
        sql('ALTER TABLE chain MODIFY g int')
        sql('UPDATE chain SET g=42')
        assert sql('SELECT h FROM chain') == ((45,),)

    def like():
        sql("SET SESSION sql_mode=''")
        sql('CREATE DATABASE ' + other + ' CHARACTER SET utf8mb4')
        sql('CREATE TABLE like_source(v varchar(8),g varchar(2) AS(v) VIRTUAL)')
        sql('CREATE TABLE ' + other + '.copied LIKE like_source')
        sql('USE ' + other)
        sql('DROP DATABASE ' + db)
        sql("INSERT INTO " + other + ".copied(v) VALUES('abcd')")
        warning()
        assert sql('SELECT g FROM ' + other + '.copied') == (('ab',),)
        assert 'VIRTUAL' in sql('SHOW CREATE TABLE ' + other + '.copied')[0][1]

    try:
        sql('CREATE DATABASE ' + db + ' CHARACTER SET utf8mb4')
        sql('USE ' + db)
        for test in (defaults, views, constraints, recompute, coercion, generated_binary, dependencies, like):
            try:
                test()
                print(json.dumps({'port': port, 'case': test.__name__, 'pass': True}), flush=True)
            except Exception as e:
                failures.append(test.__name__)
                print(json.dumps({'port': port, 'case': test.__name__, 'pass': False, 'error': str(e)}), flush=True)
    finally:
        sql('DROP DATABASE IF EXISTS ' + other)
        sql('DROP DATABASE IF EXISTS ' + db)
        c.close()
    return failures


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--ports', type=int, nargs='+', default=[3790, 3791])
    parser.add_argument('--user', default='dbuser_mysql')
    parser.add_argument('--password', default=os.getenv('MYSQL_PWD', ''))
    args = parser.parse_args()
    results = [run(port, args) for port in args.ports]
    sys.exit(1 if any(results) else 0)
