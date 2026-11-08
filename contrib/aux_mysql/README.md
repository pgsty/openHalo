# aux_mysql

openHalo's MySQL Supplementary Extension (version 1.8, same name and settings as in openHalo 1.0) on
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
- **Binary strings**: BINARY / VARBINARY / BLOB columns store the bytes they are given, whatever they are,
  and return them unchanged in text and binary result sets: `_binary '...'`, `X'..'` / `0x..` (an odd
  number of digits gets a leading 0, as `0xabc` in MySQL), plain `'...'` / `"..."` literals with backslash
  escapes (`\0`, `\\`, `\Z`, ...), raw NULs and bytes that are not valid UTF-8 (such a literal becomes a
  binary string; a TEXT column rejects it with 1366, as strict MySQL does), and prepared-statement
  parameters of any string or BLOB type. So mysqldump output replays byte for byte, with or without
  `--hex-blob`. A NUL at the end of a query ends it, as in MySQL.
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
- **Prefix key parts** `KEY (col(n))`: a column that may hold values beyond a btree entry (TEXT, BLOB,
  VARCHAR over 600 characters) is not indexed whole, so long values insert as in MySQL. A key starting
  with one becomes a hash index on that column, which serves `=` lookups (also for TEXT / BLOB keys
  without a prefix, which MySQL refuses); a later part becomes `left(col, n)`, at most 600 characters
  in all. A UNIQUE key enforces such a prefix as MySQL does (also for ON DUPLICATE KEY and REPLACE),
  with a hash index for lookups when it is the first part. Other columns are indexed whole.
  `SHOW CREATE TABLE` / `SHOW INDEX` show `col(n)` and `Sub_part` for `left(col, n)` parts.
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
- **Clients tested**: Connector/J 8.4, go-sql-driver 1.9, PyMySQL, mysql / MariaDB CLI, mysqlslap, sysbench.

## Known gaps

- `SHOW STATUS`, some `SHOW VARIABLES LIKE` forms; `information_schema` mapping is partial (`TABLE_CONSTRAINTS` is
  empty, no `KEY_COLUMN_USAGE`; GUI tools and ORMs need more).
- A key name longer than 54 bytes (18 CJK characters) is shortened to that length (PostgreSQL index
  names hold 63 bytes, and the suffix takes 9); key names compare case-insensitively in ASCII only.
- Native ENUM/SET, negative and long-duration TIME, and direct PG14 physical upgrades remain unsupported;
  [migration checks and explicit conversion examples](MIGRATION.md) describe the supported path.
- Statement-level rollback uses a subtransaction per statement inside a transaction block: more than 64
  writing statements in one transaction overflow the PGPROC subxid cache; for bulk loads use autocommit or
  `mysql.statement_rollback = off`.
- `bigint unsigned` is an int8 domain (no values above 2^63-1); no zero dates; `'a' = 'A'` between two
  literals is case-sensitive (literals use the database collation); `1.5e0` is numeric, not DOUBLE.
- PostgreSQL reads a string literal for a binary column with its bytea input, which takes backslashes as
  escapes. aux_mysql passes the bytes of such literals in INSERT / REPLACE values, `UPDATE ... SET` and
  `ON DUPLICATE KEY UPDATE`, but not elsewhere: `WHERE b = 'a\\b'` (also a string-typed parameter, such as
  Go's `[]byte`) fails, and `'\\x41'` compares as `A`. Write `_binary '...'` or `X'..'` there. A literal
  that is not valid UTF-8 or holds a NUL is a binary string everywhere: `SELECT '\xff'` returns a BLOB
  column, and a TEXT column rejects it also in non-strict mode (MySQL truncates it with a warning).
  A NUL inside a comment is rejected.
- BINARY(n) and VARBINARY(n) are BLOB domains without the length (the grammar drops it): BINARY(n) does not
  pad with 0x00 (MySQL stores `'ab'` in BINARY(4) as `ab\0\0`), so a shorter value reads back, compares
  and is indexed unpadded; values of exactly n bytes (UUIDs, hashes) behave as in MySQL. A longer value is
  stored whole (strict MySQL: 1406; non-strict: truncated with a warning). Both would need a binary type
  that carries a length, in place of the bytea domains every binary code path knows. SHOW CREATE TABLE and
  SHOW COLUMNS print `binary` / `varbinary` without the length; aux_mysql reads both back, MySQL takes
  `binary` as BINARY(1) and refuses `varbinary`. They also print a BLOB / BINARY default as `'\x..'::bytea`,
  which the MySQL dialect reads back as the characters `x..`: a replayed default (mysqldump) is wrong.
- Hexadecimal literals: aux_mysql has no unsigned 64-bit integer, so a literal of 2^63 or more in numeric
  context, or such a result (`0x8000000000000000 + 0`, a literal into BIGINT UNSIGNED), is error 1264
  rather than MySQL's unsigned value (`~0x0f` is exact, as DECIMAL). `0x41 + '1'` reads the string literal
  as bytes (MySQL: as the number 1). A literal is taken by functions of BLOBs (SUBSTRING, LENGTH, MD5,
  REVERSE, ...), CONCAT, HEX, LOWER / UPPER and IFNULL, not by other functions: `REPLACE(0x41, ..)`,
  `LEFT`, `TRIM`, `LOCATE`, `CHAR_LENGTH`, `ASCII`, `IF(c, 0x41, 0x42)`, `SUM(0x10)`, `REPEAT('a', 0x3)`,
  `INTERVAL 0x10 DAY`, `LEAST(1, 0x42)` and MySQL's `||` (OR) fail; write `CAST(0x10 AS UNSIGNED)` or
  `CAST(0x41 AS CHAR)` there. An integer column compared with a literal is not looked up by index. BIT columns still return their bits as
  text (`01000001`, MySQL: the bytes) and have no arithmetic of their own (`b + 0` fails; `b | b'1'` works).
  User variables hold strings: `HEX(@n)` of a number shows its digits (`323535` for 255), and a string
  variable in numeric context (`@s + 0`) fails. A MySQL session opened before `ALTER EXTENSION aux_mysql
  UPDATE` to 1.8 (or with new binaries and an older aux_mysql) keeps openHalo's bit-string literals.
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
- `ON DUPLICATE KEY UPDATE` and `REPLACE` find duplicates in one unique key: the PRIMARY KEY or the
  first UNIQUE key whose columns the statement supplies, else the PRIMARY KEY or the first UNIQUE key
  (a table without one just inserts, as in MySQL, also when created earlier in the same packet). A
  duplicate in another unique key fails with 1062, a key repeated within one multi-row statement
  with 1105, where MySQL updates. Their affected rows (and `ROW_COUNT()`) are MySQL's: 2 per updated or
  replaced row, 0 per row left unchanged (1 for CLIENT_FOUND_ROWS clients), but a plain `UPDATE` counts
  the rows it matches (MySQL without CLIENT_FOUND_ROWS: only those it changes).
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
  and SHOW / information_schema report DECIMAL precision and scale, SHOW prefix key parts (1.7), and
  `GET_LOCK()` locks, SHOW / information_schema show key names, hexadecimal literals are binary strings
  and numbers, `HEX()` / `UNHEX()` exist (1.8: in 1.7, `GET_LOCK()`, `RELEASE_LOCK()`, `IS_FREE_LOCK()`
  and `IS_USED_LOCK()` were stubs that let every session "hold" every lock).
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
