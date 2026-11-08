# aux_mysql 1.9 Route A release recheck

Base: clean main `6821954a3f8387287e655d7e91036a3ea0d80c28`.
Branch: `fix/aux-mysql-1.9-route-a`. Nothing pushed.
Builds, clusters and raw logs: ohdev `/tmp/aux-route-a/`.
Debug: MySQL 3804 / PG 5804 (ports checked before initialization).
Reference: MySQL 5.7.32 on 3771. Baselines 3792/3793 remain intact.

## WP1 — native TEXT (`8cf384d`)

Removed the unpublished custom character type, its SQL objects, C paths and
fixtures. All TEXT declarations again use PG text; binary types retain arbitrary
bytes. Updated NUL boundaries and binary/character round-trip fixtures.
The MySQL CAST lowering now retains explicit CHAR(n) lengths (1292 on truncation),
and CONVERT USING accepts UTF-8 names. Other encoding names use the closest
Unicode representation with 1105; this is an approximation, not transcoding.
Native PG casts are unchanged. Character arithmetic emits 1292 on bad prefixes.

Validation:

- Debug configured with `--enable-debug --enable-cassert --enable-depend
  --with-openssl`, full kernel/extension built and installed successfully.
- `python3 tests/route_a.py --port 3804 --reference-port 3771 --wp text`:
  371 assertions passed (values, HEX, warnings, 1366 boundaries).
- `python3 tests/check_generated.py`: all five generated files byte-identical.
- `PG_CONFIG=/usr/pgsql-18/bin/pg_config BUILD_DIR=/tmp/aux-route-a/standalone-binary
  tests/binary_fix/run.sh` (OS user pg, original pg14 grammar supplied): passed,
  including private libpq binary I/O and pg_dump/restore.
- Baseline AB `p04.sql` / `p04b.sql`: restored native string results and mixed
  expressions. LOCATE/INSTR remains for WP4. Existing excluded function gaps
  remain; unordered GROUP BY output is compared as a set, not by incidental order.
- Removed-type source search: zero references. Generated output keeps original
  upstream whitespace on the four restored TEXT rules.

Actual metadata: TINYTEXT, TEXT, MEDIUMTEXT and LONGTEXT all display `text` in
SHOW CREATE TABLE and both DATA_TYPE and COLUMN_TYPE. Subtype fidelity is outside
this release repair, as requested.

Observed warning difference: on live 3771, `'Hello'+0` gives 1292 but table-column
`s+0` gives 0 without a warning. OpenHalo returns 0 with the requested 1292;
regression assertions explicitly distinguish this diagnostic difference.
CHAR/VARCHAR mixed IFNULL/IF overloads are ambiguous on both baseline 3793 and the
new build; the extra CHAR scan excludes those two pre-existing combinations.

Remaining packages and final debug/release acceptance are in progress. A package
being committed or built does not imply the final full suite has passed.

## WP2 — binary-to-character conversions (`894b949`)

Added assignment casts from both length-aware binary types to text/varchar/bpchar.
They validate the entire payload as UTF-8; NUL (including BINARY padding) and
invalid UTF-8 produce 1366 even under IGNORE. CHAR assignment into the binary
types uses its logical value. GROUP_CONCAT has byte-preserving binary-array
implementations, including the historical binary domains, and returns binary
metadata. Types remain category U, with implicit casts only to bytea; character
assignment casts cannot silently take over binary operator resolution.

The output-function audit fixed SQL EXECUTE expression parameters, binary-valued
system settings and partition-value formatting. Protocol result columns and lock
arguments already branch on binary base types. User-variable serialization pairs
byteaout with byteain and is lossless (SET/GET/EXECUTE byte identity tested).
The remaining system-variable catalog formatting reads declared scalar metadata;
INSERT ID capture formats the numeric identity result. Legacy CONCAT/ELT helpers
are bypassed by the MySQL grammar's typed lowering; native PG functions retain
their own semantics.

Debug rebuilt/installed successfully. `tests/route_a.py --port 3804 --wp binary`:
110 assertions against 3771 passed. WP1 text scan: 371 passed again. Stock-PG18
private binary suite, libpq and pg_dump/restore: passed again. Test schemas now
explicitly select utf8mb4, avoiding the reference server's latin1 database default.

## WP3 — CHAR logical values (`29a3d6d`)

Centralized extraction of CHAR bytes. Default mode trims trailing spaces;
PAD_CHAR_TO_FULL_LENGTH is already supported and retains all storage padding.
CONCAT/CONCAT_WS/ELT, binary and character CAST, HEX, GROUP_CONCAT, lock names,
user variables and SQL EXECUTE now follow this rule. Exact character overloads
prevent an implicit text cast from discarding PAD mode before function entry.

Debug rebuilt and installed. `tests/route_a.py --port 3804 --wp char`: 42
assertions against 3771 passed in default and PAD modes. The reported examples
produce `782C21` and `78`. WP2 binary (110) and WP1 text (371) assertions passed
again after the character overload changes.

## WP4 — searches under MySQL collations (`4419811`)

MySQL-session LOCATE/INSTR and grammar-lowered POSITION use length-aware search.
The dedicated general_ci collation uses the existing MySQL 5.7 weight table and
linear search; bin uses byte equality with character positions. Historical ICU
aliases use explicit-length collation comparison without the PG14 substring
restriction. REPLACE gets a case-sensitive character overload, matching MySQL
even under general_ci. Native PG substring and cast paths remain unchanged.

The error-string audit found the copied substring engine (now bypassed on these
MySQL entry points) and ILIKE's ICU restriction (ILIKE is a PG extension, not a
MySQL function). SUBSTRING_INDEX deliberately compares bytes, FIND_IN_SET uses
collation-aware array lookup, and ordinary LIKE supports nondeterministic
collations; the dedicated MySQL LIKE implementation uses simple weights.

Debug built and installed. `tests/route_a.py --port 3804 --wp collation`: 468
assertions, VARCHAR/CHAR/TEXT × utf8/utf8mb4 general_ci/bin/unicode_ci, plus all
three historical aliases. Values, HEX and warning codes compared to 3771;
unicode_ci's existing approximate mapping emits 1105 at DDL. Historical aliases
have no namesake in MySQL and are asserted against their documented behavior.

## WP5 — decimal interpretation of binary CAST sources

The MySQL CAST/CONVERT lowering now dispatches exact binary sources through a
length-aware decimal parser, retaining numeric hex literals and all nonbinary
cast resolution. Integer casts consume the signed integer prefix; DECIMAL also
accepts fractions/exponents and clips precision overflow with 1264. NUL and bad
UTF-8 are numeric terminators, not text-conversion errors. Native PG bytea casts
are untouched and still decode `3432` as the binary integer 13362.

Debug rebuilt. `tests/route_a.py --port 3804 --wp numeric_cast`: 372 assertions
passed, including strict/non-strict, BLOB/VARBINARY/BINARY/UNHEX, hex literals,
CONVERT, DECIMAL rounding/ranges and a native PG protocol check. All six groups
of `review_fix_generated_policy.py --ports 3804` passed.

Diagnostic difference required by the product rule: malformed binary DECIMAL
sources consistently get 1292. Reference 3771 emits no tail warning for BLOB or
UNHEX, 1292 for VARBINARY/BINARY, and 1366 (plus 1292 for VARBINARY/BINARY) when
there is no numeric prefix. Values agree; the assertions record these distinct
reference warning sequences. Full uint64 expression results remain out of scope.
