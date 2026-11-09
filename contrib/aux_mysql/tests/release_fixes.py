#!/usr/bin/env python3
"""Final 1.9 review regressions: values, row identity and wire metadata.

Run against an isolated 1.9 instance and MySQL 5.7, never a shared application
schema. Uses the same environment and unique-schema cleanup as route_a.py.
"""
import argparse
import os
import struct

import psycopg2
from psycopg2 import sql as pgsql
from pymysql import converters
from pymysql.protocol import FieldDescriptorPacket

from route_a import Checks


class ReleaseChecks(Checks):
    def find_in_set(self):
        for collation in ('utf8mb4_general_ci', 'utf8mb4_bin', 'utf8_general_ci', 'utf8_bin'):
            charset = 'utf8' if collation.startswith('utf8_') else 'utf8mb4'
            for typ in ('VARCHAR(20)', 'TEXT', 'CHAR(20)'):
                self.both('CREATE TABLE f(id INT PRIMARY KEY,s '+typ+' CHARACTER SET '+charset+
                          ' COLLATE '+collation+',items TEXT CHARACTER SET '+charset+' COLLATE '+collation+')')
                self.both("INSERT INTO f VALUES(1,'a ','a,b'),(2,'a','x,a '),(3,'a ','x,a '),"
                          "(4,'a','x,a'),(5,'A','a,b'),(6,'é','e,x'),(7,'','x,,b'),"
                          "(8,'',''),(9,NULL,'a,b'),(10,'a',NULL),(11,'a  ','a ,a  '),"
                          "(12,'a,b','a,b'),(13,'中','x,中'),(14,' ',' ,x')")
                for expr in ('FIND_IN_SET(s,items)', "FIND_IN_SET(s,'a,b')", "FIND_IN_SET(s,'x,a ')",
                             'FIND_IN_SET(1,items)', "FIELD(s,'a')", "STRCMP(s,'a')", "s LIKE 'a '",
                             "s IN ('a')", "HEX(NULLIF(s,'a'))"):
                    self.both('SELECT id,HEX(s),HEX(items),'+expr+' FROM f ORDER BY id')
                self.both('SELECT DISTINCT s FROM f ORDER BY s')
                self.both('DELETE FROM f WHERE FIND_IN_SET(s,items)')
                self.both('SELECT id,HEX(s),HEX(items) FROM f ORDER BY id')
                self.both('DROP TABLE f')
        self.both("SELECT FIND_IN_SET(1,'0,1 '),FIND_IN_SET(1,'0,1'),FIND_IN_SET('','')")

    def boolean_strings(self):
        self.both('CREATE TABLE bo(id INT PRIMARY KEY,c INT,v VARCHAR(20))')
        self.both("INSERT INTO bo VALUES(1,1,CONCAT(1=1)),(2,0,CONCAT(1=0)),(3,NULL,NULL)")
        self.both('SELECT id,v,HEX(v),CONCAT(c=1),HEX(CONCAT(c=1)) FROM bo ORDER BY id')
        for expr in ('GROUP_CONCAT(c=1 ORDER BY id)',
                     "GROUP_CONCAT(c=1 ORDER BY id SEPARATOR '|')",
                     'GROUP_CONCAT(DISTINCT c=1 ORDER BY c=1)',
                     'GROUP_CONCAT(CONCAT(c=1,c=0) ORDER BY id)'):
            self.both('SELECT '+expr+',HEX('+expr+') FROM bo')
        self.both('UPDATE bo SET v=CONCAT(c=1,c=0)')
        self.both('SELECT id,v,HEX(v),IF(c,TRUE,FALSE),IF(c=1,TRUE,FALSE) FROM bo ORDER BY id')
        self.both('SELECT IF(1,TRUE,FALSE),IF(0,TRUE,FALSE),IF(NULL,TRUE,FALSE),IF(1=1,NULL,FALSE)')
        self.both('SELECT GROUP_CONCAT(c=1) FROM bo WHERE id=3')
        self.both('DROP TABLE bo')

    def boolean_assignments(self):
        # TRUE/FALSE and predicates are MySQL's integers 1/0: numbers 1/0 and
        # strings '1'/'0' in any sql_mode.  A wrong value also selects the
        # wrong unique-key row for ON DUPLICATE KEY UPDATE and REPLACE.
        for mode in ('', 'STRICT_TRANS_TABLES'):
            for session in (self.oh, self.my):
                session.q("SET sql_mode='"+mode+"'")
            self.both('CREATE TABLE ba(id INT PRIMARY KEY,i INT,b BOOL,d DECIMAL(6,2),f DOUBLE,'
                      'v VARCHAR(20),c CHAR(4),t TEXT)')
            self.both('INSERT INTO ba VALUES(1,TRUE,TRUE,TRUE,TRUE,TRUE,FALSE,1=1),'
                      '(2,FALSE,FALSE,FALSE,FALSE,1=0,IF(0,TRUE,FALSE),IF(1,TRUE,FALSE)),'
                      '(3,NULL,NULL,NULL,NULL,NULL=1,NULL>0,IF(NULL,TRUE,FALSE))')
            self.both('UPDATE ba SET i=TRUE,b=FALSE,v=(id=3),c=CASE WHEN id=3 THEN TRUE ELSE id>5 END WHERE id=3')
            self.both('INSERT INTO ba(id,v,c,t) SELECT 10,1=1,1=0,IF(1,TRUE,FALSE)')
            self.both("INSERT INTO ba(id,v) VALUES(10,'x') ON DUPLICATE KEY UPDATE v=(id=1),t=(id=10)")
            self.both('SELECT id,i,b,d,f,v,HEX(v),c,HEX(c),t FROM ba ORDER BY id')
            self.both('DROP TABLE ba')
            for typ, seed, expr in (('INT', '0', 'TRUE'), ('BOOL', '0', 'TRUE'),
                                    ('VARCHAR(20)', "'true'", '1=1'), ('VARCHAR(20)', "'true'", 'IF(1,TRUE,FALSE)')):
                self.both('CREATE TABLE bk(id INT PRIMARY KEY,k '+typ+' UNIQUE,note VARCHAR(10))')
                for statement in ("INSERT INTO bk VALUES(20,"+expr+",'new') ON DUPLICATE KEY UPDATE note='changed'",
                                  "REPLACE INTO bk VALUES(20,"+expr+",'new')"):
                    self.both('DELETE FROM bk')
                    self.both("INSERT INTO bk VALUES(10,"+seed+",'old')")
                    self.both(statement)
                    self.both('SELECT id,k,note FROM bk ORDER BY id')
                self.both('DROP TABLE bk')
        for session in (self.oh, self.my):
            session.q("SET sql_mode=''")
        self.both('CREATE TABLE bg(id INT PRIMARY KEY,g VARCHAR(20) AS(id=1) STORED,'
                  'h TEXT AS(IF(id=1,TRUE,FALSE)) STORED,c CHAR(4) AS(id>1) VIRTUAL)')
        self.both('INSERT INTO bg(id) VALUES(1),(2)')
        self.both('SELECT id,g,HEX(g),h,c FROM bg ORDER BY id')
        self.both('DROP TABLE bg')

    def collation_order(self):
        # Comparison stops at the first differing weight, and sorts compare
        # abbreviated keys first; both must keep MySQL's weights and PAD SPACE,
        # including characters that sort below the space.
        values = ['', ' ', '  ', 'a', 'a ', 'a\t', 'a\x01', 'A', 'a b', 'a\tb', 'ab', 'aB ', 'b', '\x01', '\t',
                  '!', 'é', 'É', 'e', 'ß', 'ss', 'Æ', 'æ', 'ae', '中', 'Ａ', '\U0001F600', '\U0001F601', '�',
                  'abcdefgh', 'abcdefghi', 'ABCDEFGH ', 'abcdefg\t', 'abcdefg', 'abcdefghé', 'abcdefgÉ', 'abcdefgh\x01']
        rows = ','.join('(%d,%s)' % (i, converters.escape_string(v).join("''")) for i, v in enumerate(values))
        for collation in ('utf8mb4_general_ci', 'utf8mb4_bin'):
            self.both('CREATE TABLE co(id INT PRIMARY KEY,k VARCHAR(20) CHARACTER SET utf8mb4 COLLATE '+collation+')')
            self.both('INSERT INTO co VALUES'+rows)
            for query in ('SELECT id FROM co ORDER BY k,id', 'SELECT id FROM co ORDER BY k DESC,id',
                          'SELECT MIN(id),COUNT(*) FROM co GROUP BY k ORDER BY MIN(id)', 'SELECT COUNT(DISTINCT k) FROM co',
                          'SELECT a.id,b.id FROM co a JOIN co b ON a.k=b.k AND a.id<b.id ORDER BY 1,2'):
                self.both(query)
            self.both('CREATE INDEX co_k ON co(k)')
            self.both("SELECT id FROM co WHERE k='a' ORDER BY id")
            self.both("SELECT id FROM co WHERE k>'a' AND k<'b' ORDER BY id")
            self.both('DROP TABLE co')

    def mixed_strings(self):
        self.both('CREATE TABLE ch(i INT,c CHAR(5),v VARCHAR(8))')
        self.both("INSERT INTO ch VALUES(1,'a','A'),(2,NULL,'b '),(3,'é',NULL),(4,NULL,NULL)")
        for mode in ('', 'PAD_CHAR_TO_FULL_LENGTH'):
            self.both("SET sql_mode='"+mode+"'")
            for expr in ('IFNULL(c,v)', 'IFNULL(v,c)', 'IF(i,c,v)', 'IF(i,v,c)',
                         'IF(i=1,c,v)', 'IF(i=1,v,c)', 'IF(i+0.0,c,v)', 'IF(i+0.0,v,c)',
                         "IFNULL(c,'x')", "IFNULL(v,'x')", "IFNULL('x',c)", "IFNULL('x',v)",
                         "IF(i,c,'x')", "IF(i,v,'x')", "IF(i,'x',c)", "IF(i,'x',v)",
                         'IFNULL(c,NULL)', 'IFNULL(v,NULL)', 'IF(i,c,NULL)', 'IF(i,v,NULL)'):
                self.both('SELECT i,'+expr+',HEX('+expr+') FROM ch ORDER BY i')
            self.both('CREATE TABLE ch_copy(v VARCHAR(20))')
            self.both('INSERT INTO ch_copy SELECT IFNULL(c,v) FROM ch')
            self.both('SELECT v,HEX(v) FROM ch_copy ORDER BY v')
            self.both('DROP TABLE ch_copy')
        self.both("SET sql_mode=''")
        self.both('DROP TABLE ch')

    def connection_init(self):
        variables = ('character_set_client', 'character_set_results', 'character_set_connection', 'collation_connection')
        query = 'SELECT '+','.join('@@'+v for v in variables)
        for statement, expected in (
            ('SET NAMES utf8mb4 COLLATE utf8mb4_general_ci', ('utf8mb4',)*3+('utf8mb4_general_ci',)),
            ("SET NAMES 'utf8mb4' COLLATE 'utf8mb4_bin'", ('utf8mb4',)*3+('utf8mb4_bin',)),
            ('SET NAMES utf8 COLLATE utf8_bin', ('utf8',)*3+('utf8_bin',)),
            ("SET NAMES 'utf8'", ('utf8',)*3+('utf8_general_ci',)),
            ('SET NAMES utf8mb4', ('utf8mb4',)*3+('utf8mb4_general_ci',)),
        ):
            self.both(statement)
            assert self.both(query)[0] == (expected,), (statement,expected)
        for spelling in ('CHARACTER SET','CHARSET'):
            for value in ('utf8', "'utf8mb4'", 'DEFAULT'):
                for session in (self.oh,self.my):
                    defaults = session.q('SELECT @@character_set_database,@@collation_database,@@global.character_set_server')[0]
                    result = session.result('SET '+spelling+' '+value)
                    assert result == ((),()), (spelling,value,result)
                    charset = defaults[2] if value == 'DEFAULT' else value.strip("'")
                    assert session.q(query) == ((charset,charset,defaults[0],defaults[1]),)
                    self.count += 1
        for session in (self.oh,self.my):
            defaults = session.q('SELECT @@global.character_set_server,@@global.collation_server')[0]
            assert session.result('SET NAMES DEFAULT') == ((),())
            assert session.q(query) == ((defaults[0],)*3+(defaults[1],),)
            self.count += 1
        self.both('SET NAMES utf8mb4 COLLATE utf8mb4_general_ci')
        self.both("SELECT HEX('é中'),@@character_set_connection,@@collation_connection")
        # Non-UTF8 encoding and locale mapping is explicit; no false claim of
        # transcoding, and literal-collation propagation remains deferred.
        assert self.oh.result('SET NAMES latin1 COLLATE latin1_swedish_ci') == ((),(1105,1105))
        assert self.oh.q(query) == (('utf8mb4','utf8mb4','utf8mb4','utf8mb4_general_ci'),)
        self.count += 1
        for requested, effective in (('utf8mb4_unicode_ci','utf8mb4_general_ci'),
                                     ('latin2_czech_cs','utf8mb4_bin')):
            assert self.oh.result('SET NAMES utf8mb4 COLLATE '+requested) == ((),(1105,))
            assert self.oh.q(query) == (('utf8mb4','utf8mb4','utf8mb4',effective),)
            self.count += 1
        self.both('SET NAMES utf8mb4')

    def generated_strings(self):
        self.both('CREATE TABLE generated_strings(id INT,c CHAR(5),v VARCHAR(8),'
                  'g VARCHAR(20) AS(IFNULL(c,v)) STORED,'
                  'h VARCHAR(20) AS(IF(id>1,c,v)) STORED,'
                  'b VARCHAR(20) AS(CONCAT(IF(id>1,TRUE,FALSE))) STORED)')
        self.both("INSERT INTO generated_strings(id,c,v) VALUES(1,'a','b'),(2,NULL,'x ')")
        self.both('SELECT id,g,HEX(g),h,HEX(h),b,HEX(b) FROM generated_strings ORDER BY id')
        self.both("UPDATE generated_strings SET c='é' WHERE id=2")
        self.both('SELECT id,g,HEX(g),h,HEX(h),b,HEX(b) FROM generated_strings ORDER BY id')
        statement = ('CREATE TABLE bad_generated(id INT,c CHAR(5),g VARCHAR(20) '
                     'AS(IF(id>1,c,CONCAT(RAND()))) STORED)')
        for session in (self.oh,self.my):
            assert session.result(statement) == ('error',3102)
        self.count += 1
        self.both('DROP TABLE generated_strings')

    def metadata(self):
        self.both('CREATE TABLE wire(c CHAR(4),v VARCHAR(8),t TEXT,b BLOB,f BINARY(4),r VARBINARY(8))')
        self.both("INSERT INTO wire VALUES('é','中','é中',UNHEX('4100ff'),UNHEX('4100ff'),UNHEX('4100ff'))")
        query = 'SELECT c,v,t,b,f,r FROM wire'
        self.both(query)
        def fields(session):
            session.q(query)
            return [(f.type_code,f.charsetnr) for f in session.conn._result.fields]
        expected = [(254,45),(253,45),(252,45),(252,63),(254,63),(253,63)]
        assert fields(self.my) == fields(self.oh) == expected
        for session in (self.oh,self.my):
            c = session.conn
            c._execute_command(0x16,query.encode())
            packet = c._read_packet().get_all_data()
            sid,cols,params = struct.unpack('<IHH',packet[1:9])
            assert cols == 6 and params == 0
            try:
                meta = [c._read_packet(FieldDescriptorPacket) for _ in range(cols)]
                assert c._read_packet().get_all_data()[0] == 254
                assert [(f.type_code,f.charsetnr) for f in meta] == expected
                c._execute_command(0x17,struct.pack('<IBI',sid,0,1))
                assert c._read_packet().get_all_data()[0] == cols
                meta = [c._read_packet(FieldDescriptorPacket) for _ in range(cols)]
                assert c._read_packet().get_all_data()[0] == 254
                assert [(f.type_code,f.charsetnr) for f in meta] == expected
                row = c._read_packet().get_all_data()
                assert row == b'\0\0\x02\xc3\xa9\x03\xe4\xb8\xad\x05\xc3\xa9\xe4\xb8\xad\x03A\0\xff\x04A\0\xff\0\x03A\0\xff', row.hex()
                assert c._read_packet().get_all_data()[0] == 254
                self.count += 1
            finally:
                c._execute_command(0x19,struct.pack('<I',sid))
        self.both('DROP TABLE wire')

    def overflow_atomicity(self, port):
        self.oh.q('CREATE TABLE atomic_u(id INT PRIMARY KEY,u BIGINT UNSIGNED UNIQUE,v VARCHAR(20))')
        self.oh.q('CREATE TABLE audit_u(id INT)')
        pg = psycopg2.connect(host=os.getenv('PGHOST','/tmp'),port=os.getenv('PGPORT',str(port+2000)),
                             user=os.getenv('PGUSER','postgres'),dbname=os.getenv('PGDATABASE','postgres'),
                             password=os.getenv('PGPASSWORD','Mysql.123'))
        pg.autocommit = True
        try:
            with pg.cursor() as c:
                c.execute(pgsql.SQL('CREATE FUNCTION {}.audit_insert() RETURNS trigger LANGUAGE plpgsql AS '
                    "'BEGIN INSERT INTO {}.audit_u VALUES(NEW.id); RETURN NEW; END'").format(
                        pgsql.Identifier(self.schema),pgsql.Identifier(self.schema)))
                c.execute(pgsql.SQL('CREATE TRIGGER audit_insert AFTER INSERT ON {}.atomic_u FOR EACH ROW '
                    'EXECUTE FUNCTION {}.audit_insert()').format(pgsql.Identifier(self.schema),pgsql.Identifier(self.schema)))
            self.oh.q("INSERT INTO atomic_u VALUES(1,9223372036854775807,'original')")
            self.oh.q('BEGIN')
            self.oh.q("INSERT INTO atomic_u VALUES(2,2,'prior')")
            for statement in (
                "INSERT INTO atomic_u VALUES(3,3,'partial'),(4,18446744073709551616,'overflow')",
                "INSERT IGNORE INTO atomic_u VALUES(3,3,'partial'),(4,18446744073709551616,'overflow')",
                "REPLACE INTO atomic_u VALUES(3,3,'partial'),(4,18446744073709551616,'overflow')",
                "INSERT INTO atomic_u VALUES(3,3,'partial'),(4,18446744073709551616,'overflow') ON DUPLICATE KEY UPDATE v=VALUES(v)",
            ):
                assert self.oh.result(statement) == ('error',1264), statement
                assert self.oh.q('SELECT id,u,v FROM atomic_u ORDER BY id') == ((1,9223372036854775807,'original'),(2,2,'prior'))
                assert self.oh.q('SELECT id FROM audit_u ORDER BY id') == ((1,),(2,))
                self.count += 1
            self.oh.q("INSERT INTO atomic_u VALUES(5,5,'after')")
            self.oh.q('COMMIT')
            assert self.oh.q('SELECT id FROM atomic_u ORDER BY id') == ((1,),(2,),(5,))
            assert self.oh.q('SELECT id FROM audit_u ORDER BY id') == ((1,),(2,),(5,))
        finally:
            self.oh.q('ROLLBACK')
            pg.close()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,default=int(os.getenv('MYSQL_PORT','3306')))
    p.add_argument('--reference-port',type=int,default=int(os.getenv('MYSQL_REFERENCE_PORT','3771')))
    a=p.parse_args()
    checks=ReleaseChecks(a.port,a.reference_port)
    try:
        for name in ('find_in_set','boolean_strings','boolean_assignments','collation_order','mixed_strings',
                     'connection_init','generated_strings','metadata'):
            getattr(checks,name)()
            print('PASS',name,checks.count,flush=True)
        checks.overflow_atomicity(a.port)
        print('PASS overflow_atomicity; total',checks.count,flush=True)
    finally:
        checks.close()


if __name__ == '__main__':
    main()
