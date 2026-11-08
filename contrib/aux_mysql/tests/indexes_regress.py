#!/usr/bin/env python3
"""Exact prefix index regression; stdlib + libpq, no Python packages needed.

Run via indexes_standalone.py, or set PREFIX_TEST_DSN and LIBPQ against a
private integrated instance. Creates and finally drops review_fix_* only.
"""
import concurrent.futures
import ctypes as C
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

lib = C.CDLL(os.environ.get('LIBPQ') or ctypes.util.find_library('pq'))
for name, result, args in (
    ('PQconnectdb', C.c_void_p, [C.c_char_p]),
    ('PQstatus', C.c_int, [C.c_void_p]),
    ('PQfinish', None, [C.c_void_p]),
    ('PQexec', C.c_void_p, [C.c_void_p, C.c_char_p]),
    ('PQresultStatus', C.c_int, [C.c_void_p]),
    ('PQresultErrorMessage', C.c_char_p, [C.c_void_p]),
    ('PQresultErrorField', C.c_char_p, [C.c_void_p, C.c_int]),
    ('PQerrorMessage', C.c_char_p, [C.c_void_p]),
    ('PQntuples', C.c_int, [C.c_void_p]),
    ('PQnfields', C.c_int, [C.c_void_p]),
    ('PQgetvalue', C.c_char_p, [C.c_void_p, C.c_int, C.c_int]),
    ('PQgetisnull', C.c_int, [C.c_void_p, C.c_int, C.c_int]),
    ('PQclear', None, [C.c_void_p]),
):
    f = getattr(lib, name)
    f.restype, f.argtypes = result, args


class DBError(Exception):
    def __init__(self, code, message):
        self.code = code
        super().__init__(f'{code}: {message}')


class DB:
    def __init__(self, dsn=None):
        self.conn = lib.PQconnectdb((dsn or os.environ.get('PREFIX_TEST_DSN', '')).encode())
        if lib.PQstatus(self.conn):
            raise RuntimeError(lib.PQerrorMessage(self.conn).decode())

    def q(self, sql):
        r = lib.PQexec(self.conn, sql.encode())
        try:
            if lib.PQresultStatus(r) not in (1, 2):
                code = lib.PQresultErrorField(r, ord('C')) or b'unknown'
                raise DBError(code.decode(), lib.PQresultErrorMessage(r).decode())
            return [tuple(None if lib.PQgetisnull(r, i, j) else lib.PQgetvalue(r, i, j).decode()
                          for j in range(lib.PQnfields(r))) for i in range(lib.PQntuples(r))]
        finally:
            lib.PQclear(r)

    def close(self):
        lib.PQfinish(self.conn)


def lit(s):
    return "'" + s.replace("'", "''") + "'"


def blob(b):
    return f"decode('{b.hex()}','hex')"


def fails(db, sql, code='23505'):
    try:
        db.q(sql)
    except DBError as e:
        assert e.code == code, (code, e)
    else:
        raise AssertionError('unexpected success: ' + sql[:180])


checks = []
def passed(name):
    checks.append(name)
    print('PASS', name, flush=True)


a = bytes.fromhex('''d131dd02c5e6eec4693d9a0698aff95c2fcab58712467eab4004583eb8fb7f89
55ad340609f4b30283e488832571415a085125e8f7cdc99fd91dbdf280373c5b
d8823e3156348f5bae6dacd436c919c6dd53e2b487da03fd02396306d248cda0
e99f33420f577ee8ce54b67080a80d1ec69821bcb6a8839396f9652b6ff72a70''')
b = bytes.fromhex('''d131dd02c5e6eec4693d9a0698aff95c2fcab50712467eab4004583eb8fb7f89
55ad340609f4b30283e4888325f1415a085125e8f7cdc99fd91dbd7280373c5b
d8823e3156348f5bae6dacd436c919c6dd53e23487da03fd02396306d248cda0
e99f33420f577ee8ce54b67080280d1ec69821bcb6a8839396f965ab6ff72a70''')
assert a != b and len(a) == len(b) == 128
assert hashlib.md5(a).hexdigest() == hashlib.md5(b).hexdigest() == '79054025255fb1a26e4bc422aef54eb4'

db = DB()
schema = f'review_fix_indexes_{os.getpid():x}'
try:
    db.q('CREATE SCHEMA ' + schema)
    db.q(f'SET search_path={schema},pg_catalog')
    db.q('SET statement_timeout=15000')
    db.q('CREATE TABLE short(a varchar(20) COLLATE mysql.utf8mb4_general_ci, b int)')
    db.q("CREATE UNIQUE INDEX short_u ON short USING mysql_prefix(a,b) WITH(prefix_lengths='3,0')")
    db.q("INSERT INTO short VALUES ('abc1',1),('abd1',1),('abc1',2),(NULL,1),(NULL,1)")
    fails(db, "INSERT INTO short VALUES ('ABC2',1)")
    fails(db, "UPDATE short SET a='abc3' WHERE a='abd1'")
    assert db.q("SELECT * FROM mysql.index_key_parts('short_u'::regclass)") == [('1', 'a', '3'), ('2', 'b', None)]
    passed('short prefixes, composite keys, updates, NULLs and metadata')

    ddl = "CREATE TABLE pk(a varchar(20) COLLATE mysql.utf8mb4_general_ci, b int, PRIMARY KEY(a,b) WITH(prefix_lengths='3,0'))"
    db.q('SELECT mysql.prefix_test_ddl(' + lit(ddl) + ')')
    db.q("INSERT INTO pk VALUES ('abc1',1),('abc2',2)")
    fails(db, "INSERT INTO pk VALUES ('abc9',1)")
    fails(db, "INSERT INTO pk VALUES (NULL,1)", '23502')
    assert db.q("SELECT indisprimary FROM pg_index WHERE indrelid='pk'::regclass") == [('t',)]
    assert db.q("SELECT attnotnull FROM pg_attribute WHERE attrelid='pk'::regclass AND attnum>0") == [('t',), ('t',)]
    passed('real PRIMARY constraint and NOT NULL with a declared prefix')
    db.q('CREATE TABLE pk_copy(LIKE pk INCLUDING ALL)')
    db.q("INSERT INTO pk_copy VALUES ('abc1',1)")
    fails(db, "INSERT INTO pk_copy VALUES ('abc9',1)")
    assert db.q("SELECT c.reloptions @> ARRAY['prefix_lengths=3,0'] FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid WHERE i.indrelid='pk_copy'::regclass") == [('t',)]
    db.q('CREATE TABLE explicit_options(a text COLLATE mysql.utf8mb4_general_ci)')
    db.q('CREATE UNIQUE INDEX explicit_u ON explicit_options USING mysql_prefix(a mysql.prefix_25(mysql_prefix=2))')
    db.q('CREATE TABLE explicit_copy(LIKE explicit_options INCLUDING ALL)')
    db.q("INSERT INTO explicit_copy VALUES ('abc')")
    fails(db, "INSERT INTO explicit_copy VALUES ('abd')")
    passed('CREATE TABLE LIKE preserves primary, reloptions and opclass options')

    db.q('CREATE TABLE collision(k bytea, value int)')
    db.q("CREATE UNIQUE INDEX collision_u ON collision USING mysql_prefix(k) WITH(prefix_lengths='3000')")
    db.q(f'INSERT INTO collision VALUES ({blob(a)},1),({blob(b)},2)')
    db.q(f'INSERT INTO collision VALUES ({blob(b)},7) ON CONFLICT(k) DO UPDATE SET value=excluded.value')
    assert db.q('SELECT value FROM collision ORDER BY value') == [('1',), ('7',)]
    fails(db, f'INSERT INTO collision VALUES ({blob(a)},9)')
    passed('real Wang MD5 collision retains both rows and updates the right row')

    x = b''.join(hashlib.sha256(str(i).encode()).digest() for i in range(100))[:3000]
    y = x[:2800] + b'Y' + x[2801:]
    db.q(f'INSERT INTO collision VALUES ({blob(x)},3),({blob(y)},4)')
    fails(db, f'INSERT INTO collision VALUES ({blob(x + b"tail")},9)')
    db.q(f'INSERT INTO collision VALUES ({blob(y + b"tail")},8) ON CONFLICT(k) DO UPDATE SET value=excluded.value')
    assert db.q(f'SELECT value FROM collision WHERE k={blob(x)}') == [('3',)]
    assert db.q(f'SELECT value FROM collision WHERE k={blob(y)}') == [('8',)]
    passed('incompressible 3000-byte keys and equal candidate/different original prefixes')

    db.q('CREATE TABLE twoblob(a bytea,b bytea)')
    db.q("CREATE UNIQUE INDEX twoblob_u ON twoblob USING mysql_prefix(a,b) WITH(prefix_lengths='1500,1500')")
    db.q(f'INSERT INTO twoblob VALUES ({blob(x)},{blob(y)})')
    fails(db, f'INSERT INTO twoblob VALUES ({blob(x[:1500])},{blob(y[:1500])})')
    passed('two 1500-byte key parts retain exact composite uniqueness')

    db.q('CREATE TABLE ci(a text COLLATE mysql.utf8mb4_general_ci)')
    db.q('CREATE UNIQUE INDEX ci_u ON ci USING mysql_prefix(a)')
    db.q("INSERT INTO ci VALUES ('e')")
    for value in ('é', 'E', 'e ', 'É   '):
        fails(db, 'INSERT INTO ci VALUES (' + lit(value) + ')')
    assert db.q("SELECT count(*) FROM ci WHERE a='é '") == [('1',)]
    assert db.q("SELECT 'e '::text COLLATE mysql.utf8mb4_general_ci LIKE 'e', 'é'::text COLLATE mysql.utf8mb4_general_ci LIKE 'E'") == [('f', 't')]
    assert db.q("SELECT count(DISTINCT a) FROM (VALUES ('e'::text COLLATE mysql.utf8mb4_general_ci),('É '),('E')) v(a)") == [('1',)]
    assert db.q("SELECT 'e'::text COLLATE \"C\" = 'é', 'e'::text COLLATE \"C\" = 'e '") == [('f', 'f')]
    passed('general_ci accents, case, PAD SPACE, LIKE, hash/grouping; ordinary PG unchanged')
    db.q('CREATE TABLE bin_ci(a text COLLATE mysql.utf8mb4_bin)')
    db.q('CREATE UNIQUE INDEX bin_ci_u ON bin_ci USING mysql_prefix(a)')
    db.q("INSERT INTO bin_ci VALUES ('e'),('é'),('E')")
    fails(db, "INSERT INTO bin_ci VALUES ('e ')")
    assert db.q("SELECT 'e '::text COLLATE mysql.utf8mb4_bin LIKE 'e'") == [('f',)]
    passed('utf8mb4_bin retains accents/case while enforcing PAD SPACE')

    if db.q("SELECT to_regtype('mysql.varbinary_n') IS NOT NULL") == [('t',)]:
        for typ in ('binary_n','varbinary_n'):
            db.q(f'CREATE TABLE {typ}(a mysql.{typ})')
            db.q(f"CREATE UNIQUE INDEX {typ}_u ON {typ} USING mysql_prefix(a) WITH(prefix_lengths='3')")
            db.q(f'INSERT INTO {typ} VALUES ({blob(bytes([0,255,1,2]))}::mysql.{typ})')
            fails(db,f'INSERT INTO {typ} VALUES ({blob(bytes([0,255,1,3]))}::mysql.{typ})')
        # An explicit custom-type opclass with options must also survive a
        # restore into another database, where the type OID is different.
        opclass = db.q("SELECT opc.opcname FROM pg_opclass opc JOIN pg_am am ON am.oid=opc.opcmethod "
                       "WHERE am.amname='mysql_prefix' AND opc.opcintype='mysql.varbinary_n'::regtype")[0][0]
        db.q('CREATE TABLE custom_options(a mysql.varbinary_n)')
        db.q('CREATE UNIQUE INDEX custom_options_u ON custom_options USING mysql_prefix '
             '(a mysql."' + opclass.replace('"', '""') + '"(mysql_prefix=2))')
        db.q(f"INSERT INTO custom_options VALUES ({blob(b'abc')}::mysql.varbinary_n)")
        db.q('SELECT mysql.prefix_test_ddl(' + lit('CREATE TABLE pk_custom('
             'a mysql.varbinary_n, PRIMARY KEY(a) '
             "WITH(prefix_lengths='2'))") + ')')
        nul_binary = blob(bytes([97,0,98]))
        db.q(f"INSERT INTO pk_custom VALUES ({nul_binary}::mysql.varbinary_n)")
        fails(db, f"INSERT INTO pk_custom VALUES ({blob(bytes([97,0,122]))}::mysql.varbinary_n)")


    db.q('CREATE TABLE multibyte(a text COLLATE mysql.utf8mb4_general_ci)')
    db.q("CREATE UNIQUE INDEX multibyte_u ON multibyte USING mysql_prefix(a) WITH(prefix_lengths='1')")
    db.q("INSERT INTO multibyte VALUES ('éx')")
    fails(db, "INSERT INTO multibyte VALUES ('eY')")
    db.q("INSERT INTO multibyte VALUES ('😀x')")
    fails(db, "INSERT INTO multibyte VALUES ('😇y')")
    passed('character prefixes and MySQL supplementary-character weights')

    # Fault injection only: character writers reject NUL, but an internal
    # value must not truncate the index key and collide with an unrelated row.
    for coll in ('mysql.utf8mb4_bin','"C"','mysql.utf8mb4_general_ci'):
        db.q('CREATE TABLE nul_defense(s text COLLATE '+coll+',v int)')
        db.q("CREATE UNIQUE INDEX nul_defense_u ON nul_defense USING mysql_prefix(s) WITH(prefix_lengths='3')")
        for h,n in [('610041',1),('610042',2),('c3a90041',3),('c3a90042',4)]:
            db.q("INSERT INTO nul_defense VALUES(mysql.prefix_test_text(decode('"+h+"','hex')),"+str(n)+")")
        assert db.q('SELECT count(*),sum(v) FROM nul_defense') == [('4','10')]
        fails(db,"INSERT INTO nul_defense VALUES(mysql.prefix_test_text(decode('61004158','hex')),5)",'23505')
        db.q('REINDEX INDEX nul_defense_u')
        assert db.q('SELECT sum(v) FROM nul_defense') == [('10',)]
        db.q('DROP TABLE nul_defense')
    passed('NUL counts as a character in bin/C/general_ci prefix keys, including multibyte prefixes')

    db.q('CREATE TABLE plans(a text COLLATE mysql.utf8mb4_general_ci,b int)')
    db.q("CREATE INDEX plans_ab ON plans USING mysql_prefix(a,b) WITH(prefix_lengths='10,0')")
    db.q("INSERT INTO plans SELECT 'x'||md5(g::text)||repeat('long',200),g%5 FROM generate_series(1,3000)g")
    db.q("INSERT INTO plans VALUES ('needle hello',2),('NEEDLE ',2),('needle ',3),('é ',2),('é',2),('e_y',2),('e%z',2)")
    db.q('ANALYZE plans')
    plan = '\n'.join(r[0] for r in db.q("EXPLAIN SELECT * FROM plans WHERE a LIKE 'needle%' AND b=2"))
    assert 'Index' in plan and 'b = 2' in plan and '~#~' in plan, plan
    print(plan, flush=True)
    for pattern in ('needle%', 'e%', 'e %', 'e_', 'e\\_%', 'e\\%%', '%é%', 'é', 'é ', ''):
        sql = 'SELECT a,b FROM plans WHERE a LIKE ' + lit(pattern) + ' AND b=2 ORDER BY a COLLATE "C",b'
        db.q('SET enable_indexscan=off;SET enable_bitmapscan=off;SET enable_seqscan=on')
        seq = db.q(sql)
        db.q('SET enable_indexscan=on;SET enable_bitmapscan=on;SET enable_seqscan=off')
        assert db.q(sql) == seq, (pattern, seq, db.q(sql))
        db.q('SET enable_indexscan=off;SET enable_bitmapscan=on')
        assert db.q(sql) == seq, ('bitmap', pattern)
    db.q('RESET enable_indexscan;RESET enable_bitmapscan;RESET enable_seqscan')
    passed('LIKE range plan uses both parts; sequential/index/bitmap results agree')

    for table in ('short', 'collision', 'twoblob', 'ci', 'plans'):
        db.q('VACUUM ' + table)
        db.q('REINDEX TABLE ' + table)
    fails(db, "INSERT INTO short VALUES ('ABC9',1)")
    assert db.q('SELECT count(*) FROM collision') == [('4',)]
    passed('VACUUM and REINDEX preserve original-value uniqueness')

    db.q('CREATE TABLE bulk(a text COLLATE mysql.utf8mb4_general_ci)')
    db.q('CREATE UNIQUE INDEX bulk_u ON bulk USING mysql_prefix(a)')
    db.q("BEGIN;INSERT INTO bulk SELECT g::text FROM generate_series(1,12000)g")
    assert db.q("SELECT count(*) FROM pg_locks WHERE pid=pg_backend_pid() AND locktype='advisory'") == [('0',)]
    db.q('COMMIT')
    passed('12000 unique rows in one transaction do not exhaust or retain candidate locks')

    db.q('CREATE TABLE concurrent(a text COLLATE mysql.utf8mb4_general_ci,b int)')
    db.q("CREATE UNIQUE INDEX concurrent_u ON concurrent USING mysql_prefix(a,b) WITH(prefix_lengths='3,0')")
    other = DB()
    other.q(f'SET search_path={schema},pg_catalog;SET statement_timeout=10000')
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            db.q("BEGIN;INSERT INTO concurrent VALUES ('held',1)")
            for value, part in (('free',1),('held',2)):
                fut = pool.submit(other.q, f'INSERT INTO concurrent VALUES ({lit(value)},{part})')
                fut.result(timeout=2)  # first transaction is still open
            db.q('COMMIT')
            passed('disjoint keys, including different composite suffix, finish before holder commits')
            db.q("BEGIN;INSERT INTO concurrent VALUES ('dupa',1)")
            fut = pool.submit(other.q, "INSERT INTO concurrent VALUES ('DUPb',1)")
            time.sleep(0.2)
            assert not fut.done(), 'equal keys did not wait for the owning transaction'
            db.q('COMMIT')
            try:
                fut.result(timeout=3)
                raise AssertionError('concurrent duplicate accepted')
            except DBError as e:
                assert e.code == '23505', e
            db.q("BEGIN;INSERT INTO concurrent VALUES ('rollback',1)")
            fut = pool.submit(other.q, "INSERT INTO concurrent VALUES ('ROLLother',1)")
            time.sleep(0.2)
            assert not fut.done()
            db.q('ROLLBACK')
            fut.result(timeout=3)
            passed('equal concurrent keys wait, commit conflicts, rollback permits insertion')
    finally:
        db.q('ROLLBACK')
        other.close()
    metadata_sql = """SELECT t.relname,c.relname,i.indisprimary,i.indisunique,am.amname,
        c.reloptions::text,pg_get_indexdef(i.indexrelid)
        FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid
        JOIN pg_class t ON t.oid=i.indrelid JOIN pg_namespace ns ON ns.oid=t.relnamespace
        JOIN pg_am am ON am.oid=c.relam WHERE ns.nspname=""" + lit(schema) + ' ORDER BY t.relname,c.relname'
    before_dump = db.q(metadata_sql)
    with tempfile.TemporaryDirectory(prefix='review_fix_dump_', dir=os.environ.get('PREFIX_TEST_ARTIFACT_DIR')) as td:
        dump = Path(td) / 'native.sql'
        dsn = os.environ.get('PREFIX_TEST_DSN', '')
        subprocess.run([os.environ.get('PG_DUMP', 'pg_dump'), '--dbname=' + dsn,
                        '--no-owner', '--no-privileges', '--schema=' + schema, '--file=' + str(dump)], check=True)
        assert 'PRIMARY KEY' in dump.read_text() and 'prefix_lengths' in dump.read_text()
        db.q('DROP SCHEMA ' + schema + ' CASCADE')
        subprocess.run([os.environ.get('PSQL', 'psql'), '--dbname=' + dsn, '-X', '-v',
                        'ON_ERROR_STOP=1', '-f', str(dump)], check=True, stdout=subprocess.DEVNULL)
        if os.environ.get('PREFIX_TEST_RESTORE_DSN'):
            restore_dsn = os.environ['PREFIX_TEST_RESTORE_DSN']
            restored = DB(restore_dsn)
            try:
                assert restored.q("SELECT 'mysql.varbinary_n'::regtype::oid") != db.q("SELECT 'mysql.varbinary_n'::regtype::oid")
                subprocess.run([os.environ.get('PSQL', 'psql'), '--dbname=' + restore_dsn, '-X', '-v',
                                'ON_ERROR_STOP=1', '-f', str(dump)], check=True, stdout=subprocess.DEVNULL)
                restored.q(f'SET search_path={schema},pg_catalog')
                assert restored.q(metadata_sql) == before_dump
                fails(restored, "INSERT INTO pk VALUES ('ABC9',1)")
                fails(restored, "INSERT INTO pk_copy VALUES ('ABC9',1)")
                fails(restored, "INSERT INTO pk VALUES (NULL,1)", '23502')
                fails(restored, f"INSERT INTO custom_options VALUES ({blob(b'abz')}::mysql.varbinary_n)")
                fails(restored, f"INSERT INTO pk_custom VALUES ({blob(bytes([97,0,122]))}::mysql.varbinary_n)")
                fails(restored, "INSERT INTO pk_custom VALUES (NULL)", '23502')
                assert restored.q('SELECT count(*) FROM collision') == [('4',)]
                passed('native dump to a different database retains PRIMARY and custom-type opclass options despite changed type OIDs')
            finally:
                restored.q('DROP SCHEMA IF EXISTS ' + schema + ' CASCADE')
                restored.close()
    assert db.q(metadata_sql) == before_dump, 'native dump/restore changed AM, options or primary metadata'
    fails(db, "INSERT INTO pk VALUES ('ABC9',1)")
    fails(db, "INSERT INTO pk_copy VALUES ('ABC9',1)")
    fails(db, "INSERT INTO pk VALUES (NULL,1)", '23502')
    assert db.q('SELECT count(*) FROM collision') == [('4',)]
    passed('native pg_dump/psql round-trip preserves real PRIMARY, AM, opclasses, prefixes, data and NOT NULL')
    print(json.dumps({'passed': checks, 'total': len(checks)}, ensure_ascii=False), flush=True)
finally:
    db.q('ROLLBACK')
    db.q('DROP SCHEMA IF EXISTS ' + schema + ' CASCADE')
    db.close()
