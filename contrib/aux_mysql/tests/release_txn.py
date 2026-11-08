#!/usr/bin/env python3
"""Release transaction regressions (PyMySQL; no installation or restart).

Run session-only checks on a reference or integrated server:
  docker exec -i ohdev python3 - --ports 3771,3790,3791 < tests/release_txn.py

Only the integrator should opt into GLOBAL tests, on an isolated instance:
  python3 tests/release_txn.py --ports 3792 --pg-port 5792 --allow-global-read-only
The opt-in suite needs psql for PostgreSQL-only fixtures, and restores both
global flags in finally.  The default suite NEVER writes a global variable.
Every run uses a unique review_fix_* schema and cleans up in finally.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import uuid

import pymysql
from pymysql.constants import CLIENT


# PyMySQL discards server_status in EOF packets.  Preserve it before its
# wrapper advances the packet cursor; OK status is already preserved.
_original_eof = pymysql.connections.MySQLResult._check_packet_is_eof


def record_eof(self, packet):
    data = packet.get_all_data()
    result = _original_eof(self, packet)
    if result:
        self.server_status = struct.unpack_from('<H', data, 3)[0]
    return result


pymysql.connections.MySQLResult._check_packet_is_eof = record_eof
STATUS_MASK = 1 | 2 | 8 | 0x2000  # IN_TRANS, AUTOCOMMIT, MORE, IN_TRANS_READONLY


class Suite:
    def __init__(self, args, port):
        self.args, self.port = args, port
        self.schema = 'review_fix_txn_' + uuid.uuid4().hex[:16]
        self.failures = self.checks = 0
        self.last_results = []

    def connect(self, database=None):
        return pymysql.connect(host=self.args.host, port=self.port,
                               user=self.args.user, password=self.args.password,
                               database=database, autocommit=True,
                               client_flag=CLIENT.MULTI_STATEMENTS,
                               connect_timeout=10, read_timeout=30, write_timeout=30)

    def check(self, label, actual, expected):
        self.checks += 1
        ok = actual == expected
        print(f'{self.port} {"ok" if ok else "FAIL"} {label}' +
              ('' if ok else f': got {actual!r}, expected {expected!r}'), flush=True)
        self.failures += not ok

    def query(self, conn, sql):
        self.last_results = []
        with conn.cursor() as cur:
            cur.execute(sql)
            while True:
                self.last_results.append((cur.fetchall(), cur._result.server_status))
                if not cur.nextset():
                    break
        return self.last_results

    def one(self, conn, sql):
        return self.query(conn, sql)[-1][0]

    def flags(self, conn, sql, expected):
        results = self.query(conn, sql)
        self.check(sql + ' status', [r[1] & STATUS_MASK for r in results], expected)
        return results

    def error(self, conn, sql, errno):
        try:
            self.query(conn, sql)
        except pymysql.MySQLError as exc:
            self.check(sql + ' error', exc.args[0], errno)
        else:
            self.check(sql + ' error', 'success', errno)

    def reset(self, conn):
        conn._execute_command(0x1f, b'')  # COM_RESET_CONNECTION
        return conn._read_ok_packet().server_status

    def case(self, name, callback):
        conn = self.connect(self.schema)
        try:
            self.query(conn, 'SET SESSION TRANSACTION READ WRITE')
            self.query(conn, 'SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ')
            self.query(conn, 'DELETE FROM t')
            callback(conn)
        except Exception as exc:
            self.check(name + ' completed', repr(exc), 'success')
        finally:
            try:
                self.query(conn, 'ROLLBACK')
            finally:
                conn.close()

    def session_cases(self):
        def flags(c):
            self.flags(c, 'SET @x=1; SELECT 2', [0xa, 0x2])
            self.flags(c, 'BEGIN; SET @x=2; SELECT 3; COMMIT', [0xb, 0xb, 0xb, 0x2])
            self.flags(c, 'SET autocommit=0; SELECT 1; SET @x=2; SELECT 3', [8, 8, 8, 0])
            self.flags(c, 'SELECT * FROM t; SET @x=3; ROLLBACK', [9, 9, 0])
            self.flags(c, 'START TRANSACTION READ ONLY', [0x2001])
            self.flags(c, 'SELECT 1; ROLLBACK', [0x2009, 0])

        def lazy(c):
            self.flags(c, 'SET autocommit=0', [0])
            self.flags(c, 'SELECT 1', [0])
            self.flags(c, 'SET TRANSACTION READ ONLY', [0])
            self.error(c, 'INSERT INTO t VALUES(1)', 1792)
            self.error(c, 'INSERT INTO t VALUES(2)', 1792)
            self.flags(c, 'ROLLBACK', [0])
            self.query(c, 'INSERT INTO t VALUES(3)')
            self.query(c, 'ROLLBACK')
            self.check('failed/rolled back writes leave no rows', self.one(c, 'SELECT COUNT(*) FROM t'), ((0,),))

        def characteristics(c):
            self.query(c, 'SET autocommit=0')
            self.flags(c, 'SELECT * FROM t WHERE 0', [1])
            self.error(c, 'SET TRANSACTION READ ONLY', 1568)
            self.query(c, 'INSERT INTO t VALUES(1)')
            self.query(c, 'ROLLBACK')
            self.query(c, 'BEGIN')
            self.error(c, 'SET TRANSACTION ISOLATION LEVEL READ COMMITTED', 1568)
            self.query(c, 'ROLLBACK')
            self.flags(c, 'SAVEPOINT empty_txn', [0])
            self.query(c, 'SET TRANSACTION READ ONLY')
            self.error(c, 'INSERT INTO t VALUES(2)', 1792)
            self.query(c, 'ROLLBACK')
            self.flags(c, 'CREATE TEMPORARY TABLE actual_temp(i INT)', [1])
            self.error(c, 'SET TRANSACTION READ ONLY', 1568)

        def one_shot(c):
            self.query(c, 'SET TRANSACTION READ ONLY')
            self.query(c, 'SELECT 1; SET @x=2')
            self.error(c, 'INSERT INTO t VALUES(1)', 1792)
            self.query(c, 'SELECT * FROM t WHERE 0')
            self.query(c, 'INSERT INTO t VALUES(2)')
            self.check('table read consumes one-shot mode', self.one(c, 'SELECT i FROM t'), ((2,),))
            self.query(c, 'SET autocommit=0; SELECT 1; SET TRANSACTION READ ONLY; SELECT 2')
            self.query(c, 'SET TRANSACTION READ WRITE')
            self.query(c, 'INSERT INTO t VALUES(3); ROLLBACK')
            self.check('replaced one-shot mode stays transactional', self.one(c, 'SELECT i FROM t'), ((2,),))

        def batch(c):
            self.error(c, 'SET autocommit=0; SELECT 1; SET TRANSACTION READ ONLY; INSERT INTO t VALUES(1)', 1792)
            self.check('batch statuses before read-only error',
                       [r[1] & STATUS_MASK for r in self.last_results], [8, 8, 8])
            self.query(c, 'ROLLBACK; SET TRANSACTION READ WRITE')
            self.query(c, 'SELECT 1; INSERT INTO t VALUES(2); SELECT 3; INSERT INTO t VALUES(4); ROLLBACK')
            self.check('mixed no-table/table packet rolls back together', self.one(c, 'SELECT COUNT(*) FROM t'), ((0,),))

        def errors(c):
            self.query(c, 'SET autocommit=0; SELECT 1')
            self.error(c, 'SELECT * FROM no_such_review_fix_table', 1146)
            self.query(c, 'SET TRANSACTION READ ONLY')
            self.error(c, 'INSERT INTO t VALUES(1)', 1792)
            self.query(c, 'ROLLBACK')
            self.query(c, 'INSERT INTO t VALUES(2)')
            self.error(c, 'INSERT INTO t VALUES(2)', 1062)
            self.error(c, 'SELEC 1', 1064)
            self.error(c, 'SET TRANSACTION READ ONLY', 1568)
            self.query(c, 'INSERT INTO t VALUES(3); COMMIT')
            self.check('statement errors keep earlier writes', self.one(c, 'SELECT i FROM t ORDER BY i'), ((2,), (3,)))

        def execute(c):
            self.query(c, "PREPARE p FROM 'SELECT 1'")
            self.error(c, 'SET autocommit=0; EXECUTE p; SET TRANSACTION READ ONLY; INSERT INTO t VALUES(1)', 1792)
            self.query(c, 'ROLLBACK; DEALLOCATE PREPARE p')
            self.query(c, "PREPARE p FROM 'SELECT * FROM t'")
            self.flags(c, 'EXECUTE p', [1])
            self.error(c, 'SET TRANSACTION READ ONLY', 1568)
            self.query(c, 'ROLLBACK; DEALLOCATE PREPARE p; SET autocommit=1')
            self.query(c, "PREPARE p FROM 'INSERT INTO t VALUES(2)'")
            result = self.flags(c, 'SET @x=1; EXECUTE p; SELECT COUNT(*) FROM t', [0xa, 0xa, 2])
            self.check('EXECUTE in mixed packet writes once', result[-1][0], ((1,),))
            self.query(c, 'ROLLBACK')
            self.check('autocommit EXECUTE persisted', self.one(c, 'SELECT i FROM t'), ((2,),))

        def reset(c):
            name = self.schema + '_lock'
            self.query(c, 'CREATE TEMPORARY TABLE tmp(i INT)')
            self.query(c, "PREPARE p FROM 'SELECT 1'")
            self.query(c, 'SET autocommit=0; SELECT 1; SET TRANSACTION READ ONLY; SET @x=42')
            self.one(c, f"SELECT GET_LOCK('{name}', '0')")
            self.check('RESET status', self.reset(c) & STATUS_MASK, 2)
            self.check('RESET clears user variables', self.one(c, 'SELECT @x'), ((None,),))
            self.check('RESET releases locks', self.one(c, f"SELECT IS_FREE_LOCK('{name}')"), ((1,),))
            self.error(c, 'EXECUTE p', 1243)
            self.error(c, 'SELECT * FROM tmp', 1146)
            self.query(c, 'INSERT INTO t VALUES(1)')
            self.query(c, 'SET autocommit=0; INSERT INTO t VALUES(2)')
            self.reset(c)
            self.check('RESET rolls back active table transaction', self.one(c, 'SELECT i FROM t'), ((1,),))

        def locks(c):
            for value in ("'0'", "' 0 '", "'0.25'", "_binary '0'", "'0000000000000000000000000000'"):
                name = self.schema + '_lock'
                self.check('string timeout ' + value, self.one(c, f"SELECT GET_LOCK('{name}', {value})"), ((1,),))
                self.check('release after string timeout', self.one(c, f"SELECT RELEASE_LOCK('{name}')"), ((1,),))

        def serializable(c):
            self.query(c, 'SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE')
            self.query(c, 'BEGIN; SAVEPOINT a; SELECT * FROM t')
            self.query(c, 'INSERT INTO t VALUES(1)')
            self.error(c, 'INSERT INTO t VALUES(1)', 1062)
            self.query(c, 'ROLLBACK TO SAVEPOINT a; INSERT INTO t VALUES(2); COMMIT')
            self.check('SERIALIZABLE subtransaction cleanup', self.one(c, 'SELECT i FROM t'), ((2,),))

        for name, callback in [('flags', flags), ('lazy', lazy), ('characteristics', characteristics),
                               ('one-shot', one_shot), ('batch', batch), ('errors', errors),
                               ('EXECUTE', execute), ('RESET', reset), ('locks', locks),
                               ('SERIALIZABLE', serializable)]:
            self.case(name, callback)

    def pg(self, sql):
        psql = shutil.which(self.args.psql)
        if not psql:
            raise RuntimeError('global suite requires psql on PATH or --psql /path/to/psql')
        env = dict(os.environ, PGPASSWORD=self.args.password)
        result = subprocess.run([psql, '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1',
                                 '-h', self.args.host, '-p', str(self.args.pg_port),
                                 '-U', self.args.user, '-d', 'postgres'],
                                input=sql, text=True, capture_output=True, env=env, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    def global_cases(self, admin):
        """Explicit opt-in only. Use a dedicated integration instance."""
        schema = self.schema
        self.pg(f"""
CREATE SEQUENCE {schema}.seq;
CREATE FUNCTION {schema}.seq_body() RETURNS bigint LANGUAGE SQL VOLATILE
AS $$ SELECT pg_catalog.nextval('{schema}.seq'::regclass) $$;
CREATE FUNCTION {schema}.seq_stable() RETURNS bigint LANGUAGE SQL STABLE
AS $$ SELECT pg_catalog.nextval('{schema}.seq'::regclass) $$;
CREATE FUNCTION {schema}.seq_plpgsql() RETURNS bigint LANGUAGE plpgsql
AS $$ BEGIN RETURN pg_catalog.nextval('{schema}.seq'::regclass); END $$;
""")
        c = self.connect(schema)
        old = self.one(admin, 'SELECT @@global.read_only, @@global.super_read_only')[0]
        try:
            self.query(c, 'SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE')
            self.query(c, 'CREATE TEMPORARY TABLE tmp(i INT AUTO_INCREMENT PRIMARY KEY)')
            # Prepare before the flag changes: cached calls must still be checked.
            self.query(c, f"PREPARE p FROM 'SELECT pg_catalog.nextval(''{schema}.seq'')'")
            self.query(c, 'EXECUTE p')
            for name in ('seq_body', 'seq_stable', 'seq_plpgsql'):
                self.query(c, f'SELECT {schema}.{name}()')
            before = self.pg(f'SELECT last_value, is_called FROM {schema}.seq')
            self.query(admin, 'SET GLOBAL super_read_only=1')
            for sql in (f"SELECT mysql.setval('{schema}.seq', 900, true)",
                        f"SELECT nextval('{schema}.seq')",
                        f"SELECT pg_catalog.nextval(concat('{schema}', '.seq'))",
                        f"SELECT pg_catalog.setval('{schema}.seq', 901)",
                        f"SELECT pg_catalog.setval('{schema}.seq', 902, false)",
                        'EXECUTE p', f'SELECT {schema}.seq_body()',
                        f'SELECT {schema}.seq_stable()', f'SELECT {schema}.seq_plpgsql()'):
                self.error(c, sql, 1290)
                self.check('blocked sequence call leaves value unchanged',
                           self.pg(f'SELECT last_value, is_called FROM {schema}.seq'), before)
            self.error(c, 'INSERT INTO t VALUES(1)', 1290)
            self.error(c, 'CREATE TABLE blocked(i INT)', 1290)
            self.query(c, 'INSERT INTO tmp VALUES(NULL)')
            self.check('temporary AUTO_INCREMENT remains writable', self.one(c, 'SELECT i FROM tmp'), ((1,),))

            # A server policy must not change actual SERIALIZABLE access mode.
            self.query(c, 'BEGIN')
            self.query(c, 'USE ' + schema)
            self.query(c, 'SAVEPOINT a')
            self.error(c, 'INSERT INTO t VALUES(2)', 1290)
            self.query(c, 'ROLLBACK TO SAVEPOINT a')
            self.check('utility/error did not change transaction mode',
                       self.one(c, "SELECT current_setting('transaction_read_only')"), (('off',),))
            self.query(admin, 'SET GLOBAL super_read_only=0; SET GLOBAL read_only=0')
            self.query(c, 'INSERT INTO t VALUES(3); COMMIT')
            self.check('SERIALIZABLE remains writable after policy change', self.one(c, 'SELECT i FROM t'), ((3,),))

            self.query(admin, 'SET GLOBAL super_read_only=1')
            self.query(c, "SELECT set_config('mysql.parser', 'postgres', false)")
            self.query(c, 'CREATE TEMP SEQUENCE review_fix_temp_seq')
            self.check('temporary nextval remains writable',
                       self.one(c, "SELECT pg_catalog.nextval('review_fix_temp_seq')"), ((1,),))
            self.query(c, "SELECT mysql.setval('review_fix_temp_seq', 40, true)")
            self.check('temporary setval remains writable',
                       self.one(c, "SELECT pg_catalog.nextval('review_fix_temp_seq')"), ((41,),))
            self.query(c, f'VACUUM {schema}.t')  # internal transaction boundaries
            self.query(c, f'ANALYZE {schema}.t')
            self.error(c, f"DO $$ BEGIN PERFORM pg_catalog.nextval('{schema}.seq'); END $$", 1290)
            self.query(c, "SELECT set_config('mysql.parser', 'mysql', false)")
            self.reset(c)
            self.error(c, 'INSERT INTO t VALUES(4)', 1290)
            self.check('RESET retains global policy, clears transaction mode',
                       self.one(c, "SELECT current_setting('transaction_read_only')"), (('off',),))
        finally:
            c.close()
            # Setting read_only=0 also clears super_read_only. Restore in order.
            self.query(admin, 'SET GLOBAL super_read_only=0')
            self.query(admin, f'SET GLOBAL read_only={int(old[0])}')
            self.query(admin, f'SET GLOBAL super_read_only={int(old[1])}')

    def run(self):
        admin = self.connect()
        try:
            self.query(admin, 'CREATE DATABASE ' + self.schema)
            self.query(admin, f'CREATE TABLE {self.schema}.t(i INT PRIMARY KEY)')
            self.session_cases()
            if self.args.allow_global_read_only:
                self.query(admin, f'DELETE FROM {self.schema}.t')
                self.global_cases(admin)
        except Exception as exc:
            self.check('suite completed', repr(exc), 'success')
        finally:
            try:
                if self.args.allow_global_read_only:
                    self.pg(f'DROP SCHEMA IF EXISTS {self.schema} CASCADE')
                else:
                    self.query(admin, 'DROP DATABASE IF EXISTS ' + self.schema)
            finally:
                admin.close()
        print(f'{self.port}: {self.checks} checks, {self.failures} failures', flush=True)
        return self.failures


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--ports', default='3790')
    p.add_argument('--user', default=os.environ.get('MYSQL_USER', 'dbuser_mysql'))
    p.add_argument('--password', default=os.environ.get('MYSQL_PASSWORD', 'Mysql.123'))
    p.add_argument('--allow-global-read-only', action='store_true')
    p.add_argument('--pg-port', type=int)
    p.add_argument('--psql', default=os.environ.get('PSQL', 'psql'))
    args = p.parse_args()
    ports = [int(x) for x in args.ports.split(',')]
    if args.allow_global_read_only and (len(ports) != 1 or args.pg_port is None):
        p.error('global tests require exactly one isolated MySQL port and --pg-port')
    return int(sum(Suite(args, port).run() for port in ports) != 0)


if __name__ == '__main__':
    sys.exit(main())
