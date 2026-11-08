#!/usr/bin/env python3
"""Integration checks: explicit --port, unique schema, session settings only.
Run after the parent installs/rebuilds 1.9 in its isolated instance.
"""
import argparse, os, struct, uuid
import pymysql

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',type=int,required=True)
p.add_argument('--host',default='127.0.0.1')
p.add_argument('--user',default='dbuser_mysql')
p.add_argument('--password',default=os.environ.get('MYSQL_PWD','Mysql.123'))
a=p.parse_args()
schema='review_fix_binary_'+uuid.uuid4().hex[:12]
c=pymysql.connect(host=a.host,port=a.port,user=a.user,password=a.password,autocommit=True,charset='utf8mb4')

def execute(q,args=None):
    with c.cursor() as cur:
        cur.execute(q,args)
        return cur.fetchall()

def equal(q,expected):
    actual=execute(q)
    assert actual==expected,(q,actual,expected)

def send(command,payload=b''):
    c._next_seq_id=0
    c._write_bytes(struct.pack('<I',len(payload)+1)[:3]+b'\0'+bytes([command])+payload)
    c._next_seq_id=1

def packet(): return c._read_packet().get_all_data()

def lenc(data,i):
    n=data[i];i+=1
    if n<251: return n,i
    width={252:2,253:3,254:8}[n]
    return int.from_bytes(data[i:i+width],'little'),i+width

def prepared_strings(sql):
    send(0x16,sql.encode())
    response=packet()
    stmt,ncols,nparams=struct.unpack('<IHH',response[1:9])
    assert nparams==0
    eof=not (c.server_capabilities & c.client_flag & (1<<24))
    for _ in range(ncols): packet()
    if ncols and eof: packet()
    try:
        send(0x17,struct.pack('<IBI',stmt,0,1))
        n,_=lenc(packet(),0)
        for _ in range(n): packet()
        if eof: packet()
        row=packet()
        assert row[0]==0
        offset=1+(n+9)//8
        values=[]
        for i in range(n):
            assert not (row[1+(i+2)//8] & (1<<((i+2)%8))), 'unexpected NULL'
            size,offset=lenc(row,offset)
            values.append(row[offset:offset+size]);offset+=size
        assert packet()[0]==254
        return values
    finally:
        send(0x19,struct.pack('<I',stmt))

mode=execute('SELECT @@SESSION.sql_mode')[0][0]
try:
    execute('CREATE DATABASE `'+schema+'`')
    execute('USE `'+schema+'`')
    equal("SELECT 0x41+'1', '1'+0x41, 0x41+0x31",((66.0,66.0,114),))
    equal("SELECT CONCAT(0x00ff,'a'),LEFT(0x00ff,1),IF(1,0x00ff,0x42)",((b'\0\xffa',b'\0',b'\0\xff'),))
    equal("SELECT CONCAT_WS(0x00,'a',NULL,'',0xff),ELT(1,0x00ff,'x'),ELT(2,0x00ff,'x')",((b'a\0\0\xff',b'\0\xff',b'x'),))
    equal("SELECT HEX(TRIM(0x20004120)),HEX(REPLACE(0x410042,'A','Z')),CHAR_LENGTH(0x00ff)",(('0041','5A0042',2),))
    execute('CREATE TABLE b (id INT PRIMARY KEY, fixed BINARY(4) UNIQUE, varying VARBINARY(3), chars TEXT)')
    execute("INSERT INTO b VALUES (1,'ab','xy',0x410142c3a9)")
    equal('SELECT HEX(fixed),HEX(varying),HEX(chars),CHAR_LENGTH(chars) FROM b',(('61620000','7879','410142C3A9',4),))
    equal('SELECT fixed,varying,chars FROM b',((b'ab\0\0',b'xy','A\1Bé'),))
    assert prepared_strings('SELECT fixed,varying,chars,CONCAT(0x00ff,\'a\') FROM b') == [b'ab\0\0',b'xy',b'A\1B\xc3\xa9',b'\0\xffa']
    equal("SELECT CONCAT_WS(',',chars,'z'),ELT(1,chars,'z') FROM b",(('A\1Bé,z','A\1Bé'),))
    assert prepared_strings("SELECT CONCAT_WS(0x00,'a',0xff),ELT(2,0x00ff,'x'),ELT(1,chars,'z') FROM b") == [b'a\0\xff',b'x',b'A\1B\xc3\xa9']
    execute('CREATE TABLE searches(t TEXT)')
    execute("INSERT INTO searches VALUES('Hello World'),(0xc3a901e7958c576f726c64)")
    equal("SELECT t RLIKE 'world$',LOCATE('WORLD',t),INSTR(t,'world'),t LIKE '%WORLD' FROM searches WHERE t='Hello World'",((1,7,7,1),))
    equal("SELECT LOCATE('WORLD',t),INSTR(t,'world') FROM searches WHERE CHAR_LENGTH(t)=8",((4,4),))
    execute('CREATE TABLE locate_weight(needle TEXT,ci TEXT COLLATE utf8mb4_general_ci,bin TEXT COLLATE utf8mb4_bin)')
    execute("INSERT INTO locate_weight(ci,bin) VALUES('a','a')")
    equal("SELECT LOCATE('a ',ci),LOCATE('a ',bin),INSTR(ci,'a ') FROM locate_weight",((0,0,0),))
    for needle,haystack,ci,bin_result in [('É','e',0,0),('s','ß',0,0),('éA','xeÁ',2,0),('ée','eeé',2,0),('é','x\1É',3,0),('\1a','é\1a\1a',2,2)]:
        nhex=needle.encode('utf8').hex();hhex=haystack.encode('utf8').hex()
        execute(f'UPDATE locate_weight SET needle=0x{nhex},ci=0x{hhex},bin=0x{hhex}')
        actual=execute('SELECT LOCATE(needle,ci),LOCATE(needle COLLATE utf8mb4_bin,bin) FROM locate_weight')
        assert actual==((ci,bin_result),),(needle,haystack,actual)
    timeout=execute("SELECT pg_catalog.current_setting('statement_timeout')")[0][0]
    try:
        execute("SELECT pg_catalog.set_config('statement_timeout','5s',false)")
        execute("UPDATE locate_weight SET ci=REPEAT('a',65535),bin=REPEAT('a',65535)")
        equal("SELECT LOCATE('z',ci),LOCATE('z',bin),INSTR(ci,'z') FROM locate_weight",((0,0,0),))
        equal("SELECT LOCATE(CONCAT(REPEAT('a',32767),'b'),ci),LOCATE(CONCAT(REPEAT('a',32767),'b'),bin) FROM locate_weight",((0,0),))
    finally:
        execute("SELECT pg_catalog.set_config('statement_timeout',%s,false)",(timeout,))
    execute('CREATE TABLE hexkeys(id VARBINARY(16) PRIMARY KEY,n INT)')
    execute('CREATE TABLE fixedkeys(id BINARY(4) PRIMARY KEY,n INT)')
    execute('INSERT INTO hexkeys VALUES(0x41000000,1),(0x410000ff,2),(0x42000000,3)')
    execute('INSERT INTO fixedkeys SELECT * FROM hexkeys')
    # Only this connection is affected. Use PG set_config through the MySQL
    # listener; the parent protocol exposes ordinary schema-qualified calls.
    seqscan=execute("SELECT pg_catalog.current_setting('enable_seqscan')")[0][0]
    try:
        execute("SELECT pg_catalog.set_config('enable_seqscan','off',false)")
        for table in ('hexkeys','fixedkeys'):
            plan=execute('EXPLAIN SELECT n FROM '+table+' WHERE id=0x410000ff')
            assert any('Index Scan' in str(row) for row in plan),plan
            equal('SELECT n FROM '+table+' WHERE id=0x410000ff',((2,),))
        equal('SELECT count(*) FROM hexkeys WHERE id>=0x410000ff AND id<0x43000000',((2,),))
    finally:
        execute("SELECT pg_catalog.set_config('enable_seqscan',%s,false)",(seqscan,))
    execute('CREATE TABLE locknames(n TEXT)')
    lockname=(schema+'_A\1Ä').encode()
    execute('INSERT INTO locknames VALUES(0x'+lockname.hex()+')')
    try:
        equal('SELECT GET_LOCK(n,0) FROM locknames',((1,),))
        equal('SELECT IS_FREE_LOCK(LOWER(n)),RELEASE_LOCK(LOWER(n)) FROM locknames',((0,1),))
    finally:
        execute('SELECT RELEASE_ALL_LOCKS()')
    execute("SET SESSION sql_mode='STRICT_TRANS_TABLES'")
    try: execute("INSERT INTO b(id,fixed) VALUES(2,'abcde')")
    except pymysql.MySQLError as e: assert e.args[0]==1406,e
    else: raise AssertionError('strict overflow accepted')
    execute("SET SESSION sql_mode=''")
    execute("INSERT INTO b VALUES(2,'abcde','abcd','z')")
    warnings=execute('SHOW WARNINGS')
    assert len([w for w in warnings if w[1]==1265])==2,warnings
    equal('SELECT HEX(fixed),HEX(varying) FROM b WHERE id=2',(('61626364','616263'),))
    execute('CREATE TABLE i (v INT PRIMARY KEY)')
    with c.cursor() as cur: cur.executemany('INSERT INTO i VALUES(%s)',[(n,) for n in range(1000)])
    equal('SELECT v FROM i WHERE v=0x41',((65,),))
    # openHalo's EXPLAIN prints a PostgreSQL plan in one column.
    plan=execute('EXPLAIN SELECT * FROM i WHERE v=0x41')
    assert any('Index' in str(row) for row in plan),plan
    print('PASS: MySQL DDL, text/binary packets, length rules, character contexts/locks and integer/binary indexes; schema '+schema)
finally:
    try:
        execute('SET SESSION sql_mode=%s',(mode,))
        execute('DROP DATABASE IF EXISTS `'+schema+'`')
    finally: c.close()
