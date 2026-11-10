#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Isolated pending-FK regression; run after installing the integrated 1.9 build.
MYSQL_PWD=... python3 tests/review_fix_pending_fk.py --ports 3790 3791 3771
Only session FOREIGN_KEY_CHECKS changes; every review_fix_* database is cleaned.
"""
import argparse
import json
import os
import sys
import uuid

import pymysql


def run(port, args):
    db = 'review_fix_fk_' + uuid.uuid4().hex[:12]
    parent_db = db + '_parent'
    c = pymysql.connect(host=args.host, port=port, user=args.user, password=args.password,
                        autocommit=True, charset='utf8mb4')
    q = c.cursor()
    failed = []

    def sql(s):
        q.execute(s)
        return q.fetchall()

    def error(s, code):
        try:
            sql(s)
        except pymysql.MySQLError as e:
            assert e.args[0] == code, (s, e.args)
        else:
            raise AssertionError(('accepted invalid statement', s))

    def missing_parent():
        sql('SET SESSION FOREIGN_KEY_CHECKS=0')
        sql('CREATE TABLE child(id int,x int,y int,CONSTRAINT f_child FOREIGN KEY(x,y) REFERENCES parent(a,b) ON UPDATE CASCADE ON DELETE CASCADE)')
        sql('INSERT INTO child VALUES(1,10,20)')
        assert not sql('SHOW TRIGGERS'), 'pending FK implementation leaked as a user trigger'
        ddl = sql('SHOW CREATE TABLE child')[0][1]
        assert 'CONSTRAINT `f_child` FOREIGN KEY' in ddl
        sql(ddl.replace('CREATE TABLE `child`', 'CREATE TABLE `child_copy`', 1)
               .replace('CONSTRAINT `f_child`', 'CONSTRAINT `f_child_copy`', 1))
        sql('SET SESSION FOREIGN_KEY_CHECKS=1')
        error('INSERT INTO child VALUES(2,11,20)', 1452)
        error('INSERT INTO child_copy VALUES(2,11,20)', 1452)
        sql('INSERT INTO child VALUES(2,NULL,20)')
        sql('UPDATE child SET id=3 WHERE id=1')
        sql('UPDATE child SET x=x')
        error('UPDATE child SET x=11 WHERE id=3', 1452)

    def metadata():
        q.execute("SELECT CONSTRAINT_NAME,CONSTRAINT_TYPE FROM information_schema.table_constraints WHERE TABLE_SCHEMA=%s AND TABLE_NAME='child'", (db,))
        assert q.fetchall() == (('f_child', 'FOREIGN KEY'),)
        q.execute("SELECT COLUMN_NAME,ORDINAL_POSITION,POSITION_IN_UNIQUE_CONSTRAINT,REFERENCED_TABLE_SCHEMA,REFERENCED_COLUMN_NAME FROM information_schema.key_column_usage WHERE TABLE_SCHEMA=%s AND TABLE_NAME='child' ORDER BY ORDINAL_POSITION", (db,))
        assert q.fetchall() == (('x', 1, 1, db, 'a'), ('y', 2, 2, db, 'b'))
        q.execute("SELECT UNIQUE_CONSTRAINT_NAME,UPDATE_RULE,DELETE_RULE FROM information_schema.referential_constraints WHERE CONSTRAINT_SCHEMA=%s AND TABLE_NAME='child'", (db,))
        assert q.fetchall() == ((None, 'CASCADE', 'CASCADE'),)

    def bind_and_drop():
        sql('CREATE TABLE parent(a int,b int,PRIMARY KEY(a,b))')
        # Old orphan rows survive NOT VALID binding; new invalid keys do not.
        assert sql('SELECT x,y FROM child WHERE id=3') == ((10, 20),)
        error('INSERT INTO child VALUES(4,10,20)', 1452)
        sql('INSERT INTO parent VALUES(10,20)')
        sql('INSERT INTO child VALUES(4,10,20)')
        sql('UPDATE parent SET a=11')
        assert sql('SELECT COUNT(*) FROM child WHERE x=11') == ((2,),)
        sql('SET SESSION FOREIGN_KEY_CHECKS=0')
        sql('DROP TABLE parent')
        assert 'CONSTRAINT `f_child` FOREIGN KEY' in sql('SHOW CREATE TABLE child')[0][1]
        sql('SET SESSION FOREIGN_KEY_CHECKS=1')
        error('INSERT INTO child VALUES(5,11,20)', 1452)
        error('CREATE TABLE parent(a int,b int)', 1215)
        assert not sql("SHOW TABLES LIKE 'parent'"), 'failed binding left a parent table'
        assert 'CONSTRAINT `f_child` FOREIGN KEY' in sql('SHOW CREATE TABLE child')[0][1]
        error('INSERT INTO child VALUES(5,11,20)', 1452)
        # Repeat the negative path to catch debug ErrorContext assertions and
        # error-stack corruption before retrying with a valid definition.
        error('CREATE TABLE parent(a int,b int)', 1215)
        sql('CREATE TABLE parent(a int,b int,PRIMARY KEY(a,b))')
        sql('INSERT INTO parent VALUES(11,20)')
        sql('DELETE FROM parent')
        assert sql('SELECT COUNT(*) FROM child WHERE x=11') == ((0,),)

    def rename_and_drop_fk():
        sql('SET SESSION FOREIGN_KEY_CHECKS=0')
        sql('DROP TABLE parent')
        sql('ALTER TABLE child CHANGE x renamed_x int')
        sql('RENAME TABLE child TO renamed_child')
        assert '`renamed_x`' in sql('SHOW CREATE TABLE renamed_child')[0][1]
        sql('ALTER TABLE renamed_child DROP FOREIGN KEY f_child')
        sql('SET SESSION FOREIGN_KEY_CHECKS=1')
        sql('INSERT INTO renamed_child VALUES(99,99,99)')
        q.execute("SELECT COUNT(*) FROM information_schema.table_constraints WHERE TABLE_SCHEMA=%s AND TABLE_NAME='renamed_child'", (db,))
        assert q.fetchone()[0] == 0

    def drop_database():
        sql('SET SESSION FOREIGN_KEY_CHECKS=0')
        sql('CREATE DATABASE ' + parent_db)
        sql('CREATE TABLE ' + parent_db + '.p(id int PRIMARY KEY)')
        sql('CREATE TABLE cross_child(x int,CONSTRAINT f_cross FOREIGN KEY(x) REFERENCES ' + parent_db + '.p(id))')
        sql('DROP DATABASE ' + parent_db)
        assert parent_db in sql('SHOW CREATE TABLE cross_child')[0][1]
        sql('SET SESSION FOREIGN_KEY_CHECKS=1')
        error('INSERT INTO cross_child VALUES(123)', 1452)
        sql('CREATE DATABASE ' + parent_db)
        sql('CREATE TABLE ' + parent_db + '.p(id int PRIMARY KEY)')
        error('INSERT INTO cross_child VALUES(123)', 1452)
        sql('INSERT INTO ' + parent_db + '.p VALUES(123)')
        sql('INSERT INTO cross_child VALUES(123)')

    try:
        sql('CREATE DATABASE ' + db)
        sql('USE ' + db)
        for case in (missing_parent, metadata, bind_and_drop, rename_and_drop_fk, drop_database):
            try:
                case()
                print(json.dumps({'port': port, 'case': case.__name__, 'pass': True}), flush=True)
            except Exception as e:
                failed.append(case.__name__)
                print(json.dumps({'port': port, 'case': case.__name__, 'pass': False, 'error': str(e)}), flush=True)
    finally:
        sql('SET SESSION FOREIGN_KEY_CHECKS=0')
        sql('DROP DATABASE IF EXISTS ' + parent_db)
        sql('DROP DATABASE IF EXISTS ' + db)
        c.close()
    return failed


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--ports', nargs='+', type=int, default=[3790, 3791])
    p.add_argument('--user', default='dbuser_mysql')
    p.add_argument('--password', default=os.getenv('MYSQL_PWD', ''))
    args = p.parse_args()
    results = [run(port, args) for port in args.ports]
    sys.exit(1 if any(results) else 0)
