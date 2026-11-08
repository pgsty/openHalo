#!/usr/bin/env python3
"""Build and test the prefix AM in a disposable PG18 cluster.

PG_CONFIG selects PostgreSQL; BUILD_DIR retains all artifacts under that
directory. Otherwise mktemp is used and removed on exit. Never installs a
library or accesses an existing server. Requires cc, Python 3 and PG18+ICU.
"""
import getpass
import os
from pathlib import Path
import platform
import shlex
import shutil
import socket
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pg_config = os.environ.get('PG_CONFIG', 'pg_config')
def config(key):
    return subprocess.check_output([pg_config, '--' + key], text=True).strip()

keep = 'BUILD_DIR' in os.environ
build = Path(os.environ['BUILD_DIR']).resolve() if keep else Path(tempfile.mkdtemp(prefix='review_fix_indexes_'))
build.mkdir(parents=True, exist_ok=True)
bindir = Path(config('bindir'))
module = build / 'mys_prefix.so'
pgdata = build / 'pgdata'
assert not pgdata.exists(), 'BUILD_DIR must not contain an existing pgdata directory'
with socket.socket() as s:
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

started = False
observer = None
try:
    compiler = shlex.split(os.environ.get('CC', config('cc')))
    flags = ['-bundle', '-undefined', 'dynamic_lookup'] if platform.system() == 'Darwin' else ['-shared', '-fPIC']
    run(compiler + flags + ['-g', '-O1', '-DMYS_PREFIX_STANDALONE', '-Wall', '-Wextra',
        '-Wno-unused-parameter', '-Wno-missing-field-initializers'] + shlex.split(config('cppflags')) +
        ['-I' + config('includedir-server'), '-I' + config('includedir'), '-I' + str(root / 'src'),
         root / 'src/mys_prefix.c', root / 'tests/indexes_standalone.c', '-o', module])
    with (build / 'initdb.log').open('w') as log:
        run([bindir / 'initdb', '-D', pgdata, '-A', 'trust', '--no-locale', '--encoding=UTF8'], stdout=log)
    with (pgdata / 'postgresql.conf').open('a') as f:
        f.write(f"\nport={port}\nlisten_addresses='127.0.0.1'\nunix_socket_directories=''\n"
                f"shared_preload_libraries='{str(module).replace(chr(39), chr(39)*2)}'\n")
    run([bindir / 'pg_ctl', '-D', pgdata, '-l', build / 'server.log', '-w', 'start'])
    started = True
    psql = [bindir / 'psql', '-X', '-h', '127.0.0.1', '-p', str(port), '-U', getpass.getuser(),
            '-d', 'postgres', '-v', 'ON_ERROR_STOP=1']
    libname = str(module).replace("'", "''")
    run(psql + ['-c', "CREATE SCHEMA mysql; CREATE FUNCTION mysql.prefix_test_available(oid) RETURNS text "
                     f"AS '{libname}', 'mys_prefix_test_available' LANGUAGE C STRICT"], stdout=subprocess.DEVNULL)
    observer = subprocess.Popen([str(a) for a in psql + ['-Atq']], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)
    def observed(typ='text'):
        observer.stdin.write(f"SELECT mysql.prefix_test_available('{typ}'::regtype);\n")
        observer.stdin.flush()
        result = observer.stdout.readline().strip()
        assert result, 'availability observer exited unexpectedly'
        return result
    assert observed() == 'case_insensitive,case_sensitive,no,no'
    sql = (root / 'tests/indexes_types_fixture.sql').read_text()
    sql += (root / 'sql/aux_mysql_fix_indexes.sql').read_text()
    sql += "\nCREATE FUNCTION mysql.prefix_test_ddl(text) RETURNS void AS '$libdir/aux_mysql', 'mys_prefix_test_ddl' LANGUAGE C STRICT;\n"
    sql = sql.replace('$libdir/aux_mysql', str(module).replace("'", "''"))
    install = build / 'install.sql'
    install.write_text(sql)
    run(psql + ['-f', install], stdout=subprocess.DEVNULL)
    assert observed() == 'utf8mb4_general_ci,utf8mb4_bin,yes,yes'
    fresh = subprocess.check_output([str(a) for a in psql + ['-Atq', '-c',
        "SELECT mysql.prefix_test_available('text'::regtype)"]], text=True).strip()
    assert fresh == observed()
    run(psql + ['-c', 'ALTER COLLATION mysql.utf8mb4_general_ci RENAME TO review_fix_hidden_ci'], stdout=subprocess.DEVNULL)
    assert observed() == 'case_insensitive,utf8mb4_bin,yes,yes'
    run(psql + ['-c', 'ALTER COLLATION mysql.review_fix_hidden_ci RENAME TO utf8mb4_general_ci'], stdout=subprocess.DEVNULL)
    assert observed() == 'utf8mb4_general_ci,utf8mb4_bin,yes,yes'
    run(psql + ['-c', 'DROP OPERATOR FAMILY mysql.prefix_2950 USING mysql_prefix CASCADE'], stdout=subprocess.DEVNULL)
    assert observed('uuid') == 'utf8mb4_general_ci,utf8mb4_bin,yes,no'
    run(psql + ['-c', "SELECT mysql.register_prefix_type('uuid',false)"], stdout=subprocess.DEVNULL)
    assert observed('uuid') == 'utf8mb4_general_ci,utf8mb4_bin,yes,yes'
    print('PASS pre-upgrade fallback; existing and new connections observe SQL installation and catalog invalidation', flush=True)
    # A second database deliberately gives extension types different OIDs.
    run(psql + ['-c', 'CREATE DATABASE review_fix_restore TEMPLATE template0'], stdout=subprocess.DEVNULL)
    run(psql + ['-d', 'review_fix_restore', '-c', 'CREATE SCHEMA mysql'], stdout=subprocess.DEVNULL)
    run(psql + ['-d', 'review_fix_restore', '-f', install], stdout=subprocess.DEVNULL)
    env = os.environ.copy()
    env['PREFIX_TEST_DSN'] = f'host=127.0.0.1 port={port} user={getpass.getuser()} dbname=postgres'
    env['LIBPQ'] = str(Path(config('libdir')) / ('libpq.dylib' if platform.system() == 'Darwin' else 'libpq.so'))
    env['PG_DUMP'] = str(bindir / 'pg_dump')
    env['PSQL'] = str(bindir / 'psql')
    env['PREFIX_TEST_ARTIFACT_DIR'] = str(build)
    env['PREFIX_TEST_RESTORE_DSN'] = env['PREFIX_TEST_DSN'].replace('dbname=postgres', 'dbname=review_fix_restore')
    run([os.environ.get('PYTHON', 'python3'), root / 'tests/indexes_regress.py'], env=env)
    run(psql + ['-c', 'DROP ACCESS METHOD mysql_prefix CASCADE'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert observed() == 'utf8mb4_general_ci,utf8mb4_bin,no,no'
    print('PASS existing connection detects removed AM and opclasses', flush=True)
finally:
    if observer:
        observer.stdin.close()
        observer.wait(timeout=10)
    if started:
        run([bindir / 'pg_ctl', '-D', pgdata, '-m', 'fast', '-w', 'stop'])
    if keep:
        print('Artifacts retained in', build)
    else:
        shutil.rmtree(build)
