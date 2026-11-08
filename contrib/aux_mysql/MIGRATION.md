# OpenHalo 14 → 18 migration and supported boundaries

OpenHalo 18 uses PostgreSQL 18 catalogs and an extension for MySQL behavior. It is
not a binary-compatible continuation of the OpenHalo 14 fork. Do not point the
18 binaries at a 14 data directory or use `pg_upgrade --link`. `pg_upgrade` now
rejects the known `1.0.14.x` branded binaries explicitly. A version-string wrapper
would only bypass the first check; it would not make the catalogs compatible.

Use a separate 18 instance, inventory each database, review the schema conversion,
load data logically, compare counts/keys/value ranges, and test the application
before cutover. Keep the old instance and a tested backup until acceptance.

## Configuration preflight

Run the read-only checker against the old configuration (including includes):

```sh
python3 contrib/aux_mysql/tools/check_pg14_config.py /path/to/old/postgresql.conf
psql -X -d old_database -f contrib/aux_mysql/tools/check_pg14_schema.sql
```

The configuration checker exits 2 for settings needing review. It reports known
migration changes, not every possible PostgreSQL configuration incompatibility.
Do not copy the old configuration wholesale. Start with an 18 `initdb` configuration
and transfer reviewed settings.

| Old setting | OpenHalo 18 action |
|---|---|
| `database_compat_mode`, `standard_parserengine_auxiliary` | Remove. Native PG connections use PG SQL; MySQL connections use `mysql.parser`. |
| `mysql.ci_collation` | Remove; it was already removed upstream on the PG14 line. Use column/table collations and `mysql.default_collation`. |
| `mysql.max_allowed_packet` | Supported in bytes (1024..1073741823), reloadable; applies to incoming messages and accumulated prepared long data. `@@max_allowed_packet` reports this GUC. SET through MySQL is explicitly rejected. |
| `halo_mysql.*` research names | Use documented `mysql.*` names. No automatic prefix alias. |
| `shared_preload_libraries` | Include `aux_mysql` and restart. |
| listener/address/port settings | Restart; a successful reload does not change listening sockets. |

Automatic installation now happens synchronously on the first MySQL connection
by a superuser, under that authenticated role. PG-only startup creates no SQL
objects. For managed installations run `CREATE EXTENSION aux_mysql` explicitly in
`mysql.database` before admitting clients. Ordinary users cannot auto-install it.
Missing databases fail connection attempts; creating the database lets a later
connection retry. On a standby or during binary upgrade installation is disabled.
Set `mysql.auto_create_extension=off` and restart if dropping the extension should
remain effective. With it on, a later superuser MySQL connection reinstalls it.

## Schema and data boundaries

| Feature | Supported repair / migration action |
|---|---|
| AUTO_INCREMENT | Identity plus an explicit-value trigger. Atomic advancement requires INCREMENT 1 CACHE 1 NO CYCLE. Empty-table reset is serialized against writes. Partition-parent changes raise an error; complete MySQL partition support is not claimed. |
| UNSIGNED identity | Retains the original domain checks via a table CHECK; inspect and repair negative values loaded with the earlier 18 build before validating the new constraints. |
| Foreign-key parent keys | PRIMARY KEY or UNIQUE is required. Referencing a non-unique parent index is deliberately rejected even with FOREIGN_KEY_CHECKS=0. Review the relationship and existing duplicates before adding uniqueness; never add a unique constraint automatically. |
| NUL in CHAR/VARCHAR/TEXT families | Rejected with 1366. Retaining native PG character types is an intentional boundary. Use an explicit BLOB/BINARY/VARBINARY representation when the application needs NUL, and review length constraints, comparison behavior and indexes. Never remove NUL bytes during import. |
| VIRTUAL generated columns | Physically stored, evaluated under writer settings. Do not rely on a reader's sql_mode changing existing generated values. Account for storage and write work. Virtual primary keys and references to AUTO_INCREMENT are OpenHalo extensions whose DDL does not replay on MySQL 5.7. |
| Table collation default | New MySQL tables retain it in an `aux_mysql` object label. PG dump/restore carries it; do not use `--no-security-labels` when this metadata matters. Existing 18 tables without metadata fall back to `mysql.default_collation`: set a reviewed label explicitly with `SECURITY LABEL FOR aux_mysql ON TABLE schema.table IS 'case_sensitive'`. Do not infer it from an arbitrary column. |
| FLOAT | New unqualified FLOAT uses float4 and wire type FLOAT; DOUBLE/FLOAT(25..53) use float8. Existing float8 data is not automatically narrowed. Use DOUBLE explicitly to preserve the old storage precision. Deprecated FLOAT(M,D) does not implement decimal-place constraints. |
| DATETIME(n) | New declarations keep precision 0..6 on a native timestamp without time zone; DATETIME without `(n)` uses 0. Text and prepared results retain the stored precision. Earlier 18 declarations used an unconstrained datetime domain: review existing values before narrowing them. SHOW CREATE may normalize the spelling to TIMESTAMP(n). |
| ENUM / SET | Native MySQL ENUM/SET syntax and ordinal/bitmask semantics remain unsupported. Export definitions and choose an explicit PG enum, constrained text, or application representation. The inventory flags affected columns. Do not silently turn them into unconstrained text. |
| Negative / ≥24-hour TIME | Native MySQL TIME duration compatibility remains unsupported. Migrate explicitly to PG `interval` or integer microseconds; adapt application decoding and time functions. Nonnegative clock-time columns can keep TIME. |
| SHOW diagnostics | SHOW WARNINGS/ERRORS and LIMIT return the current session's diagnostics; at most 64 messages are retained. This is not a full MySQL performance-schema diagnostic history. |
| Build | GNU make (including VPATH) and PGXS. Meson fails early with a build-path instruction; a successful incomplete installation is no longer produced. |

The following is an explicit representation example, not an automatic or lossless
MySQL schema translator. It preserves the demonstrated values and enforces the
listed member constraints. ENUM sort ordinal, SET numeric coercion/canonical
ordering, duration functions and driver return types require application review.

```sql
-- Execute over the PostgreSQL connection in the target database.
CREATE SCHEMA migrated;
CREATE TABLE migrated.example (
    id integer PRIMARY KEY,
    state text CHECK (state IN ('new', 'done')),
    flags text CHECK (string_to_array(flags, ',') <@ ARRAY['red', 'blue']),
    elapsed interval CHECK (extract(year FROM elapsed) = 0
                            AND extract(month FROM elapsed) = 0)
);
INSERT INTO migrated.example VALUES (1, 'done', 'red,blue', interval '-01:02:03');
SELECT state, flags, extract(epoch FROM elapsed) FROM migrated.example;
-- done | red,blue | -3723
```

SQL dump files from OpenHalo14 may refer to fork-specific functions, casts and
catalogs. Restore the reviewed user schema and data into a freshly installed 18
extension; do not replay the old extension's internal SQL/catalog modifications.

## Updating an existing OpenHalo 18 installation

Version 1.9 supplies an extension update path from 1.6 through 1.7 and 1.8.
Install matching OpenHalo kernel and extension binaries, restart, and run this
through a PostgreSQL connection in the MySQL database:

```sql
ALTER EXTENSION aux_mysql UPDATE TO '1.9';
```

The protocol callback and node layouts changed: rebuild the kernel and extension
together, using `--enable-depend` or a clean build. Do not mix old kernel objects
with a newly linked extension. New binaries retain the old SQL type behavior
until the SQL upgrade; existing sessions refresh type availability after catalog
invalidation. This compatibility window does not install new SQL semantics by
itself. Do not DROP EXTENSION CASCADE on a populated database.

Catalog equality with a fresh install does not imply historical data or user
objects were repaired. Inventory each application schema before cutover:

```sh
psql -X -v review_schema=app -v check_fk=1 -f tools/review_fix_migration.sql
```

The default audit lists real enforcement/validation flags and repair statements.
`check_fk=1` also counts orphans and may scan large tables. After resolving those
rows using the application's data rules, enforce and validate in place:

```sh
psql -X -v review_schema=app -v enforce_fk=1 -v validate_fk=1 \
  -f tools/review_fix_migration.sql
```

These operations retain constraint identity and fail transactionally if orphans
remain. An old NOT ENFORCED foreign key is never reported as repaired merely
because its catalog definition exists.

Recover old generated expressions and BINARY/VARBINARY lengths from an original
MySQL schema dump or migration source. NULL values and old defaults cannot reveal
a discarded generation expression. Supply generated definitions in a trusted SQL
manifest, for example:

```sql
INSERT INTO pg_temp.review_fix_generated_source
 (table_schema,table_name,column_name,type_sql,expression_sql,kind,attributes_sql,provenance)
VALUES ('app','t','g','varchar(2)','v','STORED','','original MySQL schema dump');
```

Pass it with `-v generated_manifest=/absolute/path/manifest.sql`. Review the
emitted MODIFY statements and execute them through the MySQL connection in
column-dependency order, with an appropriate sql_mode. Existing generator bodies
also need reviewed MODIFY statements to adopt new result coercion semantics.

Review the generator function definitions printed by the audit as well as the
column expressions. Earlier releases could hide RAND, current time, session
variables or external reads inside an IMMUTABLE wrapper. New CREATE/ALTER/LIKE
operations reject these expressions; upgrading alone does not repair existing
rows or remove old wrappers. Replace or drop the affected generated column using
an authoritative deterministic expression, then verify the recomputed values.
An expression already folded or discarded cannot be reconstructed from stored
values. Legal compatibility functions and writer sql_mode conversions remain
supported; arbitrary user-defined functions are outside this compatibility path.

Old binary domains and native PG TEXT columns retain their old representation.
Use explicit type migration based on original DDL, then verify byte lengths,
padding, generated values and unique constraints. The upgrade does not silently
rewrite them. Old ICU collations and whole-column/MD5 prefix indexes likewise
retain their original definitions; create replacement tables/indexes from the
original MySQL DDL to obtain correct prefix and general_ci/PAD SPACE semantics.
Resolve newly discovered duplicate keys before replacing the old objects.

Keep both native and MySQL logical backups and exercise restore before cutover.
Native dumps must retain extension-managed triggers and aux_mysql security labels.
Compare row counts, HEX values, defaults, generated expressions, FK actions and
unique conflicts after restore. Legacy index names without the per-table suffix
remain recognized and do not need a naming-only rebuild.
