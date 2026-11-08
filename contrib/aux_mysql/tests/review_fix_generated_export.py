#!/usr/bin/env python3
"""Bounded generated assignment/export checks for integrated 1.9 and MySQL 5.7.
MYSQL_PWD=... python3 tests/review_fix_generated_export.py --ports 3792 3793 3771
Only unique review_fix_* schemas and session variables; no install or global SET.
"""
import argparse
import json
import os
import re
import sys
import uuid

import pymysql


def run(port, args):
    schema = 'review_fix_gen_export_' + uuid.uuid4().hex[:10]
    c = pymysql.connect(host=args.host, port=port, user=args.user,
                        password=args.password, autocommit=True, charset='utf8mb4')
    q = c.cursor()
    failed = []

    def sql(statement, params=None):
        q.execute(statement, params)
        return q.fetchall()

    def warning(code):
        rows = sql('SHOW WARNINGS')
        assert any(row[0] == 'Warning' and row[1] == code for row in rows), rows

    def error(statement, code):
        try:
            sql(statement)
        except pymysql.MySQLError as e:
            assert e.args[0] == code, (statement, e.args)
        else:
            raise AssertionError(('expected error', code, statement))

    def numeric_assignment():
        sql("SET SESSION sql_mode=''")
        sql('CREATE TABLE num(v int,g decimal(2,0) AS(v) STORED)')
        sql("PREPARE num_insert FROM 'INSERT INTO num(v) VALUES(?)'")
        sql('SET @num=999')
        sql('EXECUTE num_insert USING @num')
        warning(1264)
        assert sql('SELECT v,g FROM num') == ((999, 99),)
        sql("SET SESSION sql_mode='STRICT_TRANS_TABLES'")
        error('EXECUTE num_insert USING @num', 1264)
        assert sql('SELECT COUNT(*) FROM num') == ((1,),)
        sql("PREPARE num_ignore FROM 'INSERT IGNORE INTO num(v) VALUES(?)'")
        sql('SET @num=-999')
        sql('EXECUTE num_ignore USING @num')
        warning(1264)
        assert sql('SELECT v,g FROM num ORDER BY v') == ((-999, -99), (999, 99))
        sql("SET SESSION sql_mode=''")
        sql('EXECUTE num_insert USING @num')
        warning(1264)
        assert sql('SELECT COUNT(*) FROM num WHERE v=-999 AND g=-99') == ((2,),)
        sql('DEALLOCATE PREPARE num_insert')
        sql('DEALLOCATE PREPARE num_ignore')
        sql('CREATE TABLE scaled(v decimal(8,3),g decimal(3,1) AS(v) STORED)')
        sql('INSERT INTO scaled(v) VALUES(99.95),(-99.95),(NULL)')
        warning(1264)
        rows = sql('SELECT g FROM scaled ORDER BY v IS NULL,v')
        assert tuple(None if r[0] is None else str(r[0]) for r in rows) == ('-99.9', '99.9', None)
        assert 'generated_numeric' not in sql('SHOW CREATE TABLE num')[0][1]

    def varchar_ignore():
        sql("SET SESSION sql_mode='STRICT_TRANS_TABLES'")
        sql('CREATE TABLE chars(s varchar(8),g varchar(2) AS(s) STORED)')
        sql("PREPARE char_insert FROM 'INSERT INTO chars(s) VALUES(?)'")
        sql("PREPARE char_ignore FROM 'INSERT IGNORE INTO chars(s) VALUES(?)'")
        sql("SET @txt='abcd'")
        error('EXECUTE char_insert USING @txt', 1265)
        sql('EXECUTE char_ignore USING @txt')
        warning(1265)
        assert sql('SELECT s,g FROM chars') == (('abcd', 'ab'),)
        sql("SET SESSION sql_mode=''")
        sql('EXECUTE char_insert USING @txt')
        warning(1265)
        assert sql('SELECT COUNT(*) FROM chars WHERE s=%s AND g=%s', ('abcd', 'ab')) == ((2,),)
        sql("SET SESSION sql_mode='STRICT_TRANS_TABLES'")
        error('EXECUTE char_insert USING @txt', 1265)
        sql('DEALLOCATE PREPARE char_insert')
        sql('DEALLOCATE PREPARE char_ignore')

    def export_helpers():
        sql('CREATE TABLE export_src(b VARBINARY(8),n INT,s VARCHAR(20),'
            'g BLOB AS(CONCAT(b,0x00)) STORED,'
            'nested BLOB AS(CONCAT(CONCAT(b,0x00),s)) STORED,'
            'ws BLOB AS(CONCAT_WS(0x00,b,s,NULL)) STORED,'
            'choice BLOB AS(ELT(n,b,0x00,s)) STORED) COLLATE=utf8mb4_bin')
        ddl = sql('SHOW CREATE TABLE export_src')[0][1]
        assert not re.search(r'\b(concat_pair|concat_one|string_result|concat_ws_bytes|elt_index|raw_bytes)\b', ddl), ddl
        assert 'case_sensitive' not in ddl and 'case_insensitive' not in ddl, ddl
        assert re.search(r'\bCOLLATE\s*=\s*utf8mb4_bin\b', ddl, re.I), ddl
        assert re.search(r'\bCONCAT_WS\s*\(', ddl, re.I) and re.search(r'\bELT\s*\(', ddl, re.I), ddl
        sql(ddl.replace('CREATE TABLE `export_src`', 'CREATE TABLE `export_copy`', 1))
        for table in ('export_src', 'export_copy'):
            sql('INSERT INTO '+table+"(b,n,s) VALUES(0x4142,2,'xy')")
            assert sql('SELECT HEX(g),HEX(nested),HEX(ws),HEX(choice) FROM '+table) == (('414200', '4142007879', '4142007879', '00'),)
        sql('CREATE TABLE coll_ci(s varchar(3)) COLLATE=utf8mb4_general_ci')
        ci = sql('SHOW CREATE TABLE coll_ci')[0][1]
        assert 'case_insensitive' not in ci and 'case_sensitive' not in ci, ci
        # MySQL may omit its default collation; an emitted name must be portable.
        assert not re.search(r'\bCOLLATE\s*=\s*(?!utf8mb4_general_ci\b)\w+', ci, re.I), ci

    def view_status():
        sql('CREATE VIEW export_v AS SELECT 1 AS n')
        cols = ('ENGINE,TABLE_TYPE,VERSION,ROW_FORMAT,TABLE_ROWS,AVG_ROW_LENGTH,'
                'DATA_LENGTH,MAX_DATA_LENGTH,INDEX_LENGTH,DATA_FREE,AUTO_INCREMENT,'
                'TABLE_COLLATION,CREATE_OPTIONS,TABLE_COMMENT')
        rows = sql('SELECT '+cols+' FROM information_schema.tables WHERE table_schema=%s AND table_name=%s', (schema, 'export_v'))
        assert rows == ((None, 'VIEW') + (None,)*11 + ('VIEW',),), rows

    try:
        sql('CREATE DATABASE '+schema)
        sql('USE '+schema)
        for case in (numeric_assignment, varchar_ignore, export_helpers, view_status):
            try:
                case()
                print(json.dumps({'port': port, 'case': case.__name__, 'pass': True}), flush=True)
            except Exception as e:
                failed.append(case.__name__)
                print(json.dumps({'port': port, 'case': case.__name__, 'pass': False, 'error': str(e)}), flush=True)
    finally:
        sql('DROP DATABASE IF EXISTS '+schema)
        c.close()
    return failed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--ports', type=int, nargs='+', default=[3792, 3793])
    parser.add_argument('--user', default='dbuser_mysql')
    parser.add_argument('--password', default=os.getenv('MYSQL_PWD', ''))
    args = parser.parse_args()
    results = [run(port, args) for port in args.ports]
    sys.exit(1 if any(results) else 0)
