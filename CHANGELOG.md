# Changelog

Release notes for OpenHalo 18 ([`pgsty/openHalo`](https://github.com/pgsty/openHalo)).

Versions are `<PostgreSQL version>.<OpenHalo revision>`: `v18.6.1` is the first OpenHalo
revision after `v18.6`, still on PostgreSQL 18.6. The kernel keeps reporting PostgreSQL's
own version (`PG_VERSION` `18.6`, `PG_VERSION_NUM` `180006`), so extension builds and
client tools see a regular PostgreSQL 18.6. The MySQL layer is versioned separately as
the `aux_mysql` extension.

| Release | PostgreSQL | `aux_mysql` | Date |
| :--- | :--- | :--- | :--- |
| [`v18.6.1`](#v1861) | 18.6 | 1.9 | 2026-11-10 |
| [`v18.6`](#v186) | 18.6 | 1.5 | 2026-11-07 |

---

## v18.6.1

PostgreSQL 18.6 with `aux_mysql` **1.9** (from 1.5). This release closes the correctness
gaps found after `v18.6`: statements that MySQL accepts but OpenHalo refused, and
statements that ran but stored or reported something else than MySQL 5.7 does.

### Upgrade notes

- **Rebuild the kernel and `aux_mysql` together.** Besides the protocol hooks of `v18.6`,
  the kernel now carries execution hooks for conflict handling, foreign keys, sequences
  and collations. Catalog layouts are still those of PostgreSQL 18, but protocol
  callbacks and parse/plan node structures differ from `v18.6` and from stock
  PostgreSQL 18. `v18.6` stated that stock PGDG extension binaries load unchanged; that
  no longer holds. Rebuild third-party C extensions against these headers and validate them.
- **Updating a `v18.6` installation**: install the new binaries and extension files,
  restart, then run `ALTER EXTENSION aux_mysql UPDATE TO '1.9'` over a PostgreSQL
  connection to `mysql.database` (path 1.5 → 1.6 → 1.7 → 1.8 → 1.9; versions 1.6–1.8 were
  never released). No user table is rewritten.
- **Existing objects keep their old definitions.** Foreign keys created by 1.5 stay
  `NOT ENFORCED`, old `BINARY`/`VARBINARY` columns stay length-less `bytea` domains, old
  prefix keys stay whole-column indexes, and generated columns whose expression 1.5
  discarded stay ordinary columns. Run the audit in
  [`MIGRATION.md`](contrib/aux_mysql/MIGRATION.md#updating-an-existing-openhalo-18-installation)
  and repair them from the original MySQL DDL before relying on the new semantics.
- **Behavior that changes for running applications**: foreign keys created from now on
  are enforced (use `FOREIGN_KEY_CHECKS=0` for loads, as in MySQL); a bare `DECIMAL` is
  `DECIMAL(10,0)`; an unqualified `FLOAT` is single precision; `ON DUPLICATE KEY UPDATE`
  and `REPLACE` report MySQL's affected-row counts (2 for an updated or replaced row);
  `0x41` is a binary string / number rather than a PostgreSQL bit string; TRUE/FALSE are
  stored as `1`/`0`; `#` and `/*! ... */` are a comment and executable SQL, as in MySQL.
- `aux_mysql` is now installed synchronously on the first MySQL connection of a
  superuser instead of by a background worker at startup. Ordinary users need it
  installed beforehand (`CREATE EXTENSION aux_mysql` in `mysql.database`).
- `pg_upgrade` explicitly rejects openHalo 1.0 (PostgreSQL 14) clusters, and a
  Meson build fails at configuration instead of producing an installation without
  `aux_mysql`. Build with GNU make.

### Kernel (PostgreSQL 18.6)

- Execution hooks used by `aux_mysql`: conflict arbitration over every unique index and
  delete-then-insert replacement in `ModifyTable`, foreign-key-only trigger control and
  pending foreign-key checks, atomic monotonic sequence advancement, index-constraint
  and collation method hooks. Native privilege and row-security checks are preserved,
  and stored `ON CONFLICT` nodes written by `v18.6` are still readable.

### Foreign keys

- Foreign keys created through MySQL are enforced, with their `ON DELETE` / `ON UPDATE`
  actions; in `v18.6` they were silently `NOT ENFORCED`
  ([#2](https://github.com/pgsty/openHalo/issues/2)).
- `FOREIGN_KEY_CHECKS=0` works as in InnoDB: checks and actions are skipped,
  re-enabling does not recheck rows, and a declaration whose parent table is missing or
  dropped is kept and bound when the parent appears.
- Violations report 1452 / 1451; `INSERT IGNORE` skips each invalid row with a warning.
- Binding a pending foreign key checks the child owner's `REFERENCES` privilege.

### Transactions and locks

- `SET [GLOBAL | SESSION] TRANSACTION`, `transaction_isolation`, `transaction_read_only`
  and their `tx_*` aliases take effect instead of only being echoed; read-only writes
  report 1792 and serialization failures 1213
  ([#3](https://github.com/pgsty/openHalo/issues/3), [#4](https://github.com/pgsty/openHalo/issues/4)).
- `SET GLOBAL read_only` / `super_read_only` stop writes (1290), per statement.
- `START TRANSACTION WITH CONSISTENT SNAPSHOT` is accepted and takes its snapshot at
  `START`, which `mysqldump --single-transaction` and parallel dumpers rely on.
- `GET_LOCK()`, `RELEASE_LOCK()`, `IS_FREE_LOCK()`, `IS_USED_LOCK()` and
  `RELEASE_ALL_LOCKS()` are real MySQL 5.7 user-level locks; before, they were stubs
  that always succeeded.

### DML

- `ON DUPLICATE KEY UPDATE` checks every unique key; `REPLACE` deletes each conflicting
  row and inserts the new one, so unlisted columns take their defaults and foreign-key
  actions and triggers run. Both insert plainly on a table without a unique key.
- Affected-row counts of `ON DUPLICATE KEY UPDATE` / `REPLACE` match MySQL.
- `UPDATE ... JOIN` no longer crashes the backend, and the comma form matches whole
  target names ([#1](https://github.com/pgsty/openHalo/issues/1)).
- SQL-level `PREPARE` / `EXECUTE ... USING` / `DEALLOCATE PREPARE` and `DO expr`.
- Index hints (`USE` / `IGNORE` / `FORCE INDEX`) are accepted and ignored.

### Types, keys and collations

- `BINARY(n)` / `VARBINARY(n)` keep their length (zero padding, byte limits), and binary
  strings keep every byte, including NUL and bytes that are not valid UTF-8, through
  literals, parameters, results and `pg_dump`.
- Hexadecimal and bit-value literals (`X'41'`, `0x41`, `b'101'`) are binary strings that
  are numbers in numeric context; `HEX()` / `UNHEX()`; `\%` and `\_` keep their backslash.
- `DECIMAL(p,s)` survives `SHOW COLUMNS`, `SHOW CREATE TABLE` and `information_schema`
  ([#5](https://github.com/pgsty/openHalo/issues/5)); `DATETIME(n)` keeps its precision.
- TRUE/FALSE and boolean results are stored as `1`/`0` in numeric, string and binary
  columns, including column defaults.
- `BIGINT UNSIGNED` writes above 2^63-1 fail with 1264 instead of being clipped.
- Prefix key parts `KEY (col(n))` enforce exactly the declared prefix and accept values
  longer than a btree entry, through the `mysql_prefix` index access method
  ([#7](https://github.com/pgsty/openHalo/issues/7)).
- Key names are per table, as in MySQL: tables can share key names, and `DROP INDEX` /
  `RENAME INDEX` find the key of their table.
- `utf8mb4_general_ci` / `utf8_general_ci` use MySQL 5.7's weights with `PAD SPACE`, and
  `utf8mb4_bin` / `utf8_bin` compare code points with `PAD SPACE`. Other collation names
  are accepted and mapped to one of the two with warning 1105
  ([#6](https://github.com/pgsty/openHalo/issues/6)).
- Generated columns (`AS (expr) [VIRTUAL | STORED]`) are computed; in `v18.6` they were
  ordinary columns holding NULL. Nondeterministic expressions are rejected with 3102.

### Dialect and protocol

- Executable comments `/*! ... */` and `/*!NNNNN ... */` run as SQL, so everything
  `mysqldump` wraps in them (`SET NAMES`, `FOREIGN_KEY_CHECKS`, view definitions) is no
  longer skipped on import.
- `#` starts a comment, and `--` only before a space or control character (`1--1` is 2).
- `SET NAMES ... COLLATE`, `SET CHARACTER SET` and `SET CHARSET` in all their spellings.
- `SHOW WARNINGS` / `SHOW ERRORS` return the session's diagnostics; `SHOW INDEX` reports
  key parts in key order, prefix lengths and `BTREE` / `HASH`; `SHOW CREATE TABLE`
  output replays.
- `COM_RESET_CONNECTION` resets the whole session state.
- New setting `mysql.max_allowed_packet` (default 64MB) limits an incoming MySQL message
  and accumulated prepared long data.

### Known limits

The supported surface and the accepted differences from MySQL 5.7 are listed under
[Known gaps](contrib/aux_mysql/README.md#known-gaps): no native `ENUM` / `SET`, no stored
procedures, no multi-target `UPDATE` / `DELETE`, `BIGINT UNSIGNED` limited to the signed
64-bit range, approximated language-specific collations, among others.

### Verification

[`contrib/aux_mysql/ROUTE_A_RECHECK.md`](contrib/aux_mysql/ROUTE_A_RECHECK.md) records the
release checks and their results; [`contrib/aux_mysql/TESTING.md`](contrib/aux_mysql/TESTING.md)
has the commands to repeat them, including the upgrade of a real `v18.6` data directory.

---

## v18.6

Initial OpenHalo 18 release: PostgreSQL 18.6 with generic protocol extension hooks and
`aux_mysql` 1.5, carrying openHalo's MySQL protocol, dialect and function library as an
extension. Synced with upstream `HaloTech-Co-Ltd/openHalo` up to `90fc8d7`
(see [`contrib/aux_mysql/UPSTREAM.md`](contrib/aux_mysql/UPSTREAM.md)).

The PostgreSQL 14 line of openHalo 1.0 remains on branch `pg14` (tag `v1.0-pg14`).
