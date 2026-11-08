# ![image](logo.png) OpenHalo 18
OpenHalo 18 is openHalo's MySQL compatibility on **PostgreSQL 18.6**: MySQL applications, drivers and the
`mysql` CLI talk to it on port 3306, PostgreSQL clients on 5432, against the same data. Maintained fork of PGSTY

It is built as PostgreSQL 18.6, plus a small, generic patch that lets an extension serve another wire
protocol, plus the `aux_mysql` extension (`contrib/aux_mysql`) that carries all of the MySQL protocol,
dialect and semantics. The kernel also exports an atomic sequence-advancement function.
Parse nodes and catalogs keep their PG18 layouts. `ClientSocket`, an installed server-header
structure, is larger; extensions that use it require review/rebuild. Sample PGDG binaries
(PostGIS, pgvector, Citus, TimescaleDB and pg_cron) passed smoke tests on EL9 aarch64;
this is not a blanket ABI guarantee. See [migration and support boundaries](contrib/aux_mysql/MIGRATION.md).

openHalo provides the capability for PostgreSQL to work with applications written for MySQL but provides much more better performance than MySQL!

Migrating from legacy MySQL databases can be time-consuming and resource-intensive. When migrating your databases, you can automate the migration of your database schema and data using HMT (an opensource migration tool also opened by HaloLab, https://github.com/HaloLab001/hmt-web), but there is often more work to do to migrate the application itself, including re-writing application code that interacts with the database.

openHalo understands the SQL dialect of MySQL, and supports the same communications protocol, so your apps that were originally written for MySQL can work with openHalo with fewer code changes. As a result, the effort required to modify and move applications running on MySQL 5.7 or newer to openHalo is reduced, leading to faster, lower-risk, and more cost-effective migrations.

#### A team of students from IMT Atlantique found a greate project [OpenHalo: Compatibility Testing & Performance Benchmarking](https://github.com/fxcollot/openHaloDocumentation), thanks for their work.

### Benefits
#### Reduce migration time and risk
openHalo supports commonly used MySQL dialect and semantics which reduces the amount of code changes related to database calls in an application. As a result, the amount of application code you need to re-write is minimized, reducing the risk of any new application errors.

#### Obtain better performance
With openHalo, you can obtain better performance without any extra efforts and fees! Especially for complex SQL statements!

#### Migrate at your own pace
With openHalo, you can commonly-used MySQL query tools, commands, and drivers. As a result, you can continue developing with the tools you are familiar with.

### How it works
openHalo supports commonly used SQL dialect and communication protocol of MySQL. You can use openHalo as a MySQL and enabling your legacy applications to communicate with openHalo without extensive code re-writes but provides much more better performance than MySQL!
![image](how%20it%20works.png)

```
PostgreSQL 18.6 + protocol hooks + atomic sequence advancement
  postmaster: also listens on 3306 -> regular backends (max_connections, pg_stat_activity, pg_hba.conf, cancel)
  backend:    handshake -> authenticate -> read command -> parse -> analyze/plan/execute -> results / OK / ERR
       |
aux_mysql (contrib/aux_mysql)
  MySQL wire protocol, caching_sha2_password (TLS / RSA), COM_STMT_* binary protocol,
  MySQL grammar (openHalo mys_gram.y), raw parse tree rewriting, ProcessUtility hook for MySQL DDL,
  autocommit / implicit commit / statement-level rollback, openHalo's aux_mysql function library
```

### OpenHalo 18 and openHalo 1.0

| | openHalo 1.0 (branch `pg14`, tag `v1.0-pg14`) | OpenHalo 18 (this branch) |
|---|---|---|
| Kernel | PostgreSQL 14.18, ~320 kernel files modified | PostgreSQL 18.6 + protocol hooks and sequence API |
| MySQL compatibility | inside the kernel (forked analyzer/executor) | extension `aux_mysql`, preloaded by default |
| Extension ABI | differs from PostgreSQL (`DestReceiver`, `pg_proc`, ACL bits) | sample PG18 binaries tested; ClientSocket layout differs |
| Authentication | `mysql_native_password` | `caching_sha2_password` (MySQL 8 default) over TLS or RSA, verified against the PostgreSQL password; `mysql_clear_password` on request |
| Setup | `CREATE EXTENSION aux_mysql` by hand | installed synchronously on the first superuser MySQL connection |
| Settings | `mysql.listener_on`, `mysql.port`, `mysql.halo_mysql_version`, ... in the core | same names, defined by `aux_mysql` (plus `mysql.database`, `mysql.default_authentication_plugin`, ...); `database_compat_mode` does not exist |

The MySQL dialect and the aux_mysql function library are carried over from openHalo, including upstream
changes up to `90fc8d7`; `contrib/aux_mysql/UPSTREAM.md` lists every upstream commit and how it was ported.

### Quick Tutorial
#### Installation from Source Code
- Build with GNU make; Meson is not supported and fails at configuration.
- Build dependencies (EL 9): `dnf install -y gcc make bison flex perl readline-devel zlib-devel libicu-devel openssl-devel sudo mariadb`
- ICU is needed (MySQL's case-insensitive collations); OpenSSL is recommended: without it there is no TLS and
  no RSA key exchange, so password logins over the MySQL protocol need `mysql_clear_password`.

```sh
./configure --prefix=/usr/local/pgsql --with-openssl
make -j$(nproc) && sudo make install        # builds and installs aux_mysql too
```

#### Server Setup
```sh
sudo useradd postgres                        # the superuser is named after the OS user running initdb
sudo mkdir -p /var/lib/openhalo && sudo chown postgres: /var/lib/openhalo
sudo -iu postgres /usr/local/pgsql/bin/initdb -D /var/lib/openhalo/data
sudo -iu postgres /usr/local/pgsql/bin/pg_ctl -D /var/lib/openhalo/data -l /var/lib/openhalo/server.log start
```

Nothing else to configure: `postgresql.conf` from `initdb` already has

```
unix_socket_directories = '/tmp'
shared_preload_libraries = 'aux_mysql'
mysql.listener_on = true
mysql.port = 3306
mysql.halo_mysql_version = '5.7.32-log'
```

The first MySQL connection by the superuser installs `aux_mysql` synchronously in database `postgres`.
For an application account, install it first with `psql -U postgres -d postgres -c 'CREATE EXTENSION aux_mysql'`.
`mysql.auto_create_extension = off` disables automatic installation; restart after changing it.

#### Query Database using mysql CLI
```sh
mysql -h 127.0.0.1 -P 3306 -u postgres
```
```
mysql> create database app;  use app;
mysql> create table t (id int auto_increment primary key, name varchar(20));
mysql> insert into t(name) values ('Alice'), ('bob');
mysql> select * from t where name = 'ALICE';
+----+-------+
| id | name  |
+----+-------+
|  1 | Alice |
+----+-------+
```

MySQL databases are schemas inside `mysql.database` (default `postgres`), which is where MySQL clients
land. The extension adds global casts to that database, so PostgreSQL applications are better kept in other
databases.

#### Enable remote login with mysql CLI
Logins use `pg_hba.conf` and the PostgreSQL password: the same password works over both protocols.

postgresql.conf
```
listen_addresses = '*'                       # the MySQL listener follows it (or set mysql.listen_addresses)
```

pg_hba.conf
```
host    all             all             0/0                     scram-sha-256
```

```sql
-- mysql CLI, as postgres
create user test identified by 'Test.123';
-- or psql
create role test login password 'Test.123';
```

```sh
mysql -h <Server IP> -P 3306 -u test -p
```

`contrib/aux_mysql/README.md` documents the `mysql.*` parameters, what is supported and the known gaps.

### Source layout
- PostgreSQL 18.6 with the protocol hooks, atomic sequence advancement API, and an explicit rejection of unsupported OpenHalo14 physical upgrades (see `README.postgresql.md`, `COPYRIGHT`).
- Commit "add generic protocol extension hooks": the kernel patch, nothing MySQL specific in it.
- `contrib/aux_mysql`: the extension, its tests (`tests/`) and the tools that port openHalo's grammar and
  SQL (`tools/`).
- OpenHalo defaults: `src/backend/utils/misc/postgresql.conf.sample`, `GNUmakefile.in` (builds aux_mysql
  by default), `contrib/Makefile`.

### Testing
```sh
make check                                   # PostgreSQL regression tests, with aux_mysql preloaded
contrib/aux_mysql/tests/kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport>
PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/run.sh
MYSQL_PORT=<mysqlport> contrib/aux_mysql/tests/sysbench.sh
```

### License
- PostgreSQL: PostgreSQL License, see `COPYRIGHT`.
- The protocol extension hooks are new code under the PostgreSQL License.
- `contrib/aux_mysql` is derived from openHalo and distributed under the GNU GPL v3 (`LICENSE`); openHalo's
  copyright notice is in `contrib/aux_mysql/COPYRIGHT`, third party notices in `3party-legal-notices`.
  Builds that include aux_mysql are therefore distributed under the GPL v3.

Halo, HaloBase are trademarks of Halo Tech Co.,Ltd. <br/>
openHalo is a trademark of Halo Tech Co.,Ltd. <br/>
openHalo official website: https://www.openhalo.org
