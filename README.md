# OpenHalo 18

[![PGSTY: pgsty.com](https://img.shields.io/badge/PGSTY-pgsty.com-slategray?style=flat&logo=cilium&logoColor=white)](https://pgsty.com/)
[![Pigsty: pigsty.io](https://img.shields.io/badge/Pigsty-pigsty.io-slategray?style=flat)](https://pigsty.io/)
[![Version: v18.6](https://img.shields.io/badge/Version-v18.6-slategray?style=flat)](https://github.com/pgsty/openHalo/releases)
[![Extension: aux_mysql 1.9](https://img.shields.io/badge/aux__mysql-1.9-slategray?style=flat)](contrib/aux_mysql/README.md)
[![License: PostgreSQL / GPL-3.0](https://img.shields.io/badge/License-PostgreSQL%20%2F%20GPL--3.0-slategray?style=flat&logo=gnu&logoColor=white)](LICENSE)
[![GitHub Stars](https://img.shields.io/github/stars/pgsty/openHalo?style=flat&logo=github&logoColor=black&color=slategray)](https://star-history.com/#pgsty/openHalo&Date)
[![PostgreSQL: 18.6](https://img.shields.io/badge/PostgreSQL-18.6-%233E668F?style=flat&logo=postgresql&logoColor=white&labelColor=3E668F)](README.postgresql.md)
[![MySQL: 5.7 / 8.0](https://img.shields.io/badge/MySQL-5.7%20%2F%208.0-%234479A1?style=flat&logo=mysql&logoColor=white&labelColor=4479A1)](contrib/aux_mysql/README.md)

**OpenHalo 18** is a PostgreSQL 18.6 kernel and `aux_mysql` (`1.9`) extension that speaks the MySQL wire protocol and SQL dialect natively—allowing MySQL clients, drivers, and applications on port `3306` and PostgreSQL clients on port `5432` to query the same data concurrently.

[PGSTY](https://pgsty.com) | [Pigsty](https://pigsty.io) | [`aux_mysql` Docs](contrib/aux_mysql/README.md) | [Migration Guide](contrib/aux_mysql/MIGRATION.md) | [Testing Guide](contrib/aux_mysql/TESTING.md) | [Upstream Port Log](contrib/aux_mysql/UPSTREAM.md) | [Releases](https://github.com/pgsty/openHalo/releases) | [Issues](https://github.com/pgsty/openHalo/issues) | [Upstream](https://github.com/HaloTech-Co-Ltd/openHalo)

---

> [!IMPORTANT]
> **About This Fork**: [`pgsty/openHalo`](https://github.com/pgsty/openHalo) (**OpenHalo 18**) is an independent fork maintained by **[PGSTY](https://pgsty.com)**, derived from [`HaloTech-Co-Ltd/openHalo`](https://github.com/HaloTech-Co-Ltd/openHalo) (`1.0` on PostgreSQL 14.18).
> Rather than maintaining a monolithic fork across ~320 PostgreSQL 14 kernel files, OpenHalo 18 rebases onto **PostgreSQL 18.6** with a compact set of protocol, execution, sequence, foreign-key, and collation hooks, and moves the entire MySQL wire protocol, parser, DDL/DML rewriter, and runtime semantics into the preloaded [`aux_mysql`](contrib/aux_mysql/README.md) (`1.9`) extension.

---

## Architecture

OpenHalo 18 separates generic kernel extension points from MySQL-specific protocol and SQL compatibility:

```text
PostgreSQL 18.6 + protocol/execution hooks + atomic sequence advancement
  postmaster: listens on 5432 (PostgreSQL) & 3306 (MySQL) -> regular backends
              (shared max_connections, pg_stat_activity, pg_hba.conf, query cancel)
  backend:    handshake -> authenticate -> read command -> parse -> analyze/plan/execute -> results / OK / ERR
       |
aux_mysql 1.9 (contrib/aux_mysql)
  MySQL wire protocol, caching_sha2_password (TLS / RSA), COM_STMT_* binary protocol,
  MySQL grammar (mys_gram.y), raw parse tree rewriting, ProcessUtility hook for MySQL DDL,
  autocommit / implicit commit / statement-level rollback, MySQL types, operators & functions
```

Catalog layouts remain PostgreSQL 18 compatible, while protocol, parse, and plan node structures include OpenHalo 18 hook fields. Always build the kernel and `aux_mysql` together, and rebuild and validate third-party C extensions against these headers (see [Migration & Support Boundaries](contrib/aux_mysql/MIGRATION.md)).

---

## OpenHalo 18 vs openHalo 1.0

| Dimension | openHalo 1.0 (branch `pg14`, tag `v1.0-pg14`) | OpenHalo 18 (`pgsty/openHalo`) |
| :--- | :--- | :--- |
| **PostgreSQL Base** | PostgreSQL 14.18 (~320 kernel files modified) | **PostgreSQL 18.6** + targeted protocol/execution hooks & sequence API |
| **MySQL Layer** | Compiled into the kernel (forked analyzer & executor) | **`aux_mysql` (`1.9`) extension**, preloaded by default |
| **Extension ABI** | Diverges from PG (`DestReceiver`, `pg_proc`, ACL bits) | PG18 catalogs preserved; protocol/parse/plan structs extended (rebuild extensions) |
| **Authentication** | `mysql_native_password` | **`caching_sha2_password`** (MySQL 8 default) over TLS or RSA against PG passwords; `mysql_clear_password` on request |
| **Initialization** | Manual `CREATE EXTENSION aux_mysql` | Installed synchronously on the first superuser MySQL connection (`mysql.auto_create_extension = on`) |
| **Configuration** | `mysql.*` GUCs hardcoded in core | Same `mysql.*` GUC names defined by `aux_mysql` (`database_compat_mode` removed) |

The MySQL dialect and `aux_mysql` SQL library incorporate all upstream `openHalo` changes up to commit `90fc8d7`; see [`contrib/aux_mysql/UPSTREAM.md`](contrib/aux_mysql/UPSTREAM.md) for the commit-by-commit port log.

---

## Features & Capabilities

- **Dual-Protocol Single Engine**: Native PostgreSQL wire protocol (`5432`) and MySQL wire protocol (`3306`) served by standard PostgreSQL backends against the same tables, transactions, and `pg_hba.conf` rules.
- **MySQL 8.0 & 5.7 Wire Compatibility**: Supports `caching_sha2_password` (TLS and ephemeral RSA-2048 key exchange verified against PostgreSQL `scram-sha-256` / `md5` / plaintext verifiers), TLS upgrade (`SSLRequest`), `COM_QUERY` multi-statements, `COM_PING`, `COM_INIT_DB`, `COM_RESET_CONNECTION`, and server-side prepared statements (`COM_STMT_PREPARE` / `EXECUTE` / `SEND_LONG_DATA` / `CLOSE` / `RESET`) with binary result sets.
- **MySQL SQL Dialect & DDL Rewriting**: Bison grammar (`mys_gram.y`) and parse-tree rewriter supporting backtick identifiers, double-quoted strings, `AUTO_INCREMENT`, `INSERT ... ON DUPLICATE KEY UPDATE`, `INSERT IGNORE`, `REPLACE INTO`, multi-table `UPDATE` / `DELETE` (`JOIN` and `ORDER BY ... LIMIT`), `ALTER TABLE` (`MODIFY`, `CHANGE`, `ADD/DROP INDEX/FOREIGN KEY`), `SHOW` commands, and `information_schema` / `performance_schema` / `sys` views.
- **MySQL Data Types & Semantics**: Length-preserving `BINARY(n)` and `VARBINARY(n)` (`mysql.binary_n`, `mysql.varbinary_n`), hybrid hex/bit literals (`mysql.hexlit`), case-insensitive `utf8mb4_general_ci` (`PAD SPACE`) and `utf8mb4_bin` collations, exact prefix indexes (`mysql_prefix` AM), `VIRTUAL` and `STORED` generated columns, MySQL decimal division, and session/global system variables (`@@var`) and user variables (`@var`).
- **MySQL Transaction & Lock Semantics**: Lazy transaction start under `autocommit = 0`, statement-level rollback on errors without aborting the surrounding transaction, implicit commits around DDL, `FOREIGN_KEY_CHECKS=0` deferred parent creation, and real session-level advisory locks (`GET_LOCK`, `RELEASE_LOCK`, `IS_FREE_LOCK`, `IS_USED_LOCK`, `RELEASE_ALL_LOCKS`).

For full compatibility details and known boundaries, see [`contrib/aux_mysql/README.md`](contrib/aux_mysql/README.md).

---

## Quick Start

### 1. Build from Source

- Build with GNU Make (Meson is not supported and fails at configuration).
- Build dependencies (EL 9):
  ```bash
  dnf install -y gcc make bison flex perl readline-devel zlib-devel libicu-devel openssl-devel sudo mariadb
  ```
- **ICU** is required for MySQL case-insensitive collations; **OpenSSL** is strongly recommended for TLS and RSA password exchange over the MySQL protocol.

```bash
./configure --prefix=/usr/local/pgsql --with-openssl
make -j$(nproc) && sudo make install        # builds and installs both kernel and aux_mysql
```

### 2. Initialize & Start Server

```bash
sudo useradd postgres                        # superuser is named after the OS user running initdb
sudo mkdir -p /var/lib/openhalo && sudo chown postgres: /var/lib/openhalo
sudo -iu postgres /usr/local/pgsql/bin/initdb -D /var/lib/openhalo/data
sudo -iu postgres /usr/local/pgsql/bin/pg_ctl -D /var/lib/openhalo/data -l /var/lib/openhalo/server.log start
```

`postgresql.conf` generated by `initdb` comes preconfigured with OpenHalo defaults:

```ini
unix_socket_directories = '/tmp'
shared_preload_libraries = 'aux_mysql'
mysql.listener_on = true
mysql.port = 3306
mysql.halo_mysql_version = '5.7.32-log'
```

The first MySQL connection by a superuser automatically installs `aux_mysql` in `mysql.database` (default `postgres`). If connecting first with a non-superuser account, create the extension beforehand via `psql -U postgres -d postgres -c 'CREATE EXTENSION aux_mysql;'`.

### 3. Connect with MySQL CLI

```bash
mysql -h 127.0.0.1 -P 3306 -u postgres
```

```sql
mysql> CREATE DATABASE app; USE app;
mysql> CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(20));
mysql> INSERT INTO t(name) VALUES ('Alice'), ('bob');
mysql> SELECT * FROM t WHERE name = 'ALICE';
+----+-------+
| id | name  |
+----+-------+
|  1 | Alice |
+----+-------+
```

MySQL databases map to PostgreSQL schemas inside `mysql.database` (default `postgres`). Because `aux_mysql` installs database-wide casts in `mysql.database`, keep pure PostgreSQL workloads in separate PostgreSQL databases if you want unmodified stock cast behavior.

### 4. Enable Remote Access & Authentication

MySQL connections authenticate against `pg_hba.conf` and PostgreSQL role passwords—the same role and password work across both port `5432` and port `3306`.

In `postgresql.conf`:

```ini
listen_addresses = '*'                       # MySQL listener follows listen_addresses (or mysql.listen_addresses)
```

In `pg_hba.conf`:

```text
host    all             all             0.0.0.0/0               scram-sha-256
```

Create a user via `mysql` or `psql` and connect remotely:

```sql
-- via mysql CLI (as superuser)
CREATE USER test IDENTIFIED BY 'Test.123';

-- or via psql
CREATE ROLE test LOGIN PASSWORD 'Test.123';
```

```bash
mysql -h <Server_IP> -P 3306 -u test -p
```

---

## Configuration (GUC Parameters)

All parameters are defined by `aux_mysql` under the `mysql.*` prefix:

| Parameter | Default | Context | Description |
| :--- | :--- | :--- | :--- |
| `mysql.listener_on` | `on` | `postmaster` | Listen for MySQL wire connections on `mysql.port` |
| `mysql.port` | `3306` | `postmaster` | TCP port for the MySQL protocol listener |
| `mysql.listen_addresses` | `""` | `postmaster` | Bind addresses for MySQL listener (empty = inherit `listen_addresses`) |
| `mysql.database` | `"postgres"` | `sighup` | PostgreSQL database where MySQL connections land |
| `mysql.auto_create_extension` | `on` | `sighup` | Auto-run `CREATE EXTENSION aux_mysql` on first superuser MySQL login |
| `mysql.halo_mysql_version` | `"5.7.32-log"` | `sighup` | Server version string reported in handshake and `@@version` |
| `mysql.default_authentication_plugin` | `caching_sha2_password` | `sighup` | Handshake auth plugin (`caching_sha2_password` or `mysql_clear_password`) |
| `mysql.require_secure_transport` | `off` | `sighup` | Require TLS on MySQL connections |
| `mysql.explicit_defaults_for_timestamp` | `off` | `userset` | Standard vs MySQL traditional `TIMESTAMP` nullability/default rules |
| `mysql.ci_collation` | `on` | `userset` | Apply `mysql.utf8mb4_general_ci` (`PAD SPACE`) to string columns by default |
| `mysql.Standard_ParserEngine_Auxiliary` | `off` | `userset` | Fall back to the PostgreSQL parser when a statement fails the MySQL parser |
| `mysql.table_access_method_orioledb` | `""` | `userset` | Target table AM for `ENGINE=OrioleDB` |
| `mysql.table_access_method_tde` | `""` | `userset` | Target table AM for `ENCRYPTION='Y'` |

---

## Upgrading & Migration

- **Updating `aux_mysql` (`1.5` → `1.9`)**: Install the new OpenHalo 18 binaries and extension files, restart PostgreSQL, and run in `mysql.database`:
  ```sql
  ALTER EXTENSION aux_mysql UPDATE TO '1.9';
  ```
- **Migrating from openHalo 1.0 (PG 14)**: OpenHalo 18 uses PostgreSQL 18 catalogs and is **not** binary-upgrade (`pg_upgrade`) compatible with openHalo 14 data directories. Migrate via logical dump and restore after running the read-only preflight tools (`tools/check_pg14_config.py` and `tools/check_pg14_schema.sql`). See the full [Migration Guide](contrib/aux_mysql/MIGRATION.md).

---

## Source Layout

- **PostgreSQL 18.6 Kernel Hooks**: `src/backend/postmaster/protocol_extension.c`, `src/include/postmaster/protocol_extension.h`, plus targeted parser, executor, sequence, trigger, locale, and `pg_upgrade` guards (see [`README.postgresql.md`](README.postgresql.md) and [`COPYRIGHT`](COPYRIGHT)).
- **`contrib/aux_mysql/`**: The MySQL wire protocol, grammar, rewrite engine, runtime library, regression suites (`tests/`), and PG14→PG18 code generators (`tools/`).
- **Build & Config Defaults**: `src/backend/utils/misc/postgresql.conf.sample`, `GNUmakefile.in`, and `contrib/Makefile`.

---

## Testing

```bash
# 1. Verify byte-for-byte reproducibility of generated grammar and SQL scripts
python3 contrib/aux_mysql/tests/check_generated.py

# 2. PostgreSQL core regression suite (with aux_mysql preloaded)
make check

# 3. Provision a disposable test instance and run the MySQL smoke, driver & A/B suites
contrib/aux_mysql/tests/kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport>
PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/run.sh

# 4. sysbench OLTP benchmark over the MySQL protocol
MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/sysbench.sh
```

See [`contrib/aux_mysql/TESTING.md`](contrib/aux_mysql/TESTING.md) for the complete `1.9` integration, upgrade, roundtrip, and ecosystem test suites.

---

## Documentation

- **[`contrib/aux_mysql/README.md`](contrib/aux_mysql/README.md)**: Complete `aux_mysql` 1.9 architecture, authentication, prepared statements, SQL dialect, type/index semantics, and operational notes.
- **[`contrib/aux_mysql/MIGRATION.md`](contrib/aux_mysql/MIGRATION.md)**: OpenHalo 14 → 18 logical migration workflow, preflight scripts, and post-upgrade repair procedures.
- **[`contrib/aux_mysql/TESTING.md`](contrib/aux_mysql/TESTING.md)**: Verification suites and isolated test instance instructions.
- **[`contrib/aux_mysql/UPSTREAM.md`](contrib/aux_mysql/UPSTREAM.md)**: Upstream `HaloTech-Co-Ltd/openHalo` synchronization and porting record.
- **[`README.postgresql.md`](README.postgresql.md)**: Upstream PostgreSQL 18 documentation and resources.

---

## License

OpenHalo 18 inherits the licenses of its upstream projects:

- **PostgreSQL 18.6 Kernel & Protocol Extension Hooks**: Licensed under the **[PostgreSQL License](COPYRIGHT)**.
- **`contrib/aux_mysql` Extension**: Derived from `openHalo` and licensed under the **[GNU General Public License v3.0 (GPLv3)](LICENSE)** (see [`contrib/aux_mysql/COPYRIGHT`](contrib/aux_mysql/COPYRIGHT) and [`3party-legal-notices`](3party-legal-notices)). Combined builds that include `aux_mysql` are distributed under the **GPLv3**.

### Copyright & Attribution

- Portions Copyright (c) 1996-2026 PostgreSQL Global Development Group
- Portions Copyright (c) 1994 The Regents of the University of California
- Copyright (c) 2019-2026 Halo Tech Co., Ltd. (`openHalo`, https://www.openhalo.org)
- Copyright (c) 2026 PGSTY PTE. LTD.
- Maintained by [Ruohang Feng](mailto:rh@vonng.com) @ [PGSTY](https://pgsty.com)

*Halo, HaloBase, and openHalo are trademarks of Halo Tech Co., Ltd.*
