# Contributing

OpenHalo 18 is developed at [`pgsty/openHalo`](https://github.com/pgsty/openHalo). Bug
reports and pull requests are welcome. For a compatibility bug, the most useful report
is the statement, what MySQL returns and what OpenHalo returns.

## Branches

| Branch | Content |
| :--- | :--- |
| `master` | OpenHalo 18: PostgreSQL 18.6 with protocol and execution hooks, plus `contrib/aux_mysql` |
| `pg14` | openHalo 1.0 on PostgreSQL 14 (tag `v1.0-pg14`); also the source the MySQL grammar and SQL layer are generated from |

Releases are tagged `v<PostgreSQL version>.<revision>` on `master`; see [`CHANGELOG.md`](CHANGELOG.md).

## Build

Build with GNU make (Meson is not supported). A development build with assertions:

```sh
./configure --prefix=$HOME/openhalo --with-openssl --enable-debug --enable-cassert --enable-depend
make -j$(nproc) && make install             # kernel and contrib/aux_mysql
```

The kernel and `aux_mysql` share node and callback layouts: always build and install them
together, with `--enable-depend` or from a clean tree after changing a header. Keep build
trees, data directories and logs outside the repository.

## Test

```sh
python3 contrib/aux_mysql/tests/check_generated.py       # generated files match their generators
make check                                               # PostgreSQL regression tests
contrib/aux_mysql/tests/kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport>
PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/run.sh
```

`kernel_instance.sh` creates a disposable instance with the fixture account the suites
use; never point the suites at an instance holding real data.
[`contrib/aux_mysql/TESTING.md`](contrib/aux_mysql/TESTING.md) lists the integration,
upgrade and round-trip suites and what each one needs.

Compare behavior with a real MySQL 5.7 server rather than with the manual alone: most
suites take a reference port and run the same statements against both servers.

## Changing `aux_mysql`

- **Generated files.** `src/parser/mys_gram.y` and `sql/aux_mysql.sql` are generated from
  the `pg14` branch by `tools/port_grammar.py` and `tools/port_aux_sql.py`. Change the
  generator, regenerate, and keep `tests/check_generated.py` passing; do not edit the
  outputs by hand. [`UPSTREAM.md`](contrib/aux_mysql/UPSTREAM.md) records how upstream
  commits were ported.
- **SQL objects.** A change to the extension's SQL objects needs a new extension version
  with an update script from the last released one, so that existing installations can
  run `ALTER EXTENSION aux_mysql UPDATE`. The `Makefile` shows how the install and update
  scripts are assembled.
- **Compatibility choices.** The target is legacy MySQL 5.7 applications, including
  permissive SQL: accept what MySQL accepts and use the nearest semantics, with a
  warning where exact behavior is unavailable. Raise an error only where MySQL does under
  the same `sql_mode`, or to prevent a crash or silent data loss. See the end of
  "What works" in [`contrib/aux_mysql/README.md`](contrib/aux_mysql/README.md), and list
  a deliberate difference under "Known gaps" there.
- **Tests.** Add a case that fails without the change, and check its expected values on
  MySQL 5.7.

## Debugging a MySQL session

A MySQL connection is served by a regular PostgreSQL backend. Find its process and
attach a debugger to it:

```
mysql> SELECT pg_backend_pid();
```

```sh
gdb -p <pid>
```

Server messages go to the PostgreSQL log; `SHOW WARNINGS` shows what the session received.

## Licenses

Kernel changes are under the PostgreSQL License and `contrib/aux_mysql` is under the
GPL v3, as described in the [README](README.md#license). Keep the existing copyright
headers of the files you touch.
