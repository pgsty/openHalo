# aux_mysql 1.9 integration checks

Use disposable instances with the matching kernel and extension. Build debug
(`--enable-debug --enable-cassert`) and release binaries separately; enable
`--enable-depend`, or clean all objects after changing node/protocol headers.
Keep build directories, data directories and logs outside this repository.

`tests/run.sh` recreates its `test` schema. The transaction and AB suites also
exercise global read-only flags: never point these suites at a shared production
instance. Configure SCRAM authentication and TLS before running driver/auth tests;
a trust HBA rule cannot verify password rejection. The existing smoke clients use
the documented fixture role `dbuser_mysql` and its test password.

Set connections for one isolated instance:

```sh
export PGBIN=/path/to/install/bin
export PGHOST=127.0.0.1 PGPORT=5792 PGDATABASE=postgres PGUSER=postgres
export PGPASSWORD='fixture-password'
export MYSQL_HOST=127.0.0.1 MYSQL_PORT=3792 MYSQL_USER=dbuser_mysql
export MYSQL_PASSWORD='fixture-password' MYSQL_PWD="$MYSQL_PASSWORD"
export LOG_DIR=/tmp/aux-mysql-acceptance-debug
cd contrib/aux_mysql
bash tests/run.sh
```

The smoke suite checks 26 groups, including 46 AB cases, prepared packets,
comments, authentication, Go and Connector/J. Missing dependencies are incomplete
verification, not a pass. `JAR` selects the Connector/J jar.

The following suites create unique application schemas and remove them on exit:

```sh
python3 tests/release_dml.py
python3 tests/binary_fix/mysql_check.py --port "$MYSQL_PORT"
python3 tests/review_fix_sql_generated.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_generated_export.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_generated_policy.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/review_fix_pending_fk.py --ports "$MYSQL_PORT" --password "$MYSQL_PASSWORD"
python3 tests/route_a_fk_privileges.py
python3 tests/review_fix_collation_mapping.py --mysql-port "$MYSQL_PORT" --pg-port "$PGPORT"
python3 tests/indexes_mysql.py
python3 tests/indexes_btree.py
python3 tests/release_txn.py --ports "$MYSQL_PORT" --pg-port "$PGPORT" \
  --psql "$PGBIN/psql" --allow-global-read-only
```

They cover all-key ODKU/REPLACE, FK/cascade/IGNORE behavior, repeated keys and
concurrent writers, trigger ordering, limited-column privileges, byte and NUL
preservation, generated values/ALTER/LIKE, missing-parent FK declarations,
MD5-collision keys, short-key ordered plans, prefix concurrency, queued EXECUTE,
RESET and runtime read-only enforcement. The collation test covers exact and
approximated names, warning lifetimes, expression `COLLATE` in DML/CTAS/views,
PREPARE/EXECUTE, multi-statement boundaries and COM_RESET_CONNECTION. The
index-plan and DML tests also need
a PostgreSQL connection; their PG role must be able to create fixture roles and
triggers. The transaction suite changes global flags only with its explicit flag
and restores them in cleanup.

The Route A FK privilege suite creates two non-superuser roles and verifies
schema USAGE, table/column REFERENCES, implicit composite primary keys,
revocation, parent recreation and privileged pg_dump/restore. Denied binding
must retain the declaration and old rows, warn 1142, and let parent DDL finish.
This enforces PG ownership; MySQL 5.7 does not check equivalent REFERENCES ACLs.

The generated-expression policy suite checks rejection of nondeterministic
expressions and preserves legitimate compatibility functions and sql_mode
conversions, including constant generators after repeated prepared executions.
It also creates legacy wrappers through a PG connection (default: MySQL port
plus 2000, or explicit `--pg-ports`) to check ALTER/LIKE and repair paths.
When changing an approved compatibility implementation, review the validator's
implementation identities/body fingerprints and rerun these checks.
VIRTUAL storage, virtual primary keys and references to
AUTO_INCREMENT follow the documented OpenHalo choices; do not make MySQL's
rejection of the latter two a reason to remove working PG capabilities.

For reference comparisons, `review_fix_sql_generated.py`,
`review_fix_generated_export.py` and `review_fix_pending_fk.py` accept multiple `--ports`. `indexes_mysql.py` works
against MySQL 5.7 directly. `release_dml.py` accepts `MYSQL_REFERENCE_PORT` to
repeat SQL/concurrency cases; `release_txn.py --ports 3771` runs its session-only
reference cases without changing global flags.

Upgrade and recovery checks:

```sh
python3 tests/release_upgrade.py
export MYSQLDUMP=/path/to/mysql57/bin/mysqldump
export MYSQL_CLIENT=/path/to/mysql57/bin/mysql
# Optional: also restore the same unmodified dump into the reference server.
export MYSQL_REFERENCE_PORT=3771
python3 tests/release_roundtrip.py
# Run only with no concurrent clients: briefly redirects the MySQL listener.
python3 tests/release_online_upgrade.py --allow-listener-reload
```

`release_upgrade.py` requires the original 1.6 installation SQL in the isolated
installation's extension directory. It creates two temporary PG databases,
compares 1.6→1.7→1.8→1.9 with a fresh 1.9, and checks old index names, binary
bytes, stored ON CONFLICT trees and safe FK enforcement. The 1.6 fixture must be
built from the original release SQL, not today's generator output.

`release_roundtrip.py` uses matching `pg_dump`/`pg_restore` and Oracle MySQL
5.7+ clients. MariaDB 10.5 mysqldump includes generated values even when reading
a real MySQL 5.7 server, producing INSERTs MySQL itself rejects; it is not a valid
generated-column round-trip oracle. The test checks all tables and a view,
NUL/binary data, binary defaults, generated values and prefix PRIMARY semantics.
It also preserves historical orphan rows and missing-parent FK declarations,
then checks that new invalid writes fail and cascades work after restore.

`release_online_upgrade.py` temporarily changes `mysql.database` to its own
database and restores the prior configuration on exit. It verifies new binaries
with 1.8 SQL and keeps the same MySQL connection open across ALTER EXTENSION:
binary, generated-column and prefix-index availability must refresh without a
reconnect. It needs an otherwise idle disposable instance and the original 1.6
installation SQL used to construct the 1.8 fixture.

The released 1.5 path needs an actual old server, not a new server installing old
SQL. In ohdev, build the tag outside the repository, then run the restart test
with an empty work directory and unused ports:

```sh
export UPGRADE_WORK=/tmp/aux-real-upgrade
mkdir -p "$UPGRADE_WORK/source-v18.6" "$UPGRADE_WORK/build-v18.6"
git archive v18.6 | tar -x -C "$UPGRADE_WORK/source-v18.6"
cd "$UPGRADE_WORK/build-v18.6"
../source-v18.6/configure --prefix="$UPGRADE_WORK/v18.6" --with-openssl --enable-depend
make -j12 && make install
cd /w/openhalo/contrib/aux_mysql
ss -ltn
python3 tests/release_real_upgrade.py --old-prefix "$UPGRADE_WORK/v18.6" \
  --new-prefix /path/to/new/install --work-dir "$UPGRADE_WORK/run-debug" \
  --mysql-port 3807 --pg-port 5807 --reference-port 3771
```

Repeat with the release prefix and another empty work directory. The script runs
the server as OS user `pg`, disables 1.5's racing background auto-installer,
keeps a MySQL connection open over SQL upgrade, verifies another new connection,
and stops the server on exit. Logs, original DDL/data, full catalog definitions
and differences remain outside the repository. Every application object is
created over MySQL protocol. Old procedures use the accepted BEGIN ATOMIC body;
a native DO block supplies the trigger function because 1.5 has no MySQL trigger
body parser. CREATE TRIGGER itself is sent through MySQL protocol.

For the separate unreleased 1.6→1.7→1.8→1.9 chain, construct its original
installation SQL from the historical commit, never today's generator:

```sh
git show da426c5:contrib/aux_mysql/sql/aux_mysql.sql > /path/to/new/install/share/postgresql/extension/aux_mysql--1.6.sql
git show da426c5:contrib/aux_mysql/sql/aux_mysql_additions.sql >> /path/to/new/install/share/postgresql/extension/aux_mysql--1.6.sql
python3 tests/release_upgrade.py
```

The shared catalog comparator covers membership, complete function and view
definitions, view descriptors, casts, operators and opclass support members.
Only the explicitly reported historical EXTRA collation is allowed to differ on
the unreleased chain; released 1.5→1.9 must match the fresh catalog exactly.

Run PostgreSQL's own regression suite against the debug kernel with the cluster's
bootstrap superuser and a writable build tree:

```sh
PGHOST=/path/to/socket PGPORT=5792 PGUSER=bootstrap_user \
  make -C /path/to/build-debug/src/test/regress installcheck
```

The standalone binary/prefix harnesses provide additional focused coverage on
stock PG18. They build private modules/clusters under `BUILD_DIR`, never install
into the selected PostgreSQL prefix, and are supplements to the wire tests:

```sh
PG_CONFIG=/path/to/pg18/bin/pg_config BUILD_DIR=/tmp/aux-prefix-check \
  python3 tests/indexes_standalone.py
BINARY_FIX_READONLY_REPO=/path/to/openhalo PG_CONFIG=/path/to/pg18/bin/pg_config \
  BUILD_DIR=/tmp/aux-binary-check tests/binary_fix/run.sh
```

## Route A release recheck

`python3 tests/check_generated.py` regenerates all tracked grammar/SQL outputs
from `pg14` in a temporary directory and compares their bytes.
`python3 tests/route_a.py --port "$MYSQL_PORT" --reference-port 3771 --wp text`
asserts character functions, mixed expressions, value/HEX assignments, and the
intentional 1366 boundary for NUL in every character family and write mode.
The other work packages are selected with `--wp binary`, `char`, `collation`,
`numeric_cast`, `uint64`, `prefix_nul`, and `hints`. The numeric CAST check also uses
`PSQL` (or psql on PATH) over PG at the MySQL port plus 2000.
The standalone expression harness omits executor-only conflict node assignments
when compiled against stock PG18 headers; production parser byte identity is
checked independently and DML is verified by the full wire suites.
