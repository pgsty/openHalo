#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Restart a real v18.6/1.5 cluster with new binaries, then upgrade to 1.9.

Run inside ohdev as root (kernel_instance.sh runs PostgreSQL as OS user pg).
Both prefixes must be built from their actual source versions. Work directory
must be new; data, server logs, catalogs and AB output are retained there.
The server is stopped on exit. No pre-existing database is accessed.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys

import psycopg2
from psycopg2 import sql
import pymysql
sys.dont_write_bytecode = True
from upgrade_catalog import query, compare


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--old-prefix',type=Path,required=True)
    p.add_argument('--new-prefix',type=Path,required=True)
    p.add_argument('--work-dir',type=Path,required=True)
    p.add_argument('--mysql-port',type=int,required=True)
    p.add_argument('--pg-port',type=int,required=True)
    p.add_argument('--reference-port',type=int,default=3771)
    a = p.parse_args()
    work = a.work_dir.resolve()
    work.mkdir(parents=True,exist_ok=False)
    data = work/'data'
    tests = Path(__file__).resolve().parent
    osuser = os.getenv('OSUSER','pg')
    shutil.chown(work,user=osuser)
    for port in (a.mysql_port,a.pg_port):
        with socket.socket() as sock:
            sock.bind(('0.0.0.0',port))
    old,new = a.old_prefix.resolve(),a.new_prefix.resolve()
    assert "default_version = '1.5'" in (old/'share/postgresql/extension/aux_mysql.control').read_text()
    assert "default_version = '1.9'" in (new/'share/postgresql/extension/aux_mysql.control').read_text()
    active = old
    started = False
    connections = []

    def run(args,log):
        with (work/log).open('w') as stream:
            subprocess.run([str(x) for x in args],stdout=stream,stderr=subprocess.STDOUT,check=True)

    def control(prefix,action):
        args = [prefix/'bin/pg_ctl','-D',data,'-w','-l',work/(action+'.server.log')]
        if action == 'stop': args += ['-m','fast']
        args += [action]
        run(['su',osuser,'-c',shlex.join([str(x) for x in args])],action+'.log')

    def pg(database='postgres'):
        c = psycopg2.connect(host='/tmp',port=a.pg_port,user='postgres',dbname=database)
        c.autocommit = True
        connections.append(c)
        return c

    def mysql():
        c = pymysql.connect(host='127.0.0.1',port=a.mysql_port,user='dbuser_mysql',
                            password='Mysql.123',charset='utf8mb4',autocommit=True)
        connections.append(c)
        return c

    def mq(c,statement):
        with c.cursor() as cursor:
            cursor.execute(statement)
            return tuple(cursor.fetchall())

    def snapshot(c):
        tables = ('numbers','strings','parent','child','unchecked','generated','events','bool_defaults')
        return {t:query(c,'SELECT row_to_json(snapshot_row.*)::text FROM legacy15.'+t+' snapshot_row ORDER BY id') for t in tables}

    def current_behavior(c,name):
        mq(c,'USE legacy15')
        mq(c,'CREATE TABLE '+name+'(id INT,s TEXT,b BINARY(3),g INT AS(id+1) STORED)')
        mq(c,"INSERT INTO "+name+"(id,s,b) VALUES(1,'Hello',0x41)")
        assert mq(c,"SELECT s,HEX(s),HEX(b),g,LOCATE('l',s),CAST(UNHEX('3432') AS SIGNED) FROM "+name) == (
            ('Hello','48656C6C6F','410000',2,3,42),)
        try: mq(c,'INSERT INTO '+name+'(s) VALUES(0x410042)')
        except pymysql.MySQLError as e: assert e.args[0] == 1366,e.args
        else: raise AssertionError('character NUL accepted after upgrade')

    def inherited_defaults(c):
        mq(c,'USE legacy15')
        for mode in ('', 'STRICT_TRANS_TABLES'):
            mq(c,"SET sql_mode='"+mode+"'")
            for statement in (
                "INSERT INTO bool_defaults(id,note) VALUES(20,'new') ON DUPLICATE KEY UPDATE note='changed'",
                "REPLACE INTO bool_defaults(id,note) VALUES(20,'new')",
                "INSERT INTO bool_defaults(id,note) VALUES(20,'new')",
            ):
                mq(c,'DELETE FROM bool_defaults WHERE id<>10')
                mq(c,statement)
                assert mq(c,'SELECT id,k,z,literal,note FROM bool_defaults ORDER BY id') == (
                    (10,'true','old','true','old'),(20,'1','0','true','new'))
                assert mq(c,'SHOW WARNINGS') == ()
            mq(c,"UPDATE bool_defaults SET k='other',z='x' WHERE id=20")
            mq(c,'UPDATE bool_defaults SET k=DEFAULT,z=DEFAULT WHERE id=20')
            assert mq(c,'SELECT k,z FROM bool_defaults WHERE id=20') == (('1','0'),)
            mq(c,'DELETE FROM bool_defaults WHERE id<>10')
            mq(c,"PREPARE defaults_ins FROM 'INSERT INTO bool_defaults(id,note) VALUES(?,?)'")
            for i in range(7):
                mq(c,'DELETE FROM bool_defaults WHERE id<>10')
                mq(c,"SET @i="+str(20+i)+",@note='prepared'")
                mq(c,'EXECUTE defaults_ins USING @i,@note')
                assert mq(c,'SELECT id,k,z,literal,note FROM bool_defaults WHERE id<>10') == (
                    (20+i,'1','0','true','prepared'),)
            mq(c,'DEALLOCATE PREPARE defaults_ins')
        mq(c,'DELETE FROM bool_defaults WHERE id<>10')
        mq(c,"SET sql_mode=''")

    try:
        # 1.5 has a background auto-installer; disable it so the fixture's
        # explicit CREATE EXTENSION cannot race that historical worker.
        run(['bash',tests/'kernel_instance.sh',old,data,a.pg_port,a.mysql_port,
             '--set=mysql.auto_create_extension=off'],'init-1.5.log')
        started = True
        admin = pg()
        assert query(admin,"SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [('1.5',)]
        wire = mysql()
        statements = [
            'CREATE DATABASE legacy15','USE legacy15',"SET sql_mode=''",
            'CREATE TABLE numbers(id INT PRIMARY KEY,i TINYINT,u TINYINT UNSIGNED,s SMALLINT,m MEDIUMINT,b BIGINT,bu BIGINT UNSIGNED,d DECIMAL(12,3),f FLOAT,z DOUBLE,bt BIT(8),y YEAR,dt DATE,tm TIME,ts TIMESTAMP,dd DATETIME)',
            "INSERT INTO numbers VALUES(1,-1,255,-2,-3,-4,99,12.125,1.25,2.5,b'101','2020','2020-02-29','12:34:56','2020-02-29 12:34:56','2020-02-29 12:34:56')",
            'CREATE TABLE strings(id INT PRIMARY KEY,c CHAR(5),v VARCHAR(20),t TINYTEXT,s TEXT,m MEDIUMTEXT,l LONGTEXT,b BINARY(4),vb VARBINARY(8),bb BLOB,tb TINYBLOB,mb MEDIUMBLOB,lb LONGBLOB,UNIQUE KEY prefix_v(v(3)))',
            "INSERT INTO strings VALUES(1,'x','Hello','tiny','text','medium','long',0x41,0x410042,0x00ff,0x00ff,0x00ff,0x00ff)",
            'CREATE TABLE parent(id INT PRIMARY KEY)', 'INSERT INTO parent VALUES(1)',
            'CREATE TABLE child(id INT PRIMARY KEY,pid INT,CONSTRAINT fk_child FOREIGN KEY(pid) REFERENCES parent(id) ON DELETE CASCADE)',
            'INSERT INTO child VALUES(1,1)', 'SET FOREIGN_KEY_CHECKS=0',
            'CREATE TABLE unchecked(id INT PRIMARY KEY,pid INT,CONSTRAINT fk_unchecked FOREIGN KEY(pid) REFERENCES parent(id))',
            'INSERT INTO unchecked VALUES(1,999)', 'SET FOREIGN_KEY_CHECKS=1',
            'CREATE TABLE generated(id INT PRIMARY KEY,v INT,g INT AS(v+1) STORED)',
            'INSERT INTO generated(id,v) VALUES(1,7)',
            'CREATE VIEW legacy_view AS SELECT id,v FROM generated',
            "CREATE VIEW metadata_view AS SELECT column_name,extra FROM information_schema.columns WHERE table_schema='legacy15'",
            'CREATE TABLE events(id INT PRIMARY KEY,v INT)',
            "CREATE TABLE bool_defaults(id INT PRIMARY KEY,k VARCHAR(20) UNIQUE DEFAULT TRUE,z CHAR(4) DEFAULT FALSE,literal VARCHAR(20) DEFAULT 'true',note VARCHAR(20))",
            "INSERT INTO bool_defaults VALUES(10,'true','old','true','old')",
            'CREATE PROCEDURE add_event(IN n INT) BEGIN ATOMIC INSERT INTO legacy15.events VALUES(n,n); END',
            "DO $$ BEGIN EXECUTE 'CREATE FUNCTION legacy15.event_trigger() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN NEW.v:=NEW.v+1; RETURN NEW; END $f$'; END $$ LANGUAGE plpgsql",
            'CREATE TRIGGER event_insert BEFORE INSERT ON events FOR EACH ROW EXECUTE FUNCTION event_trigger()',
            'CALL add_event(1)',
        ]
        for statement in statements: mq(wire,statement)
        assert mq(wire,'SELECT * FROM events') == ((1,2),)
        assert mq(wire,'SELECT g FROM generated') == ((None,),)
        assert query(admin,"SELECT encode(b::bytea,'hex'),encode(vb::bytea,'hex'),encode(bb::bytea,'hex') FROM legacy15.strings") == [('41','410042','00ff')]
        before = snapshot(admin)
        old_indexes = query(admin,"SELECT pg_get_indexdef(indexrelid) FROM pg_index WHERE indrelid='legacy15.strings'::regclass ORDER BY indexrelid")
        old_view_oid = query(admin,"SELECT 'legacy15.metadata_view'::regclass::oid")
        (work/'legacy-data.json').write_text(json.dumps(before,indent=2)+'\n')
        (work/'legacy-ddl.json').write_text(json.dumps(statements,indent=2)+'\n')
        print('PASS real v18.6/1.5 objects created through MySQL protocol; legacy bytes/orphan/NULL generator recorded',flush=True)
        for c in connections: c.close()
        connections.clear()
        control(old,'stop');started = False
        active = new
        control(new,'start');started = True
        admin = pg()
        assert query(admin,"SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [('1.5',)]
        wire = mysql()
        mq(wire,'USE legacy15')
        assert mq(wire,'SELECT * FROM events') == ((1,2),)
        query(admin,'ALTER EXTENSION aux_mysql UPDATE')
        assert query(admin,"SELECT extversion FROM pg_extension WHERE extname='aux_mysql'") == [('1.9',)]
        assert snapshot(admin) == before
        assert query(admin,"SELECT pg_get_indexdef(indexrelid) FROM pg_index WHERE indrelid='legacy15.strings'::regclass ORDER BY indexrelid") == old_indexes
        assert query(admin,"SELECT 'legacy15.metadata_view'::regclass::oid") == old_view_oid
        assert query(admin,"SELECT conenforced FROM pg_constraint WHERE conrelid='legacy15.unchecked'::regclass AND contype='f'") == [(False,)]
        inherited_defaults(wire)
        current_behavior(wire,'same_connection')
        mq(wire,'CALL add_event(2)')
        assert mq(wire,'SELECT * FROM events ORDER BY id') == ((1,2),(2,3))
        assert mq(wire,'SELECT * FROM legacy_view') == ((1,7),)
        assert mq(wire,'SELECT COUNT(*) FROM metadata_view')[0][0] > 0
        another_pg = pg()
        query(another_pg,'ALTER EXTENSION aux_mysql UPDATE')  # idempotent in a new connection
        current_behavior(mysql(),'new_connection')
        inherited_defaults(mysql())
        print('PASS inherited boolean defaults after real 1.5 upgrade: row identity, CHAR(4), quoted strings and prepared plans',flush=True)
        print('PASS same/new sessions after 1.5→1.9; stored procedure, trigger, dependent view and unchanged legacy data/indexes',flush=True)
        query(admin,'CREATE DATABASE upgrade_fresh19')
        fresh = pg('upgrade_fresh19')
        query(fresh,"CREATE EXTENSION aux_mysql VERSION '1.9'")
        compare(admin,fresh,work/'catalog','1.5→1.9')
        env = dict(os.environ,PGHOST='/tmp',PGPORT=str(a.pg_port),PGUSER='postgres',
                   PGDATABASE='postgres',MYSQL_HOST='127.0.0.1',MYSQL_PORT=str(a.mysql_port),
                   MYSQL_USER='dbuser_mysql',MYSQL_PASSWORD='Mysql.123',PGBIN=str(new/'bin'),
                   PSQL=str(new/'bin/psql'))
        with (work/'ab.log').open('w') as stream:
            subprocess.run([sys.executable,str(tests/'ab_regress.py')],env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
        with (work/'route-a.log').open('w') as stream:
            for wp in ('text','binary','char','collation','numeric_cast','uint64','prefix_nul','hints'):
                subprocess.run([sys.executable,str(tests/'route_a.py'),'--port',str(a.mysql_port),
                                '--reference-port',str(a.reference_port),'--wp',wp],env=env,
                               stdout=stream,stderr=subprocess.STDOUT,check=True)
            subprocess.run([sys.executable,str(tests/'route_a_fk_privileges.py')],
                           env=env,stdout=stream,stderr=subprocess.STDOUT,check=True)
        print('PASS post-upgrade AB smoke and Route A reference assertions',flush=True)
    finally:
        for c in connections: c.close()
        if started or (data/'postmaster.pid').exists(): control(active,'stop')
        print('Artifacts retained:',work,flush=True)


if __name__ == '__main__':
    main()
