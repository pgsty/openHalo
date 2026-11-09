# aux_mysql

openHalo's MySQL Supplementary Extension (version 1.9, same name and settings as in openHalo 1.0) on
PostgreSQL 18. In openHalo 1.0 the MySQL protocol, parser and semantics were compiled into the kernel and
aux_mysql held the SQL layer; here the extension implements MySQL behavior with the matching
OpenHalo kernel's protocol, executor, foreign-key and collation hooks. The node and callback
layouts must match: build the kernel and extension together. Other kernels require adaptation
and separate validation. Load it through `shared_preload_libraries` (the OpenHalo 18 default).

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
- **Connection initialization**: `SET NAMES charset [COLLATE collation]`, quoted names,
  `SET NAMES DEFAULT`, `SET CHARACTER SET` and `SET CHARSET` (including DEFAULT).
  Client/results/connection variables are updated together; CHARACTER SET takes the connection
  defaults from the current database variables. Storage and wire strings remain UTF-8: other
  encodings/locales map to UTF-8 general_ci/bin with warning 1105. Literal comparisons still have
  the collation limitation below. Character result metadata uses MySQL 5.7's charset number 45;
  binary results retain 63, including prepared statements.
- **Prepared statements**: `COM_STMT_PREPARE/EXECUTE/CLOSE/RESET/SEND_LONG_DATA` with binary result sets;
  SQL `PREPARE name FROM 'text' | @var`, `EXECUTE name [USING @a, ...]`, `DEALLOCATE | DROP PREPARE name`
  (names are case-insensitive, a new PREPARE replaces the statement, `USING` also takes other expressions);
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
  everyone, including persistent sequence writes; temporary tables stay writable.
- **Semantics**: `/` returns DECIMAL, `DIV`, division by zero gives NULL plus a warning (error 1365 in strict
  mode), case-insensitive string columns by default (`*_ci`; `*_bin` and a non-`_ci` table `COLLATE=` stay
  case-sensitive), lenient conversions when `sql_mode` is not strict, `ON UPDATE CURRENT_TIMESTAMP`,
  AUTO_INCREMENT (identity columns: explicit values advance the counter, `ALTER TABLE ... AUTO_INCREMENT`,
  MODIFY / CHANGE keeping, adding or dropping it). `DECIMAL` without a precision is `DECIMAL(10,0)`;
  `SHOW COLUMNS`, `SHOW CREATE TABLE` and `information_schema.COLUMNS` report `decimal(p,s)`. A numeric
  MySQL cannot type (no typmod, from expressions, views or PostgreSQL DDL) is reported as `decimal(65,30)`
  with a warning: replayed, it keeps values of up to 35 integer and 30 fractional digits.
- **Foreign keys**: enforced with their ON DELETE / ON UPDATE actions, as in InnoDB.
  `FOREIGN_KEY_CHECKS=0` disables only foreign-key checks/actions; user triggers, AUTO_INCREMENT and
  ON UPDATE triggers still run. Referenced-table TRUNCATE remains TRUNCATE and does not cascade.
  Missing or dropped parents retain durable FK declarations, visible in SHOW and information_schema.
  Recreating the parent restores enforcement and actions without checking historical rows.
  With checks enabled, new invalid child keys fail with 1452; blocked parent writes fail with 1451.
  `INSERT IGNORE` skips each invalid FK row with a warning. Turning checks back on does not validate
  old rows. Any MySQL user may set this session variable, as in MySQL.
- **Dialect**: openHalo's MySQL grammar (backquotes, `LIMIT m,n`, `"strings"`, `UPDATE ... JOIN`, inline
  `KEY`, `COMMENT`, `ALTER TABLE MODIFY/CHANGE/ADD INDEX/DROP PRIMARY KEY`, `ON DUPLICATE KEY UPDATE`,
  `REPLACE`, `INSERT IGNORE`, `CREATE USER ... IDENTIFIED BY`, `SHOW DATABASES/TABLES/COLUMNS/INDEX/CREATE TABLE/
  CHARACTER SET`, user and system variables, `DO expr, ...`, MySQL functions from aux_mysql; PostgreSQL's
  `DO 'block'` needs its `LANGUAGE` clause, a bare string is a MySQL expression), `ENGINE=` mapped to a table
  access method of that name, `ENCRYPTION='Y'` to `tde_heap` when pg_tde is installed. Executable comments
  `/*! ... */` and `/*!NNNNN ... */` run as SQL when NNNNN is not newer than `mysql.halo_mysql_version`
  (mysqldump's `SET NAMES`, `FOREIGN_KEY_CHECKS`, `TIME_ZONE`, `SQL_MODE` and view definitions).
  `#` comments run to the end of the line as in MySQL (so PostgreSQL's `#` operators are not available
  under `mysql.parser = mysql`); `--` starts a comment only before a space, a control character or the
  end of the text (`1--1` is `1 - -1`), and both end only at `\n`; with multiple statements enabled,
  comments after the last `;` get an OK of their own, as MySQL sends for that empty statement.
  As in MySQL, a string literal keeps the backslash of `\%` and `\_`, so `LIKE 'a\_b'` matches only `a_b`
  and `'50\%'` is four characters.
- **Binary and character strings**: new BINARY(n) columns pad with zero bytes; VARBINARY(n) enforces
  its byte limit. Strict writes reject overflow, while non-strict and INSERT IGNORE writes truncate
  with warnings. BLOB values preserve every byte. TEXT-family columns use native PostgreSQL text.
  CHAR/VARCHAR/TEXT reject NUL with error 1366 in every sql_mode, including IGNORE.
  Text and prepared result packets retain complete values. Binary-to-character assignments and
  CAST preserve UTF8 bytes; invalid UTF8 and NUL (including BINARY padding) produce 1366.
  Literal backslash escapes, `_binary`, hex/bit literals and BLOB parameters preserve binary payloads.
  Native PostgreSQL I/O for these new types uses bytea escaping, so pg_dump can round-trip NUL and
  non-UTF8 binary bytes. Existing columns keep their old types until explicitly migrated.
- **Hexadecimal and bit-value literals** (`X'41'`, `0x41`, `b'101'`, `0b101`; aux_mysql 1.8, type
  `mysql.hexlit`) are binary strings that are numbers in numeric context, as in MySQL: `SELECT 0x41`
  returns `A` (VARBINARY), `0x41 + 0` is 65, `0x10 = 16`, `flags & 0x10`, `CONCAT(X'41', 'b')` is `Ab`,
  `X'41' = 'A'`, `b'101'` is 5 and `0x05`. Arithmetic and bit operations with numbers, BIT columns, strings
  and BLOBs (read as numbers, as MySQL does) yield BIGINT (`/` DECIMAL, with a DECIMAL or DOUBLE operand
  that type); comparisons with numbers and BIT columns compare the value, with strings, BLOBs and other
  literals the bytes (case-sensitively). INSERT / UPDATE store the number into numeric columns, the bytes
  into string and binary columns (a string column rejects bytes that are not valid text with 1366) and the
  bits into BIT(n) (1406 when they do not fit); a column DEFAULT stores the value; a literal column of
  `CREATE TABLE ... AS` / `CREATE VIEW` is VARBINARY. `WHERE binary_col = X'..'` and `IN (X'..', ...)` use
  an index. `HEX()` / `UNHEX()`: strings, BLOBs and literals byte by byte, numbers rounded to 64 bits in
  two's complement (`HEX(-1)` is 16 `F`s), BIT columns as numbers; UNHEX of a non-digit is NULL with 1411.
- **Prefix key parts** `KEY (col(n))`: UNIQUE and PRIMARY prefixes enforce the declared character
  prefix (binary prefixes count bytes), including short columns and all composite components.
  Oversized keys use the `mysql_prefix` access method: bounded index keys select candidates, then
  original heap values decide equality. Hash collisions never decide uniqueness. The same indexes
  arbitrate ODKU/REPLACE. SHOW preserves names, component order and prefix lengths; native dumps and
  CREATE TABLE LIKE preserve the access method and its options. No auxiliary hash index is created.
  Unprefixed, bounded short keys keep native btree scans, including ordered and index-only scans.
- **User-level locks**: `GET_LOCK`, `RELEASE_LOCK`, `IS_FREE_LOCK`, `IS_USED_LOCK` and `RELEASE_ALL_LOCKS`
  as in MySQL 5.7: a session can hold several locks, and one lock several times; names are compared
  case-insensitively (at most 64 characters, else error 3057); the timeout is in seconds, a negative one
  waits forever; transactions neither take nor release the locks, `COM_RESET_CONNECTION` and the end of
  the session do; the session that closes a cycle of waits gets error 3058. They are session-level
  advisory locks (`pg_locks`: `locktype = 'advisory'`, `objsubid = 19801`) of the MySQL database, and
  take room in the shared lock table (`max_locks_per_transaction`).
- **Key names** are per table, as in MySQL: tables can share key names (`KEY idx_created` in each table
  of a dump), a key can be named like a table, and a key without a name is named after its first
  column (`a`, `a_2`, ...). PostgreSQL names indexes per schema, so the index of key `k` is named
  `k$<8 hex digits>`. `SHOW INDEX`, `SHOW CREATE TABLE`, `information_schema.STATISTICS` and
  duplicate-key errors show the key name; `DROP INDEX k ON t` and `ALTER TABLE t DROP INDEX k` /
  `RENAME INDEX` find the key of that table (`PRIMARY` too); a key name used twice in one table is
  error 1061. The index name alone carries the key name, so it survives `pg_dump`, `pg_upgrade` and
  `RENAME TABLE`; an index without the suffix (from PostgreSQL, or created before 1.8) is known by its
  name. `CREATE TABLE ... LIKE` keeps the key names.
- **Generated columns** `col type [GENERATED ALWAYS] AS (expr) [VIRTUAL | STORED]`: computed as in MySQL;
  VIRTUAL ones are stored too (PostgreSQL's virtual columns can neither be indexed nor use aux_mysql's
  functions). Only `DEFAULT` can be written to them (3105 otherwise), also through REPLACE and
  `ON DUPLICATE KEY UPDATE`. A generated column may read the generated columns defined before it, and
  `ALTER TABLE ... MODIFY / CHANGE` recomputes the columns that read a redefined column, also when a
  column they read changes type. CREATE, ALTER and LIKE check resolved expressions before planning:
  random numbers, current time, user/session variables, external reads and unapproved functions
  are rejected with 3102. Reviewed compatibility implementations (CONCAT, IFNULL, `/`, DIV,
  DATE_FORMAT) remain usable even when PostgreSQL marks them mutable. These use an IMMUTABLE SQL function
  `aux_mysql_gen_<hash>` in the table's schema, removed by DROP TABLE / ALTER TABLE once unused.
  New bounded VARCHAR/CHAR and DECIMAL generators apply the writer's current sql_mode and IGNORE
  rules, including constant expressions and prepared executions; non-strict overflow/truncation
  reports a warning. Arbitrary user-defined functions are not admitted by this compatibility path.
  `SHOW COLUMNS`, `SHOW CREATE TABLE` and `information_schema.COLUMNS` show them as MySQL does: no default,
  Extra `VIRTUAL GENERATED` / `STORED GENERATED` (so mysqldump leaves them out of its INSERTs),
  `GENERATION_EXPRESSION`, `GENERATED ALWAYS AS (expr) VIRTUAL | STORED`.
- **Clients tested**: Connector/J 8.4, go-sql-driver 1.9, PyMySQL, mysql / MariaDB CLI, mysqlslap, sysbench.

Compatibility targets legacy MySQL 5.7 applications, including permissive SQL.
Accept MySQL syntax and use the nearest semantics with warnings where exact
behavior is unavailable. Errors are reserved for MySQL errors under the same
sql_mode, crash prevention, and preventing silent data corruption or loss. Successful execution must not hide lost bytes, ineffective safety
settings, or silently weakened constraints. Non-strict sql_mode and INSERT IGNORE
retain their documented conversions and warnings; they do not authorize arbitrary
lossy substitutions. The read-write REPEATABLE READ mapping described above is an
explicit product choice.

## Known gaps

- `SHOW STATUS` and the system-variable catalog remain incomplete; SHOW VARIABLES reports current
  values for the declared variables. Constraint metadata now
  includes PRIMARY/UNIQUE/FK declarations and ordered key columns; broader information_schema
  compatibility is still partial.
- A key name longer than 54 bytes (18 CJK characters) is shortened to that length (PostgreSQL index
  names hold 63 bytes, and the suffix takes 9); key names compare case-insensitively in ASCII only.
- Native ENUM/SET, negative and long-duration TIME, and direct PG14 physical upgrades remain unsupported;
  [migration checks and explicit conversion examples](MIGRATION.md) describe the supported path.
- Statement-level rollback uses a subtransaction per statement inside a transaction block: more than 64
  writing statements in one transaction overflow the PGPROC subxid cache; for bulk loads use autocommit or
  `mysql.statement_rollback = off`.
- `bigint unsigned` is an int8 domain (no values above 2^63-1); no zero dates; `'a' = 'A'` between two
  literals is case-sensitive (literals use the database collation); `1.5e0` is numeric, not DOUBLE.
- Old BINARY/VARBINARY columns are bytea domains with discarded lengths. Installing 1.9 does not
  infer their original lengths or pad historical values. TEXT-family columns retain native PG text storage. Use the original schema and the [migration procedure](MIGRATION.md) to convert them.
- Index hints accept USE/IGNORE/FORCE INDEX or KEY, optional FOR JOIN/ORDER BY/GROUP BY,
  aliases, repeated hints and index lists (including empty USE lists). All hints are ignored.
- BIGINT UNSIGNED writes above INT64_MAX fail with 1264 in every sql_mode and under
  IGNORE, including positive overflow beyond UINT64_MAX: MySQL's clipped UINT64_MAX also
  cannot fit int8. The statement rolls back before a wrong unique-key update or replacement.
  Non-strict negative inputs still clip to 0 with warning 1264.
  Hexadecimal numeric contexts above signed int8 remain limited; BIGINT UNSIGNED is not a full
  unsigned 64-bit type. Common string operations, binary CONCAT and indexed integer comparisons are
  covered, but arbitrary combinations of MySQL functions and custom types are not fully implemented.
  BIT result metadata and user-variable numeric typing still differ from MySQL.
- TRUE/FALSE and boolean results (comparisons, IF, CASE) are MySQL's integers 1/0 when stored:
  numbers 1/0 and strings `'1'`/`'0'` in every sql_mode, including ODKU/REPLACE key arbitration and
  generated columns. Boolean expressions assigned to DECIMAL, DOUBLE or VARBINARY are still rejected
  with 1105 (literal TRUE/FALSE are accepted). Generated columns defined before 1.9 keep their stored
  expression, and existing `'true'` strings or zeros are not rewritten.
- CHAR/VARCHAR combinations in IFNULL/IF and boolean CONCAT/GROUP_CONCAT are supported.
  Broader mixed types, such as integer/VARCHAR or VARBINARY/CHAR conditional results, remain
  incomplete. Multi-argument GROUP_CONCAT is also incomplete; `GROUP_CONCAT(CONCAT(...))`
  supports explicit per-row concatenation.
- Deferred DML forms: `DELETE target FROM ... JOIN ...` (including comma joins and orphan
  cleanup with LEFT JOIN) and true multi-target UPDATE/DELETE. The supported single-target
  `DELETE FROM target USING ...` form does not imply support for those spellings.
- Accepted 1.9 differences: with PAD_CHAR_TO_FULL_LENGTH, some search/string-function inputs
  still lose CHAR padding, although stored bytes are unchanged; binary numeric CAST may reject
  extreme exponents such as `1e1000000`/`1e-1000000` and negative SIGNED overflow; empty-needle
  `LOCATE('', multibyte_string, pos)` returns a character position rather than MySQL's byte
  offset at these boundaries. These specific differences are not claims of exact compatibility.
- CHAR/VARCHAR and all TEXT families retain native PostgreSQL character storage: values containing NUL
  fail with 1366. Bytes are never stripped or replaced to make an insert succeed. Only BLOB/BINARY/VARBINARY preserve NUL and arbitrary bytes. Changing a column to
  a binary type is an explicit migration decision, including its length and comparison rules.
- No MySQL Unix socket, `KILL [QUERY]`, `COM_CHANGE_USER`, stored procedures.
- Four names have exact dedicated implementations: `utf8_general_ci`, `utf8mb4_general_ci`, `utf8_bin`,
  and `utf8mb4_bin`. Other MySQL collation names are accepted and mapped by their suffix to the closest
  rule: `_ci` names use MySQL 5.7 `general_ci` weights with PAD SPACE, and other names use binary
  codepoint order with PAD SPACE. Every approximation emits warning 1105 through `SHOW WARNINGS`,
  including expression-level `COLLATE`, `CREATE TABLE AS`, and views; `SHOW CREATE` shows the effective
  `utf8mb4_general_ci` or `utf8mb4_bin` name. Language-specific and unicode_ci tailoring is not
  reproduced, so comparisons, ordering, UNIQUE and `ON DUPLICATE KEY` arbitration can differ.
  Existing ICU collations and indexes are not changed in place. Rebuild from reviewed original DDL to
  adopt the new rules and resolve duplicates before cutover.
- True prefix/oversized indexes require heap rechecks and do not provide index-only scans or ORDER BY
  ordering. Large groups sharing the same candidate prefix add work during scans and unique checks.
  Benchmark such distributions; ordinary bounded full-column keys retain native btree behavior.
- ODKU checks every unique key; REPLACE deletes each conflicting row and inserts the proposed row,
  applying defaults, FK actions and triggers. Repeated keys in one statement are processed in order.
  Their affected counts include actual replaced rows and distinguish changed/unchanged updates.
  A plain UPDATE still counts matching rows rather than only changed rows.
- Generated VIRTUAL columns are physically stored and use the writer's expression and conversion
  settings. A later reader's sql_mode does not recompute existing values. Row-only deterministic
  expressions retain their values; expressions affected by session settings need migration review.
  This trades additional storage/write work for avoiding repeated read-time calculation. A VIRTUAL
  primary key and a generated expression referencing AUTO_INCREMENT remain OpenHalo extensions;
  MySQL 5.7 rejects those declarations. They are not part of the MySQL dump round-trip guarantee.
  Generated expressions and views using PostgreSQL-only syntax likewise need conversion before
  replay on MySQL.
- Generated expressions admit reviewed function implementations, not every MySQL function variant.
  DATE_FORMAT accepts typed temporal input; implicit text-to-date/time conversion is not yet
  admitted by the validator because the current PG-backed parser can read relative dates such as
  `now` from text. Use a DATE/DATETIME input column for these generated expressions. This boundary
  does not change ordinary SELECT function calls.
- Before 1.8, generated expressions could be discarded or stored as defaults. Before enforcement
  support, foreign keys could be NOT ENFORCED. Extension upgrade preserves these existing objects;
  it cannot recover discarded expressions or decide how to repair orphan rows. Run the explicit
  [object migration audit](MIGRATION.md#updating-an-existing-openhalo-18-installation) before cutover.
  Earlier generators could also wrap nondeterministic expressions as IMMUTABLE. Upgrading does not
  repair their stored values or rewrite their functions. LIKE and ALTER reject retained unsafe
  generators; dropping or replacing the affected generated column remains available for repair.
- Foreign keys intentionally require a referenced PRIMARY KEY or UNIQUE key, with PostgreSQL's
  type requirements. A non-unique parent index is rejected, including with FOREIGN_KEY_CHECKS=0;
  no approximate foreign-key implementation is provided. Pending FK metadata is kept in ordinary
  extension-managed triggers; do not strip these triggers from a native dump.
- A read-write REPEATABLE READ transaction does not keep one snapshot for its plain SELECTs (see above);
  set `READ ONLY` or `WITH CONSISTENT SNAPSHOT` where a stable view matters.
- MySQL GTID replication is unavailable: `gtid_mode` reports `OFF` and GTID sets are empty.
  PostgreSQL WAL positions are not MySQL GTIDs. Logical MySQL backups use ordinary data/schema dumps.

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
The 1.9 integration suites and isolated-instance requirements are listed in [TESTING.md](TESTING.md).

## License

Derived from openHalo: GNU GPL v3 (`../../LICENSE`), openHalo copyright notice in `COPYRIGHT`.
