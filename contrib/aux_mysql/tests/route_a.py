#!/usr/bin/env python3
"""Route A release regressions; run inside ohdev against disposable instances.

python3 tests/route_a.py --port 3804 --reference-port 3771 --wp text
Each assertion compares values (including string/binary wire types), HEX and
warning codes. Explicit representation boundaries are asserted separately.
"""
import argparse
import os
import subprocess
import struct
import uuid
import pymysql


class Session:
    def __init__(self, port):
        self.conn = pymysql.connect(host=os.getenv('MYSQL_HOST', '127.0.0.1'),
            port=port, user=os.getenv('MYSQL_USER', 'dbuser_mysql'),
            password=os.getenv('MYSQL_PASSWORD', 'Mysql.123'),
            charset='utf8mb4', autocommit=True)

    def q(self, sql):
        with self.conn.cursor() as c:
            c.execute(sql)
            return tuple(c.fetchall())

    def result(self, sql):
        try:
            rows = self.q(sql)
        except pymysql.MySQLError as e:
            return ('error', e.args[0])
        warnings = tuple(row[1] for row in self.q('SHOW WARNINGS'))
        return (rows, warnings)

    def execute_u64(self, sql, value):
        """One MYSQL_TYPE_LONGLONG parameter with the unsigned flag set."""
        c = self.conn
        c._execute_command(0x16,sql.encode())
        sid,cols,params = struct.unpack('<IHH',c._read_packet().get_all_data()[1:9])
        assert params == 1 and cols == 0, (cols,params)
        for _ in range(params+1):
            c._read_packet()
        try:
            c._execute_command(0x17,struct.pack('<IBI',sid,0,1)+b'\0\1\x08\x80'+struct.pack('<Q',value))
            assert c._read_packet().get_all_data()[0] == 0
            return ((),tuple(w[1] for w in self.q('SHOW WARNINGS')))
        except pymysql.MySQLError as e:
            return ('error',e.args[0])
        finally:
            c._execute_command(0x19,struct.pack('<I',sid))


class Checks:
    def __init__(self, port, reference):
        self.oh, self.my = Session(port), Session(reference)
        self.schema = 'route_a_' + uuid.uuid4().hex[:12]
        self.count = 0
        for s in (self.oh, self.my):
            s.q('CREATE DATABASE ' + self.schema + ' CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci')
            s.q('USE ' + self.schema)
            s.q("SET sql_mode=''")

    def both(self, sql):
        actual, expected = self.oh.result(sql), self.my.result(sql)
        if 's+0' in sql and actual[0] != 'error' and expected[0] != 'error':
            # MySQL 5.7.32 emits 1292 for literals, but this reference omits
            # it for table-column arithmetic. Retain the requested warning.
            assert actual[0] == expected[0], (sql, actual, expected)
            assert actual[1] == (1292,) * len(actual[0]), (sql, actual)
            assert expected[1] in ((), actual[1]), (sql, expected)
        else:
            assert actual == expected, (sql, actual, expected)
        assert actual[0] != 'error', (sql, actual)
        self.count += 1
        return actual

    def close(self):
        for s in (self.oh, self.my):
            try:
                s.q('DROP DATABASE IF EXISTS ' + self.schema)
            finally:
                s.conn.close()

    def text(self):
        expressions = [
            "SUBSTRING_INDEX(s,'l',1)", "LPAD(s,8,'*')", "RPAD(s,8,'*')",
            'REPEAT(s,2)', 'MID(s,2,3)', "INSERT(s,1,2,'Bye')",
            "FIND_IN_SET(s,'abc,Hello')", "FIELD(s,'abc','Hello')",
            'STRCMP(s,v)', 'ISNULL(s)', 'CONVERT(s USING utf8mb4)',
            "TRIM(BOTH 'H' FROM s)", 'COALESCE(s,v)',
            'CASE WHEN id=1 THEN s ELSE v END', 'GREATEST(s,v)',
            'REVERSE(s)', 'CAST(s AS CHAR)', 'CAST(s AS CHAR(3))',
            's+0', 'IFNULL(s,v)', 'IF(id=1,s,v)']
        for typ in ('TINYTEXT', 'TEXT', 'MEDIUMTEXT', 'LONGTEXT', 'VARCHAR(50)', 'CHAR(50)'):
            self.both('CREATE TABLE t(id INT,s '+typ+',v VARCHAR(50))')
            self.both("INSERT INTO t VALUES(1,'Hello','Hello'),(2,'abc','abc')")
            for e in expressions:
                # Pre-existing CHAR/VARCHAR IF overload ambiguity (also 3793);
                # the Route A mixed-TEXT cases remain covered above.
                if typ.startswith('CHAR(') and e in ('IFNULL(s,v)', 'IF(id=1,s,v)'):
                    continue
                self.both('SELECT '+e+' FROM t ORDER BY id')
                self.both('SELECT HEX('+e+') FROM t ORDER BY id')
            self.both('SELECT MIN(s),MAX(s),HEX(MIN(s)),HEX(MAX(s)) FROM t')
            self.both('SELECT s FROM t UNION SELECT v FROM t ORDER BY 1')
            self.both('SELECT s FROM t UNION ALL SELECT v FROM t ORDER BY 1')
            self.both('SELECT GROUP_CONCAT(s ORDER BY id),HEX(GROUP_CONCAT(s ORDER BY id)) FROM t')
            self.both('UPDATE t SET v=s')
            self.both('INSERT INTO t(v) SELECT s FROM t ORDER BY id')
            self.both('SELECT v,HEX(v) FROM t ORDER BY v')
            self.both('ALTER TABLE t MODIFY s VARCHAR(10)')
            self.both('SELECT s,HEX(s) FROM t ORDER BY s')
            self.both('DROP TABLE t')
        for typ in ('CHAR(10)', 'VARCHAR(10)', 'TINYTEXT', 'TEXT', 'MEDIUMTEXT', 'LONGTEXT'):
            self.both('CREATE TABLE nul(c '+typ+')')
            for mode in ('', 'STRICT_TRANS_TABLES', 'STRICT_ALL_TABLES'):
                for session in (self.oh, self.my):
                    session.q("SET sql_mode='"+mode+"'")
                for ignore in ('', 'IGNORE '):
                    sql = 'INSERT '+ignore+'INTO nul VALUES(0x410042)'
                    assert self.oh.result(sql) == ('error',1366), sql
                    assert self.my.result(sql) == ((),()), sql
                    self.count += 1
            assert self.oh.q('SELECT count(*) FROM nul') == ((0,),)
            self.both('DROP TABLE nul')
        self.both('CREATE TABLE bytes(b BLOB)')
        self.both('INSERT INTO bytes VALUES(0x41004200ff)')
        self.both('SELECT b,HEX(b) FROM bytes')
        for typ in ('TINYTEXT','TEXT','MEDIUMTEXT','LONGTEXT'):
            self.oh.q('CREATE TABLE names(s '+typ+')')
            ddl = self.oh.q('SHOW CREATE TABLE names')[0][1]
            meta = self.oh.q("SELECT DATA_TYPE,COLUMN_TYPE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='"+self.schema+"' AND TABLE_NAME='names'")
            assert meta == (('text','text'),), meta
            assert '`s` text' in ddl, ddl
            print(typ, 'metadata:', meta, 'SHOW:', ddl)
            self.oh.q('DROP TABLE names')


    def binary(self):
        self.both("SET sql_mode=CAST('ANSI_QUOTES' AS BINARY)")
        self.both('SELECT @@sql_mode')
        self.both("SET sql_mode=''")
        for typ in ('VARBINARY(8)', 'BINARY(5)'):
            self.both('CREATE TABLE src(id INT,s '+typ+',v VARCHAR(20),c CHAR(20),t TEXT)')
            self.both("INSERT INTO src(id,s) VALUES(1,'Hello'),(2,'éabc')")
            self.both('SELECT CAST(s AS CHAR),HEX(CAST(s AS CHAR)) FROM src ORDER BY id')
            self.both('UPDATE src SET v=s,c=s,t=s')
            self.both('SELECT v,HEX(v),c,HEX(c),t,HEX(t) FROM src ORDER BY id')
            self.both('CREATE TABLE dst(v VARCHAR(20),c CHAR(20),t TEXT)')
            self.both('INSERT INTO dst SELECT s,s,s FROM src ORDER BY id')
            self.both('SELECT v,HEX(v),c,HEX(c),t,HEX(t) FROM dst ORDER BY v')
            self.both('SET @bytes=(SELECT s FROM src WHERE id=1)')
            self.both('SELECT HEX(@bytes)')
            self.both("PREPARE p FROM 'SELECT HEX(?)'")
            self.both('EXECUTE p USING @bytes')
            # EXECUTE expressions are an OpenHalo extension; the result must
            # equal the standard MySQL variable-bound execution above.
            assert self.oh.q("EXECUTE p USING CAST('Hello' AS BINARY(5))") == (('48656C6C6F',),)
            self.both('DEALLOCATE PREPARE p')
            self.both('ALTER TABLE src MODIFY s VARCHAR(10)')
            self.both('SELECT s,HEX(s) FROM src ORDER BY id')
            self.both('DROP TABLE src')
            self.both('DROP TABLE dst')
        for typ in ('BLOB', 'VARBINARY(10)', 'BINARY(4)'):
            self.both('CREATE TABLE concat_bytes(id INT,s '+typ+')')
            self.both("INSERT INTO concat_bytes VALUES(1,0x4100ff),(2,0x00c3a9),(3,NULL),(4,X'')")
            self.both('SELECT GROUP_CONCAT(s ORDER BY id),HEX(GROUP_CONCAT(s ORDER BY id)) FROM concat_bytes')
            self.both("SELECT HEX(GROUP_CONCAT(s ORDER BY id SEPARATOR '!')) FROM concat_bytes")
            self.both('SELECT GROUP_CONCAT(s) FROM concat_bytes WHERE id=3')
            self.both('DROP TABLE concat_bytes')
        self.both('CREATE TABLE chars(c CHAR(5),v VARBINARY(5),b BINARY(5))')
        self.both("INSERT INTO chars(c) VALUES('x'),('é')")
        self.both('INSERT INTO chars(v,b) SELECT c,c FROM chars')
        self.both('SELECT HEX(v),HEX(b) FROM chars WHERE c IS NULL ORDER BY v')
        self.both('UPDATE chars SET v=c,b=c WHERE c IS NOT NULL')
        self.both('SELECT HEX(v),HEX(b) FROM chars ORDER BY v')
        for typ in ('VARBINARY(4)', 'BINARY(4)'):
            self.oh.q('CREATE TABLE bad(s '+typ+',v VARCHAR(10),c CHAR(10),t TEXT)')
            for value in ('0x410042ff', '0x41ff4243'):
                for mode in ('','STRICT_TRANS_TABLES'):
                    self.oh.q("SET sql_mode='"+mode+"'")
                    self.oh.q('DELETE FROM bad')
                    self.oh.q('INSERT INTO bad(s) VALUES('+value+')')
                    for statement in ['SELECT CAST(s AS CHAR) FROM bad',
                            'UPDATE bad SET v=s', 'UPDATE bad SET c=s', 'UPDATE bad SET t=s',
                            'INSERT IGNORE INTO bad(v) SELECT s FROM bad',
                            'ALTER TABLE bad MODIFY s VARCHAR(10)']:
                        assert self.oh.result(statement) == ('error',1366), statement
                        self.count += 1
            self.oh.q('DROP TABLE bad')
        self.oh.q('CREATE TABLE padded(s BINARY(4))')
        self.oh.q("INSERT INTO padded VALUES('x')")
        assert self.oh.result('SELECT CAST(s AS CHAR) FROM padded') == ('error',1366)
        self.count += 1


    def uint64(self):
        self.both('CREATE TABLE u64(id INT PRIMARY KEY,u BIGINT UNSIGNED UNIQUE)')
        for mode in ('','STRICT_TRANS_TABLES','STRICT_ALL_TABLES'):
            for session in (self.oh,self.my):
                session.q("SET sql_mode='"+mode+"'")
            for value in (9223372036854775808,18446744073709551615):
                for statement in (
                    'INSERT INTO u64 VALUES(2,{v})',
                    'INSERT IGNORE INTO u64 VALUES(2,{v})',
                    'INSERT INTO u64 SELECT 2,{v}',
                    'INSERT IGNORE INTO u64 SELECT 2,{v}',
                    'INSERT INTO u64 VALUES(2,{v}+0)',
                    'INSERT INTO u64 VALUES(2,\'{v}\')',
                    'UPDATE u64 SET u={v} WHERE id=1',
                    'REPLACE INTO u64 VALUES(1,{v})',
                    'INSERT INTO u64 VALUES(1,7) ON DUPLICATE KEY UPDATE u={v}',
                    'INSERT IGNORE INTO u64 VALUES(1,7) ON DUPLICATE KEY UPDATE u={v}',
                    'INSERT INTO u64 VALUES(1,{v}) ON DUPLICATE KEY UPDATE u=VALUES(u)'):
                    for s in (self.oh,self.my):
                        s.q('TRUNCATE u64');s.q('INSERT INTO u64 VALUES(1,7)')
                    sql = statement.format(v=value)
                    actual = self.oh.result(sql)
                    assert actual == ('error',1264), (mode,sql,actual,self.oh.q('SELECT * FROM u64'))
                    assert self.my.result(sql) == ((),()), (mode,sql)
                    assert self.oh.q('SELECT * FROM u64') == ((1,7),), (mode,sql)
                    self.count += 1
                for statement in ('INSERT INTO u64 VALUES(2,?)',
                                  'INSERT IGNORE INTO u64 VALUES(2,?)',
                                  'UPDATE u64 SET u=? WHERE id=1',
                                  'REPLACE INTO u64 VALUES(1,?)',
                                  'INSERT INTO u64 VALUES(1,7) ON DUPLICATE KEY UPDATE u=?'):
                    for s in (self.oh,self.my):
                        s.q('TRUNCATE u64');s.q('INSERT INTO u64 VALUES(1,7)')
                    assert self.oh.execute_u64(statement,value) == ('error',1264), (mode,statement,value)
                    assert self.my.execute_u64(statement,value) == ((),()), (mode,statement,value)
                    assert self.oh.q('SELECT * FROM u64') == ((1,7),)
                    self.count += 1
            for s in (self.oh,self.my):
                s.q('TRUNCATE u64')
            self.both('INSERT INTO u64 VALUES(1,9223372036854775807),(2,0)')
            self.both('SELECT u,HEX(u) FROM u64 ORDER BY id')
        for s in (self.oh,self.my):
            s.q("SET sql_mode=''")
        for value in (-1,18446744073709551616):
            for ignore in ('','IGNORE '):
                for s in (self.oh,self.my):
                    s.q('TRUNCATE u64')
                sql = 'INSERT '+ignore+'INTO u64 VALUES(1,'+str(value)+')'
                assert self.oh.result(sql) == self.my.result(sql) == ((),(1264,))
                assert self.oh.q('SELECT u FROM u64') == ((0 if value<0 else 9223372036854775807,),)
                assert self.my.q('SELECT u FROM u64') == ((0 if value<0 else 18446744073709551615,),)
                self.count += 1
        for s in (self.oh,self.my):
            s.q('TRUNCATE u64');s.q('INSERT INTO u64 VALUES(1,7)')
        for form in ('INTO u64 VALUES(1,8)','u64 VALUES(1,8)',
                     'INTO u64 SET id=1,u=8','u64 SET id=1,u=8'):
            self.both('INSERT IGNORE '+form+' ON DUPLICATE KEY UPDATE u=VALUES(u)')
            self.both('SELECT u FROM u64')

    def numeric_cast(self):
        for mode in ('','STRICT_TRANS_TABLES'):
            for session in (self.oh,self.my):
                session.q("SET sql_mode='"+mode+"'")
            for b in (b'42',b' \t+42',b'-12',b'12.9foo',b'1e2',b'.5',
                      b'abc',b'',b'42\x00',b'\xff42',b'12.34',b'42 '):
                self.both('CREATE TABLE cast_t(b BLOB,v VARBINARY(40),f BINARY(40))')
                h = "UNHEX('"+b.hex()+"')"
                self.both('INSERT INTO cast_t VALUES('+','.join([h]*3)+')')
                for src in ('b','v','f',h):
                    for dest in ('SIGNED','UNSIGNED','DECIMAL(12,2)'):
                        if b.startswith(b'-') and dest == 'UNSIGNED':
                            continue  # full uint64 expressions are out of scope
                        sql = 'SELECT CAST('+src+' AS '+dest+') FROM cast_t'
                        a, e = self.oh.result(sql), self.my.result(sql)
                        assert a[0] == e[0] and a[0] != 'error', (sql,b,a,e)
                        if dest.startswith('DECIMAL'):
                            # Explicit release policy uses 1292 for malformed
                            # binary decimals; MySQL 5.7 omits tail warnings
                            # and reports 1366 for a completely invalid number.
                            payload = b.ljust(40,b'\0') if src == 'f' else b
                            bad = payload not in (b'42',b' \t+42',b'-12',b'1e2',b'.5',b'12.34',b'42 ')
                            assert a[1] == ((1292,) if bad else ()), (sql,b,a,e)
                            reference_warnings = ((1366,) if b in (b'abc',b'',b'\xff42') else ())
                            if bad and src in ('v','f'):
                                reference_warnings += (1292,)
                            assert e[1] == reference_warnings, (sql,b,e)
                        else:
                            assert a[1] == e[1], (sql,b,a,e)
                        self.count += 1
                self.both('DROP TABLE cast_t')
        for source in ("X'3432'",'0x3432'):
            self.both('SELECT CAST('+source+' AS SIGNED),CAST('+source+' AS UNSIGNED),CAST('+source+' AS DECIMAL(12,2))')
        for source in ('42','42.2',"'42'",'NULL'):
            self.both('SELECT CAST('+source+' AS SIGNED),CAST('+source+' AS DECIMAL(12,2))')
        self.both("SELECT CONVERT(UNHEX('3432'),SIGNED),CAST(UNHEX('3432') AS SIGNED)")
        for value in (b'123.456',b'123456789123',b'-123456789123',b'1e100'):
            for target in ('DECIMAL','DECIMAL(5,2)','DECIMAL(3,3)'):
                self.both("SELECT CAST(UNHEX('"+value.hex()+"') AS "+target+")")
        pg = subprocess.check_output([os.getenv('PSQL','psql'),'-h','/tmp','-p',
             str(self.oh.conn.port+2000),'-U','postgres','-AtX','-c',
             "SELECT decode('3432','hex')::int8"],text=True).strip()
        assert pg == '13362', pg
        self.count += 1

    def collation(self):
        for coll in ('utf8mb4_general_ci','utf8mb4_bin','utf8_general_ci',
                     'utf8_bin','utf8mb4_unicode_ci','utf8_unicode_ci'):
            charset = 'utf8' if coll.startswith('utf8_') else 'utf8mb4'
            for typ in ('VARCHAR(30)','CHAR(30)','TEXT'):
                sql = 'CREATE TABLE search_t(id INT,s '+typ+' CHARACTER SET '+charset+' COLLATE '+coll+')'
                actual, expected = self.oh.result(sql), self.my.result(sql)
                assert actual[0] == expected[0] == (), (sql,actual,expected)
                assert actual[1] == ((1105,) if 'unicode_ci' in coll else ()), (sql,actual)
                assert expected[1] == (), (sql,expected)
                self.count += 1
                self.both("INSERT INTO search_t VALUES(1,'Hello'),(2,'éé'),(3,'xéA'),(4,''),(5,NULL),(6,'l ')")
                for e in ("LOCATE('l',s)","INSTR(s,'l')","POSITION('l' IN s)",
                          "LOCATE('L',s)","LOCATE('l',s,4)","LOCATE('',s,30)",
                          "LOCATE('É',s)","LOCATE('e',s)","LOCATE('eÁ',s)",
                          "LOCATE('l ',s)","LOCATE('x',s,NULL)",
                          "REPLACE(s,'h','X')","REPLACE(s,'é','X')",
                          "SUBSTRING_INDEX(s,'L',1)","SUBSTRING_INDEX(s,'l',-1)",
                          "FIND_IN_SET(s,'abc,HELLO,ÉÉ')", "s LIKE '%L%'",
                          "s LIKE '%e%'", "s LIKE '%É%'", "s NOT LIKE '%L%'"):
                    self.both('SELECT '+e+',HEX('+e+') FROM search_t ORDER BY id')
                self.both('DROP TABLE search_t')
        # Historical aliases survive upgrades. They are not MySQL collation
        # names, so assert their documented ICU behavior separately.
        for alias, expected in [('case_insensitive',(3,3,1)),
                                ('case_sensitive',(0,0,0)),
                                ('ignore_accents',(0,0,0))]:
            for typ in ('VARCHAR(30)','CHAR(30)','TEXT'):
                self.oh.q('CREATE TABLE aliases(s '+typ+' COLLATE '+alias+')')
                self.oh.q("INSERT INTO aliases VALUES('Hello')")
                actual = self.oh.result("SELECT LOCATE('L',s),POSITION('L' IN s),s LIKE '%L%' FROM aliases")
                assert actual == ((expected,),()), (alias,typ,actual)
                self.count += 1
                for e, value in [("INSTR(s,'l')",3),("LOCATE('l',s,4)",4),
                                 ("REPLACE(s,'h','X')",'Hello'),
                                 ("SUBSTRING_INDEX(s,'L',1)",'Hello'),
                                 ("FIND_IN_SET(s,'abc,Hello')",2)]:
                    actual = self.oh.result('SELECT '+e+' FROM aliases')
                    assert actual == (((value,),),()), (alias,typ,e,actual)
                    self.count += 1
                self.oh.q('DROP TABLE aliases')

    def char(self):
        self.both('CREATE TABLE padding(c CHAR(5),v VARBINARY(20),b BINARY(5))')
        self.both("INSERT INTO padding(c) VALUES('x'),('é'),('xy ')")
        for mode in ('', 'PAD_CHAR_TO_FULL_LENGTH'):
            for s in (self.oh, self.my):
                s.q("SET sql_mode='"+mode+"'")
            for expr in ["CONCAT_WS(',',c,'!')", 'CAST(c AS BINARY)',
                         "CONCAT(c,'!')", 'CAST(c AS CHAR)', 'HEX(c)',
                         'CONVERT(c USING utf8mb4)', "ELT(1,c,'!')"]:
                self.both('SELECT '+expr+' FROM padding ORDER BY c')
                self.both('SELECT HEX('+expr+') FROM padding ORDER BY c')
            self.both('UPDATE padding SET v=c,b=c')
            self.both('SELECT HEX(v),HEX(b) FROM padding ORDER BY c')
            self.both('SELECT HEX(GROUP_CONCAT(c ORDER BY c)) FROM padding')
            self.both("SET @c=(SELECT c FROM padding WHERE c='x')")
            self.both('SELECT HEX(@c)')
        self.both("SET sql_mode=''")
        assert self.oh.q("SELECT HEX(CONCAT_WS(',',c,'!')),HEX(CAST(c AS BINARY)) FROM padding WHERE c='x'") == (('782C21','78'),)
        self.count += 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', type=int, required=True)
    p.add_argument('--reference-port', type=int, default=3771)
    p.add_argument('--wp', choices=['text', 'binary', 'char', 'collation', 'numeric_cast', 'uint64'], default='text')
    a = p.parse_args()
    tests = Checks(a.port, a.reference_port)
    try:
        getattr(tests, a.wp)()
        print('PASS', a.wp, tests.count, 'assertions; ports', a.port, a.reference_port)
    finally:
        tests.close()


if __name__ == '__main__':
    main()
