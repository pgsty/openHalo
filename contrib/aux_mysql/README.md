# aux_mysql

openHalo's MySQL Supplementary Extension (version 1.7, same name and settings as in openHalo 1.0) on
PostgreSQL 18. In openHalo 1.0 the MySQL protocol, parser and semantics were compiled into the kernel and
aux_mysql held the SQL layer; here aux_mysql carries all of it, on top of the kernel's generic protocol
extension hooks and its atomic sequence-advancement API. Other kernels need both APIs and separate validation. It must be
loaded through `shared_preload_libraries` (the OpenHalo 18 default).

## Parameters

| Parameter | Default | |
|---|---|---|
| `mysql.listener_on` | on | enable the MySQL listener (restart) |
| `mysql.max_allowed_packet` | 67108864 | incoming message / accumulated long-data byte limit; reload; read through `@@max_allowed_packet` |
| `mysql.port` | 3306 | MySQL listener port, 0 disables it (restart) |
| `mysql.listen_addresses` | (`listen_addresses`) | addresses of the MySQL listener (restart) |
| `mysql.database` | `postgres` | database MySQL clients connect to; MySQL databases are its schemas |
| `mysql.auto_create_extension` | on | install synchronously on the first superuser MySQL connection; ordinary users need prior installation; disabled on standbys and during binary upgrade (restart) |
| `mysql.halo_mysql_version` | `5.7.32-log` | version in the handshake, `version()` and `@@version`; below 8.0 the handshake announces `utf8mb4_general_ci` |
| `mysql.default_authentication_plugin` | `caching_sha2_password` | or `mysql_clear_password` |
| `mysql.auth_cache_size` | 1024 | caching_sha2_password fast-auth cache entries, 0 disables (restart) |
| `mysql.rsa_private_key_file` | `mysql_rsa_private_key.pem` | RSA key for the public key exchange, generated in the data directory if missing (restart) |
| `mysql.statement_rollback` | on | a failing statement inside a transaction rolls back only itself (hidden savepoints) |
| `mysql.default_collation` | `case_insensitive` | collation (schema `mysql`) of string columns without COLLATE; empty: database default |
| `mysql.parser` | `mysql` | dialect of queries over the MySQL protocol: `mysql` or `postgres` |

## What works

- **Connections**: `caching_sha2_password` as in MySQL 8 (fast auth from a shared-memory cache, full auth over
  TLS or RSA-OAEP) checked against the PostgreSQL SCRAM/MD5 verifier, so one password serves both protocols
  and `ALTER ROLE ... PASSWORD` invalidates the cache; TLS through PostgreSQL's `ssl_*` settings,
  `hostssl` and `pg_stat_ssl`; `pg_hba.conf`; `USE` / `COM_INIT_DB`.
- **Prepared statements**: `COM_STMT_PREPARE/EXECUTE/CLOSE/RESET/SEND_LONG_DATA` with binary result sets;
  `COM_RESET_CONNECTION` rolls back, clears variables/prepared statements/temp tables and resets session settings.
- **Transactions**: `autocommit=0`, implicit commits before BEGIN / DDL / `SET autocommit=1`, statement-level
  rollback, a COMMIT of an aborted transaction reports an error. `SET [GLOBAL | SESSION] TRANSACTION`,
  `transaction_isolation` / `tx_isolation` and `transaction_read_only` / `tx_read_only` are accepted and
  shown as set (aliases kept equal); `SET TRANSACTION` without a scope applies to the next transaction
  (as in MySQL, a statement touching a table, COMMIT / ROLLBACK or DDL spends it).
  Access modes take effect (read-only writes report 1792). Isolation follows what MySQL applications rely
  on: READ UNCOMMITTED, READ COMMITTED and read-write REPEATABLE READ run as PostgreSQL READ COMMITTED
  (latest committed rows, no serialization failures, as InnoDB's writes see), read-only REPEATABLE READ
  and `START TRANSACTION WITH CONSISTENT SNAPSHOT` get a real snapshot, SERIALIZABLE is PostgreSQL's,
  and serialization failures report 1213 (SQLSTATE 40001) so drivers retry them as deadlocks.
  `SET GLOBAL read_only` / `super_read_only` stop writes per statement (1290) for ordinary users / for
  everyone; temporary tables stay writable.
- **Semantics**: `/` returns DECIMAL, `DIV`, division by zero gives NULL plus a warning (error 1365 in strict
  mode), case-insensitive string columns by default (`*_ci`; `*_bin` and a non-`_ci` table `COLLATE=` stay
  case-sensitive), lenient conversions when `sql_mode` is not strict, `ON UPDATE CURRENT_TIMESTAMP`,
  AUTO_INCREMENT (identity columns: explicit values advance the counter, `ALTER TABLE ... AUTO_INCREMENT`,
  MODIFY / CHANGE keeping, adding or dropping it). `DECIMAL` without a precision is `DECIMAL(10,0)`;
  `SHOW COLUMNS`, `SHOW CREATE TABLE` and `information_schema.COLUMNS` report `decimal(p,s)`. A numeric
  MySQL cannot type (no typmod, from expressions, views or PostgreSQL DDL) is reported as `decimal(65,30)`
  with a warning: replayed, it keeps values of up to 35 integer and 30 fractional digits.
- **Foreign keys**: enforced with their ON DELETE / ON UPDATE actions, as in InnoDB.
  `FOREIGN_KEY_CHECKS=0` turns off both checking and actions for the session, as in MySQL: it sets
  `session_replication_role = replica` (aux_mysql's own AUTO_INCREMENT and ON UPDATE triggers keep
  firing; other PostgreSQL triggers are skipped), `ALTER TABLE ... ADD FOREIGN KEY` does not check
  existing rows (`NOT VALID`), and `TRUNCATE` of a referenced table deletes just its rows. Turning it
  back on does not recheck existing rows. Any MySQL user may set it, as in MySQL.
- **Dialect**: openHalo's MySQL grammar (backquotes, `LIMIT m,n`, `"strings"`, `UPDATE ... JOIN`, inline
  `KEY`, `COMMENT`, `ALTER TABLE MODIFY/CHANGE/ADD INDEX/DROP PRIMARY KEY`, `ON DUPLICATE KEY UPDATE`,
  `REPLACE`, `INSERT IGNORE`, `CREATE USER ... IDENTIFIED BY`, `SHOW DATABASES/TABLES/COLUMNS/INDEX/CREATE TABLE/
  CHARACTER SET`, user and system variables, MySQL functions from aux_mysql), `ENGINE=` mapped to a table
  access method of that name, `ENCRYPTION='Y'` to `tde_heap` when pg_tde is installed. Executable comments
  `/*! ... */` and `/*!NNNNN ... */` run as SQL when NNNNN is not newer than `mysql.halo_mysql_version`
  (mysqldump's `SET NAMES`, `FOREIGN_KEY_CHECKS`, `TIME_ZONE`, `SQL_MODE` and view definitions).
  `#` comments run to the end of the line as in MySQL (so PostgreSQL's `#` operators are not available
  under `mysql.parser = mysql`); `--` starts a comment only before a space, a control character or the
  end of the text (`1--1` is `1 - -1`), and both end only at `\n`; with multiple statements enabled,
  comments after the last `;` get an OK of their own, as MySQL sends for that empty statement.
- **Prefix key parts** `KEY (col(n))`: a column that may hold values beyond a btree entry (TEXT, BLOB,
  VARCHAR over 600 characters) is not indexed whole, so long values insert as in MySQL. A key starting
  with one becomes a hash index on that column, which serves `=` lookups (also for TEXT / BLOB keys
  without a prefix, which MySQL refuses); a later part becomes `left(col, n)`, at most 600 characters
  in all. A UNIQUE key enforces such a prefix as MySQL does (also for ON DUPLICATE KEY and REPLACE),
  with a hash index for lookups when it is the first part. Other columns are indexed whole.
  `SHOW CREATE TABLE` / `SHOW INDEX` show `col(n)` and `Sub_part` for `left(col, n)` parts.
- **Clients tested**: Connector/J 8.4, go-sql-driver 1.9, PyMySQL, mysql / MariaDB CLI, mysqlslap, sysbench.

## Known gaps

- `SHOW STATUS`, some `SHOW VARIABLES LIKE` forms; `information_schema` mapping is partial (GUI tools and ORMs need more).
- Native ENUM/SET, negative and long-duration TIME, and direct PG14 physical upgrades remain unsupported;
  [migration checks and explicit conversion examples](MIGRATION.md) describe the supported path.
- Statement-level rollback uses a subtransaction per statement inside a transaction block: more than 64
  writing statements in one transaction overflow the PGPROC subxid cache; for bulk loads use autocommit or
  `mysql.statement_rollback = off`.
- `bigint unsigned` is an int8 domain (no values above 2^63-1); no zero dates; `'a' = 'A'` between two
  literals is case-sensitive (literals use the database collation); `1.5e0` is numeric, not DOUBLE.
- No MySQL Unix socket, `KILL [QUERY]`, `COM_CHANGE_USER`, stored procedures.
- Every MySQL collation name in a table or column definition is accepted and mapped to the closest
  aux_mysql collation, not reproduced (in expressions, `COLLATE utf8mb4_bin` is not mapped): `*_ci`
  compares case-insensitively but, unlike MySQL's `*_general_ci` / `*_unicode_ci`, tells accents apart
  (`'e' <> 'é'`); VARCHAR and TEXT do not pad trailing spaces as MySQL's PAD SPACE collations do
  (`'a' <> 'a '`; CHAR(n) ignores them, as in MySQL); other weights follow ICU or code point order. So
  rows MySQL treats as duplicates can coexist under a unique key, and `ON DUPLICATE KEY UPDATE` with
  such a value inserts a new row where MySQL updates the existing one.
- `SHOW CREATE TABLE` shows PostgreSQL collation names (`COLLATE case_insensitive`); they replay into
  aux_mysql, not into MySQL.
- Foreign keys created before PostgreSQL 18 enforcement was enabled in aux_mysql are `NOT ENFORCED`
  (`pg_constraint.conenforced = false`). Re-create them as enforced without checking old rows, then
  validate when convenient:
  ```sql
  DO $$ DECLARE c record; BEGIN
    FOR c IN SELECT conrelid::regclass AS tbl, conname, pg_get_constraintdef(oid) AS def
             FROM pg_constraint WHERE contype = 'f' AND NOT conenforced LOOP
      EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %I, ADD CONSTRAINT %I %s NOT VALID',
                     c.tbl, c.conname, c.conname, replace(c.def, ' NOT ENFORCED', ''));
    END LOOP; END $$;
  ```
  After upgrading the binaries, run `ALTER EXTENSION aux_mysql UPDATE` in the MySQL database so the
  AUTO_INCREMENT and ON UPDATE triggers of existing tables also fire with `FOREIGN_KEY_CHECKS=0` (1.6)
  and SHOW / information_schema report DECIMAL precision and scale, SHOW prefix key parts (1.7).
  With `FOREIGN_KEY_CHECKS=0` a foreign key still needs its referenced table to exist, and a referenced
  table cannot be dropped. `INSERT IGNORE` skips duplicate keys only: a row violating a foreign key
  fails the statement (MySQL skips it with a warning). A parent row still referenced under NO ACTION
  reports 1452 (MySQL: 1451), RESTRICT reports 1451.
- A read-write REPEATABLE READ transaction does not keep one snapshot for its plain SELECTs (see above);
  set `READ ONLY` or `WITH CONSISTENT SNAPSHOT` where a stable view matters.
- A UNIQUE prefix of a column whose values fit a btree entry, and any PRIMARY KEY prefix, is enforced on
  the whole column (a warning says so), which accepts rows that differ only after the prefix; a long
  PRIMARY KEY value can then fail. A key turned into a hash index shows no prefix length. When the
  prefixes of a UNIQUE key may exceed 2000 bytes, its BLOB prefixes are compared by MD5; long
  multi-byte TEXT prefixes can still exceed a btree entry, and so can whole VARCHAR parts of a
  composite key together (~2.7kB after compression), which are not shortened.
- With `autocommit = 0`, a `SET TRANSACTION` issued after the transaction began (even with a SELECT
  without tables) does not reach that transaction.
- PostgreSQL's MVCC differs from InnoDB locking reads for concurrent delete + insert of the same key
  (duplicate key errors where MySQL waits).

## openHalo upstream

The grammar (`src/parser/mys_gram.y`) and the SQL layer (`sql/aux_mysql.sql`) are generated from openHalo's
PG14 sources (branch `pg14` of this repository) by `tools/port_grammar.py` and `tools/port_aux_sql.py`; each
replacement asserts its match count, so upstream changes fail loudly. C code from openHalo lives in
`src/mysm`, with its original copyright headers. `UPSTREAM.md` records how each upstream commit up to
`90fc8d7` was ported.

## Tests

```sh
tests/kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport>   # instance, user dbuser_mysql, schema test
PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> tests/run.sh
MYSQL_PORT=<mysqlport> tests/sysbench.sh
ECO_ROOT=/tmp/eco ECO_PRELOAD=aux_mysql tests/ecosystem/eco_check.sh <name> <prefix> <port>
```

`run.sh` runs the SQL scripts (`p*_*.sql` against `expected/`), PyMySQL, the raw `COM_STMT_*` protocol test,
the mysql CLI and mysqlslap logins, and freshly rebuilt Go / Connector/J driver tests.
Every run keeps separate logs. Exit 0 means all required groups passed; exit 1 means failures;
exit 2 means missing dependencies (incomplete). Set `JAR` to the Connector/J jar and `LOG_DIR`
to a new output directory. `sysbench.sh` separately counts completed and error-free scenarios;
ignored database errors produce a nonzero exit. `ab_regress.py` additionally requires psycopg2.

## License

Derived from openHalo: GNU GPL v3 (`../../LICENSE`), openHalo copyright notice in `COPYRIGHT`.
