# aux_mysql 1.9 Route A release recheck

Latest follow-up: [character boolean defaults](#character-boolean-defaults--2026-11-10).
The initial run below is historical and was superseded by the independent NO-GO review;
its base, branch and port lines describe that run only.

Base: clean main `6821954a3f8387287e655d7e91036a3ea0d80c28`.
Branch: `fix/aux-mysql-1.9-route-a`. Nothing pushed.
Builds, clusters and raw logs: ohdev `/tmp/aux-route-a/`.
Debug: MySQL 3804 / PG 5804 (ports checked before initialization).
Release: MySQL 3805 / PG 5805. Both instances remain available for inspection.
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

Final debug/release acceptance is recorded below, separately from the initial
per-package checks.

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

## WP5 — decimal interpretation of binary CAST sources (`3f26bed`)

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

## WP6 — prevent loss of legal uint64 values (`bc1d29c`)

The lenient integer assignment helper distinguishes the representable int8 bound
from MySQL's uint64 bound. A value in the legal uint64 high half raises 1264,
including IGNORE; it cannot collapse distinct keys onto INT64_MAX. Strict and
expression/INSERT SELECT paths already raise 1264 through their int8 conversion.
Binary parameters are expanded before this same rewrite and conversion path.
The generator also accepts IGNORE combined with ODKU (previously 1064), for all
four VALUES/SET and optional-INTO forms, retaining both behaviors.

Debug rebuilt; `tests/route_a.py --port 3804 --wp uint64`: 115 assertions passed
against 3771. INSERT, IGNORE, INSERT SELECT, expression/string inputs, UPDATE,
REPLACE, ODKU and unsigned binary prepared parameters are covered in three modes;
every representation error leaves existing rows unchanged. Generator byte check
passed and bison's expected conflict count did not increase.

The above original run accepted INT64_MAX clipping for inputs above UINT64_MAX.
The independent final review rejected that expectation: it caused ODKU to update
an unrelated row and REPLACE to delete it. The release correction rejects every
positive BIGINT UNSIGNED write above INT64_MAX with 1264, including IGNORE.
Non-strict negative inputs still become 0 with warning 1264.

## WP7 — NUL-safe character prefix clipping (`9ef2a83`)

Both bin and historical collation paths now clip by explicit character count;
NUL consumes one character and cannot terminate the key. A private standalone
test bridge bypasses text input validation to exercise this defense; production
character writers still reject NUL with 1366. The new regression failed before
the change with an erroneous 23505 between `a\0A` and `a\0B`, then passed after
it, including multibyte prefixes, C/bin/general_ci and REINDEX. The test-only
bridge is never installed in the product. Fixed Python 3.9 compatibility in the
earlier custom-binary fixture while running the complete prefix suite.

Debug rebuilt. Stock-PG18 `tests/indexes_standalone.py` passed its complete suite,
including concurrent keys and pg_dump/restore; `tests/indexes_btree.py` passed
on 3804/5804. `tests/route_a.py --port 3804 --wp prefix_nul`: 19 assertions
against 3771 passed, covering the character 1366 boundary and byte-preserving
binary ODKU/REPLACE with distinct NUL-containing prefix keys.

## WP8 — real released 1.5 upgrade (`0dc12fe`)

Built unmodified tag v18.6 (`5086d18f03af2794b69c87c784768a7aeb5d43e9`)
outside the repository. The repeatable test initializes with its binaries and
1.5 SQL, creates all representative type families, keys, checked/unchecked FKs,
an orphan, a discarded generator, views, a procedure and a trigger through
MySQL protocol, stops it, and starts that same data directory with the new debug
build. BEGIN ATOMIC procedures and a native DO block for the trigger function
use syntax accepted by 1.5; MySQL-style routine/trigger bodies remain a historical
capability gap. The old auto-install worker is disabled to avoid its startup race.

Fixed the missing column metadata helpers, preserved the dependent EXTRA view
descriptor, added the missing diagnostics function and refreshed routines view
metadata. All changes are in generators/upgrade sources, with generated files
regenerated. No user data or old index definitions are changed. Updated migration
checks/repair instructions for lost binary/prefix lengths, generated expressions
and unchecked FK orphans. A leftover AB expectation for TEXT NUL now asserts the
requested 1366; binary byte/NUL tests are unchanged.

Debug validation:

- `tests/release_real_upgrade.py --old-prefix /tmp/aux-route-a/v18.6
  --new-prefix /tmp/aux-route-a/debug --work-dir /tmp/aux-route-a/real-upgrade-debug6
  --mysql-port 3807 --pg-port 5807`: passed. Existing/new connections, unchanged
  legacy snapshots, procedure/trigger and dependent view all verified; AB 46/46
  and all seven Route A suites (1497 assertions) passed after upgrade.
- Real 1.5→1.9 versus fresh 1.9: zero differences across 3372 extension members,
  1761 functions, 20 views/272 descriptors, 82 casts, 665 operators, 27 opclasses.
  Catalog JSON/digests are retained in the test work directory.
- `PGHOST=/tmp PGPORT=5804 PGUSER=postgres tests/release_upgrade.py`: historical
  1.6→1.7→1.8→1.9 passed, including legacy stored ON CONFLICT, old binary/index
  objects and orphan-safe enforcement. Sole retained catalog difference:
  `mys_informa_schema.columns.extra` keeps pg_catalog.default rather than fresh
  pg_catalog.C; the view definition and descriptor record that same difference.
  It is a fixed-label metadata column; preserving it retains dependent views.
- `tests/release_online_upgrade.py --allow-listener-reload` on 3804/5804 passed
  the separate same-session 1.8→1.9 refresh test.

The final section records the successful release repetition and full acceptance.

## WP9 — ignored index hints (`311ef83`)

The generator accepts USE/IGNORE/FORCE, INDEX/KEY, optional FOR JOIN/ORDER BY/
GROUP BY, multiple/quoted/PRIMARY names, repeated hints, aliases and empty USE
lists. All hints are discarded. UPDATE and DELETE share the alias/hint boundary.
The existing MySQL `DELETE FROM target USING table_references` form also needs a
distinct PG target alias when the USING list includes the target; the generator
now correlates it by ctid and tableoid, retaining all join conditions. This
prevents an otherwise valid hinted multi-table DELETE from failing with 1105.
The generated alias avoids existing aliases. Native PG grammar is untouched.

Debug rebuilt; bison still accepts exactly 47 expected conflicts. The Route A
`--wp hints` scan covers each hint spelling/scope/name list, SELECT aliases,
both JOIN operands, comma/JOIN UPDATE and USING DELETE (aliases, left/right join
operands and alias collision), plus INSERT IGNORE and USE database regression.
All 315 assertions passed against 3771. `release_dml.py` passed its SQL,
concurrency, trigger and privilege cases, including reference-port repetition.
Generator byte identity passed.

## WP10 — pending FK owner permissions (`2a7f1e7`)

Before privileged binding, check the child owner's schema USAGE and table-level
or every-column REFERENCES, including implicit composite primary keys. Missing
authority leaves the declaration pending and emits WARNING/1142; parent CREATE
and ALTER succeed. Authorized bindings retain the parent lock through ALTER.
Restore validates metadata first and applies the same owner checks, rather than
using the restoring account's privileges or requiring a table-level grant.

Debug rebuilt. `tests/route_a_fk_privileges.py` passed 46 assertions with two
non-superuser roles: revoked USAGE, absent/partial REFERENCES, successful column
grants, implicit keys, parent deletion/recreation, same-owner behavior and
privileged pg_dump/restore with/without permission. Orphan rows and binary HEX
remain unchanged; an unbound key cannot probe existing parent keys or prevent
parent deletion. `review_fix_pending_fk.py --ports 3804 3771` passed all five
groups on both servers. The real-upgrade runner now includes the ACL suite.
This PG security boundary intentionally differs from MySQL 5.7's REFERENCES
permission policy. Release repetition and final full validation follow below.

## Historical acceptance run — 2026-11-09 (superseded by final review)

All ten implementation packages are committed separately. Debug and release
kernel/extension builds are installed outside the repository. Both use OpenSSL
and `--enable-depend`; debug additionally uses `--enable-debug --enable-cassert`.
No core kernel source or native PG cast was changed. Main remains at the starting
commit; nothing was pushed. A final validation commit records the wider
collation scan, removes the standalone harness's obsolete REPLACE stub (now
provided by production SQL), and fixes MySQL boolean conversion through the new
generic byte path to use `1`/`0` rather than PG boolout's `t`/`f`. Twelve added
value/HEX/warning assertions cover CAST CHAR, ELT, CONVERT USING and CONCAT_WS
for true, false and NULL. No expected value file was changed to accept an
incorrect result.

The common environment below selects either isolated instance. Commands run from
`/w/openhalo/contrib/aux_mysql` inside ohdev. Credential values are the existing
test fixtures documented by the test scripts.

```sh
export PGBIN=/tmp/aux-route-a/debug/bin  # release: /tmp/aux-route-a/release/bin
export PGHOST=/tmp PGPORT=5804 PGUSER=postgres PGDATABASE=postgres
export MYSQL_PORT=3804 MYSQL_USER=dbuser_mysql MYSQL_REFERENCE_PORT=3771
export PGPASSWORD=Mysql.123 MYSQL_PASSWORD=Mysql.123 MYSQL_PWD=Mysql.123
export PSQL="$PGBIN/psql"
export MYSQLDUMP=/root/openhalo-release-fixes/mysql57-client/mysqldump-run
export MYSQL_CLIENT=/root/openhalo-release-fixes/mysql57-client/mysql-run
# Release uses PGPORT=5805 and MYSQL_PORT=3805.
```

| WP | Commit | Assertion command / final result on both builds |
| --- | --- | --- |
| 1 TEXT | `8cf384d` | `python3 tests/route_a.py --port "$MYSQL_PORT" --wp text`: 371 |
| 2 binary conversions | `894b949` | Same runner, `--wp binary`: 122 including the final boolean conversion checks |
| 3 CHAR padding | `29a3d6d` | `--wp char`: 42, default and PAD mode |
| 4 substring collations | `4419811` | `--wp collation`: 468; `--wp collation_all`: 9945 contract checks, with the explicit approximations below |
| 5 numeric binary CAST | `3f26bed` | `--wp numeric_cast`: 372, including unchanged native PG bytea→int8 |
| 6 legal uint64 writes | `bc1d29c` | `--wp uint64`: 115, including raw unsigned prepared parameters |
| 7 prefix NUL defense | `9ef2a83` | `--wp prefix_nul`: 19; full standalone prefix suite and integrated index suites |
| 8 actual 1.5 upgrade | `0dc12fe` | `release_real_upgrade.py`, `release_upgrade.py`, `release_online_upgrade.py`: passed |
| 9 ignored hints | `311ef83` | `--wp hints`: 315; bison remains at 47 expected conflicts |
| 10 FK owner ACLs | `2a7f1e7` | `python3 tests/route_a_fk_privileges.py`: 46, including two roles and dump/restore |

The eight Route A selections total 1824 assertions; FK ACLs add 46.
These pass on fresh debug/release 1.9 and after each real 1.5 upgrade. The complete
221-name collation scan was also run on fresh debug and release (9945 checks
each). It checks every function result against the declared general_ci/bin
mapping and separately reports any difference from the requested MySQL locale;
its approximation results are not represented as exact MySQL agreement.

Final validation results:

- Full `make -j12 && make install`: debug and release passed.
- `PGHOST=/tmp PGPORT=5804 PGUSER=postgres make -C
  /tmp/aux-route-a/build-debug/src/test/regress installcheck`, run as OS user pg:
  **231/231**; release at 5805: **231/231**. The first debug run as root hit
  COPY/largeobject fixture filesystem permissions in five groups; after making
  the external build trees writable by pg, the complete rerun passed.
- `bash tests/run.sh`: **26/26**, zero skips on each build, including 46 AB cases,
  raw prepared packets, TLS/password rejection, mysqlslap, Go and Connector/J.
- All **22** integration invocations in each `logs/acceptance-{debug,release}/`
  passed: release_dml, binary mysql_check, sql_generated, generated_export,
  generated_policy, pending_fk, FK ACLs, collation_mapping, indexes_mysql,
  indexes_btree, release_txn, eight Route A selections, release_upgrade,
  release_roundtrip, and release_online_upgrade. Exact flags are in TESTING.md;
  `status.json` retains every command and exit code.
- `release_roundtrip.py`: native pg_dump/pg_restore, OpenHalo's own Oracle
  MySQL 5.7.32 mysqldump/mysql replay, and replay of the same unmodified dump into
  3771 all passed on both builds, including binary NUL, generated columns,
  prefix keys, pending FKs, existing orphans and cascades.
- `release_real_upgrade.py --old-prefix /tmp/aux-route-a/v18.6 --new-prefix
  /tmp/aux-route-a/debug --work-dir /tmp/aux-route-a/real-upgrade-debug-accepted
  --mysql-port 3807 --pg-port 5807 --reference-port 3771`: passed; repeated with
  release and `real-upgrade-release-accepted`: passed. Each work directory retains
  old DDL/data snapshots, full catalogs, AB and Route A/ACL results; its server
  is stopped on exit.
- Real 1.5→1.9 versus fresh 1.9: **zero catalog differences** in 3372 members,
  1761 functions, 20 views/272 descriptors, 82 casts, 665 operators and 27
  opclasses (including support members). Both builds yield normalized digest
  `345b2d341c9310f59effd5f2164b2e6d4f5a87046c1607e87e12042a6ec2036d`.
- Historical 1.6→1.7→1.8→1.9: only `mys_informa_schema.columns.EXTRA` retains
  pg_catalog.default instead of fresh pg_catalog.C. The view definition and its
  column descriptor both reflect that one preserved property. All other
  compared definitions match. The released 1.5 catalog already used C.
- `python3 tests/check_generated.py`: all five outputs byte-identical to fresh
  pg14 regeneration. Removed-type search returns zero references throughout the
  repository. Native PG `decode('3432','hex')::int8` remains 13362.
- Stock PG18 standalone binary harness, including libpq binary I/O and native
  dump/restore: passed after removing the duplicate test-only REPLACE stub.
  Full standalone prefix harness passed in WP7, including the previously failing
  NUL-prefix reproducer, multibyte clipping, concurrency and dump/restore.
- `/root/claude-ab.py /root/claude-ab-cases/{p01,p02,p03,p04,p04b,p04c,p05,nul}.sql`
  was run sequentially against each build. The observed differences are listed
  below. Debug/release results are identical except the excluded current-time
  values. Both baselines 3792/3793 were left running and their files untouched.

Raw evidence is under ohdev `/tmp/aux-route-a/logs/`, with build trees, data
directories, real-upgrade snapshots and standalone outputs elsewhere under the
same external root. No binaries, build trees or raw logs are committed.

## Remaining differences and exact limits of verification

1. **Character NUL and invalid UTF8:** 1366 even under IGNORE; BINARY padding is
   also NUL. The resulting missing rows in p01/p02/nul are consequences of this
   explicit product boundary. Binary payloads retain every byte.
2. **TEXT subtype names:** TINYTEXT/TEXT/MEDIUMTEXT/LONGTEXT all show `text` in
   SHOW CREATE TABLE and information_schema DATA_TYPE/COLUMN_TYPE.
3. **uint64 representation:** legitimate high-half writes fail 1264 instead of
   corrupting keys. Above-UINT64_MAX inputs now also fail 1264: MySQL clips to
   UINT64_MAX, which is still unrepresentable. The earlier clipping expectation
   was a release blocker, not an accepted approximation. Full
   unsigned expressions, negative-to-UNSIGNED and unsigned overflow arithmetic
   remain outside scope. This is not full uint64 storage compatibility.
4. **Numeric diagnostics:** malformed binary DECIMAL follows the requested 1292
   policy; reference BLOB/UNHEX can omit truncation warnings or use 1366, while
   VARBINARY/BINARY can add 1292. Table TEXT arithmetic `s+0` emits 1292 here but
   3771 omits it in the observed column case. Values match. Some existing
   arithmetic wire metadata remains int/Decimal rather than MySQL DOUBLE.
5. **Collation approximation:** the four exact utf8/utf8mb4 general_ci/bin names
   match the tested values/HEX/warnings. Other names warn 1105 and use the
   documented closest mapping. All 221 character collation names × three types
   executed without substring-search errors. Across the exhaustive ASCII probe,
   246 results differ from the requested locale, with these complete causes:

   | Requested names | Expression and input | OpenHalo / MySQL | Cases |
   | --- | --- | --- | --- |
   | `latin2_czech_cs` | `LOCATE('L',s)`, Hello/xHello/hello | 0,0,0 / 3,4,3; existing non-ci mapping uses bin | 3 column types |
   | All 27 `ucs2_*`, 26 `utf16_*`, 2 `utf16le_*` names on 3771 | `LOCATE('',s,2)` | 2 / 3; PG retains UTF8 rather than these two-byte encodings | 165 |
   | All 26 `utf32_*` names on 3771 | `LOCATE('',s,2)` | 2 / 5; PG retains UTF8 rather than UTF32 | 78 |

   The matrix emits every differing name/type/expression/value/HEX/warning in
   `logs/collation-all-{debug,release}.log`. Reference DDL declares each native
   character set; OpenHalo DDL supplies the same collation name over UTF8.
   String HEX comparisons normalize to UTF8. Binary-character-set columns are
   binary types and use the separate binary suite. This does not claim full
   language-specific weights or non-UTF8 storage/transcoding support.
6. **Existing generated-expression policy:** p05's CAST(CONCAT(...) AS DATE)
   generator still fails 3102 (`cast_text_to_date`); a disposable temporary-table
   probe confirms the same failure on baseline 3793. YEAR/MONTH/DATE_ADD and
   CAST AS SIGNED over-restriction, and SHOW output's casts/COLLATE placement,
   were left unchanged. PG special date words remain accepted intentionally.
7. **Existing function gaps:** JSON, SHA1, CRC32, TO_BASE64, QUOTE, SOUNDEX, ORD,
   CHAR() and SHOW DATABASES LIKE were not implemented. Mixed CHAR/VARCHAR
   IFNULL/IF was corrected in the release follow-up; broader integer/string and
   binary/character combinations remain deferred. Native MySQL routine/trigger body parsing is not
   added; the real 1.5 fixture uses its accepted routine syntax over MySQL wire.
8. **Other observed limits:** explicit `CHARACTER SET ascii` still has the
   baseline 1064 keyword-parser gap. MySQL's sql_mode deprecation warnings
   3090/3135 and exact diagnostic prose are not reproduced.
   p04's GROUP BY row order differs without ORDER BY; its row values are equal.
9. **FK authorization:** PG owner USAGE/REFERENCES checks are intentional even
   though MySQL 5.7 does not perform the same permission check. Denied binding
   warns 1142 and preserves both the parent DDL and pending declaration.

All requested build/test workflows have been executed. Not validated: real
customer data/workloads, other hardware/third-party kernel ABIs, excluded missing
features, and the deferred long-prefix performance/DDL-serialization questions.

## Final review corrections — 2026-11-09

The independent review of `f0dc47625bb541d44f258766bb9c2aec0c516341` was
NO-GO. This follow-up is an **uncommitted patch on that commit**, not a release
or a claim that the original commit passed. Evidence and builds are isolated
under host/ohdev `/tmp/openhalo-aux19-fixes-20261109/`; the prior evidence and
3792/3793, 3804/3805 instances were retained.

Implemented the user's release disposition:

- F1: every positive BIGINT UNSIGNED write above INT64_MAX fails 1264, including
  above-UINT64_MAX and IGNORE. Negative non-strict clipping to zero remains.
  Regressions seed the old INT64_MAX key, check row IDs/HEX, and verify statement
  rollback, audit triggers and subsequent writes inside an explicit transaction.
- F2: FIND_IN_SET requires collation equality and equal trailing-space counts
  for each item, in both text and bigint overloads. FIELD/STRCMP/LIKE/IN/DISTINCT
  and ordinary column/index equality are unchanged. Tests cover both trailing
  directions, repeated items, empty/NULL, accents and multibyte input.
- F3/F6: boolean CONCAT/GROUP_CONCAT use 1/0; boolean IF and CHAR/VARCHAR IFNULL/IF
  are supported, including unknown literals, NULL and CHAR PAD mode. TEXT pairs
  avoid introducing ambiguity for `IFNULL(varchar_column, '')`. Reviewed exact
  SQL bodies were added to the generated-column policy; its recursive checks
  still reject volatile inputs. The generic GROUP_CONCAT array identity is
  retained to avoid making integer arrays ambiguous with a bool[] overload.
- F10: grammar generator supports quoted/unquoted SET NAMES with COLLATE,
  SET NAMES DEFAULT, SET CHARACTER SET and SET CHARSET. Session charset variables
  are updated and non-UTF8/locale approximations warn 1105. Connection literal
  collation propagation remains deferred; this does not implement transcoding.
- F9: text and prepared character field metadata uses 45; binary fields stay 63.
- Build: added missing declarations/includes and replaced invalid pointer/Datum
  conversions with typed `text_to_cstring` calls. Actual EL10 GCC 14.3.1 build
  and install passed; implicit-declaration/int-conversion warnings are gone.
  Existing unused/shadow/missing-variable-declaration warnings remain recorded.

`sql/aux_mysql_fix_compat.sql` is appended to both fresh 1.9 installation and
1.8→1.9 upgrade, so the released 1.5 and historical chains get identical objects.

| Final verification | Debug | Release |
|---|---|---|
| PostgreSQL core regression | 231/231 | 231/231 |
| tests/run.sh | 26/26, zero SKIP | 26/26, zero SKIP |
| TESTING.md integration / Route A / upgrade / recovery invocations | 25/25 | 25/25 |
| release_fixes.py | 294 checks | 294 checks |
| Route A text / uint64 | 375 / 167 | 375 / 167 |
| Actual released v18.6 binaries + 1.5 SQL → 1.9 | passed | passed |
| Fresh versus released upgrade catalog | no raw differences | no raw differences |

Both released upgrade catalogs have 3464 members and 1853 functions; the
comparison retained complete definitions, not just counts. The historical
1.6→1.7→1.8→1.9 chain retains only its previously explained EXTRA collation
difference. Stock PG18 standalone binary and prefix suites passed. All five
generator outputs remain byte-identical to regeneration; bison retains 47
expected conflicts. Final builds used `--with-openssl --enable-depend`, with
`--enable-debug --enable-cassert` additionally for debug. Main ports were
3904/5904 and 3905/5905; real upgrade ports 3907/5907 and 3908/5908.

Connector/J **5.1.49** passed UTF8 text and byte-preserving ordinary/prepared
reads with `connectionCollation=utf8mb4_general_ci`, using TLS and
`mysql_clear_password` on the private 3906/5906 fixture. An exploratory 5.1.40
run failed TLS negotiation on the installed JDK before field decoding; it is
not evidence of a charset-255 failure or of 5.1.40 support. No claim is made for
all old drivers or Clang builds.

Failures during development were retained: initial missing postgres fixture
password; new mixed-overload ambiguity and generated-body-policy rejection;
integer GROUP_CONCAT ambiguity from a bool[] overload; root invocation of stock
PG initdb; missing EL10 Perl FindBin dependency. The fixture/dependency errors
were corrected and the product regressions fixed before the final passes.
A probe also confirmed the pre-existing multi-argument GROUP_CONCAT syntax gap;
that separate extension is recorded in README, with explicit per-row CONCAT
as a supported alternative.

Per user disposition, F7's other single-target DELETE spellings and true
multi-target DML, literal/variable collation propagation, and broader mixed
conditional types are deferred. F4's PAD search differences, F5's extreme
numeric rejection and F8's empty-needle multibyte offset are accepted 1.9
limits, not exact compatibility. No expected files were changed, and there was
no commit, push, tag or publication. The original f0dc476 NO-GO report remains
valid for that original binary.

## Post-commit corrections to `aa25342` — 2026-11-09

The post-commit review of `aa25342b14832838865dfa52456bd9f2db10d5ea` was NO-GO
for two wrong-data paths and a general_ci sort regression against v18.6. This
patch on `aa25342` changes only `contrib/aux_mysql`; the kernel is unchanged.

- A: in `sql_mode=''` a TRUE literal reached the lenient conversion as the text
  `true`, so INT/BOOL/BIGINT/DECIMAL/DOUBLE stored 0 with 1265 and ODKU/REPLACE
  acted on an unrelated key-0 row. The literal assignment boundary
  (`column_value`) now uses the integer 1/0 for numeric and string targets in
  every mode, including strict UPDATE. Other targets (BIT, JSON, ...) are unchanged.
- B: PG's assignment cast `text(bool)` stored `true`/`false` (or `fals`, or 1406)
  for predicates, IF and TRUE in CHAR/VARCHAR/TEXT, in both modes, again moving
  ODKU/REPLACE to a wrong row. After parse analysis of MySQL statements,
  INSERT/UPDATE targets, ON DUPLICATE KEY UPDATE and multi-row VALUES replace
  that implicit cast with `int4(bool)` through I/O. Explicit casts and native PG
  sessions keep `true::text = 'true'`. String generated columns over a boolean
  expression convert through `::int4` inside the generator; SHOW CREATE output is
  unchanged. No SQL objects or upgrade steps were added.
- Sort: `general_strncoll` built and freed two full weight keys per comparison,
  and both MySQL collations disabled abbreviated keys. Comparison now streams
  weights (128-entry ASCII table from the same weight data) and stops at the first
  difference, keeping PAD SPACE below-space order. Both collations provide
  `strnxfrm_prefix`, padded with the space weight, so memcmp order equals the
  comparator; `strxfrm_is_safe` only enables these abbreviated sort keys, while
  `strnxfrm` remains the hashing equality key. Stored keys and indexes do not change.

| Verification | Debug | Release |
|---|---|---|
| PostgreSQL core regression | 231/231 | 231/231 |
| tests/run.sh | 26/26 | 26/26 |
| TESTING.md integration / Route A / upgrade / recovery invocations | 23/23 | 23/23 |
| release_fixes.py (adds boolean_assignments, collation_order) | 414 checks | 414 checks |
| Actual released v18.6 binaries + 1.5 SQL → 1.9 | passed | passed |
| 512 boolean write cases vs MySQL 5.7 (also on the upgraded release copy) | 0 value/warning differences | 0 value/warning differences |
| 120k adversarial strings vs MySQL 5.7: ORDER BY/GROUP BY/DISTINCT/join | identical | identical |
| amcheck `bt_index_parent_check` after CREATE INDEX | passed | passed |

The new `release_fixes.py` cases fail on `aa25342` (1406 on the first boolean
insert). Stock PG18 standalone prefix and binary suites passed. The remaining
boolean differences are the 120 pre-existing 1105 rejections of boolean
expressions assigned to DECIMAL, DOUBLE and VARBINARY; no case stores a different
value. Literal/variable collation propagation remains deferred.

Release build, 300k rows, `work_mem=64MB`, no JIT or parallel workers, median of
five EXPLAIN ANALYZE runs (FIND_IN_SET: client time); ordered ids are identical:

| Workload | This patch | v18.6 (1.5, ICU) | `aa25342` |
|---|---:|---:|---:|
| ORDER BY 32-char general_ci | 60 ms | 92 ms | 3845 ms |
| ORDER BY 8-char general_ci | 57 ms | 92 ms | 1070 ms |
| ORDER BY 32-char utf8mb4_bin | 54 ms | 52 ms | 133 ms |
| ORDER BY shared-prefix keys | 174 ms | 199 ms | 1599 ms |
| COUNT(DISTINCT) | 55 ms | 90 ms | 3949 ms |
| equality scan | 12 ms | 16 ms | 188 ms |
| FIND_IN_SET aggregate | 0.62 s | 2.84 s | 15.3 s |
| CREATE INDEX | 90 ms | 101 ms | 2495 ms |

Evidence, builds and instances are under ohdev
`/tmp/claude-aux19-boolsort-20261109/`. Earlier review instances were not used.
Initial suite runs failed on two fixture omissions (postgres MySQL password,
non-writable regress directory); both were corrected before the passes above.

## Boolean assignment casts — 2026-11-09

The 1105 rejections left by `6f9c4f3` were fixed in 1.9 at the user's request.
A probe of every MySQL 5.7 column type against 3771 (45 types × 8 boolean
expressions × 7 write forms × 2 sql_modes; ENUM/SET are unsupported in
OpenHalo, leaving 4816 cases) found 2128 rejections on `6f9c4f3`, not 120:
DECIMAL/NUMERIC, FLOAT/REAL/DOUBLE (and UNSIGNED), BINARY/VARBINARY/BLOB family,
DATE/DATETIME/TIMESTAMP/TIME, YEAR and JSON. Integer expressions such as
`(SELECT 1)` were accepted by the numeric types but rejected by the binary,
temporal and YEAR types in the same way as booleans. v18.6 rejects the same
boolean writes.

PG has no boolean cast to numeric, floating point or binary strings. The patch
adds six assignment casts from PG `boolean` (numeric, float4, float8, bytea,
`mysql.binary_n`, `mysql.varbinary_n`) in `sql/aux_mysql_fix_compat.sql`, so
fresh 1.9 and every upgrade path get them; four immutable C functions return
1/0 or the byte `'1'`/`'0'`. BINARY(n) padding and length checks come from the
existing length casts, and DOUBLE/REAL/BLOB domains use their base types. Like
the existing int2/int8 casts they are assignment casts, not implicit casts:
function and operator resolution is unchanged. The same casts serve
CAST/CONVERT, DEFAULT, prepared parameters, INSERT SELECT, multi-table UPDATE
and generated columns; the generated-column policy admits the four C symbols.
SHOW CREATE TABLE prints the original expressions (`DEFAULT true`, `(id = 1)`;
VARBINARY defaults as `X'31'`) and its output replays on both engines with the
same values. In a database with aux_mysql, native PG sessions now also get
`true::numeric = 1` and `true::bytea = '\x31'`; `true::text` is still `'true'`.

| Verification | Debug | Release |
|---|---|---|
| PostgreSQL core regression | 231/231 (rerun, see below) | 231/231 |
| tests/run.sh | 26/26 | 26/26 |
| TESTING.md integration / Route A / upgrade / recovery invocations | 23/23 | 23/23 |
| release_fixes.py (boolean_assignments extended) | 504 checks | 504 checks |
| Actual released v18.6 binaries + 1.5 SQL → 1.9 | passed | passed |
| Fresh versus released 1.5 upgrade catalog | 3476 members, 1859 functions, 88 casts, equal | equal |
| All-type boolean probe vs MySQL 5.7 (also on the upgraded release copy) | 0 numeric/binary rejections | 0 numeric/binary rejections |
| 512 boolean write cases from the previous round | 0 value/error/warning differences | 0 value/error/warning differences |

The extended `release_fixes.py` fails on `6f9c4f3` (1105 on the first boolean
insert). The standalone binary and prefix suites passed on stock PG18, and
the new code adds no compiler warnings in either build.

Remaining boolean differences, not changed here:

- 488 rejections on DATE/DATETIME/TIMESTAMP/TIME, YEAR (check violation) and
  JSON (NULL booleans only; MySQL rejects 1/0 with 3140). Integer expressions
  fail on these types too. MySQL stores FALSE/NULL dates as `0000-00-00` or
  NULL in both modes and TRUE as `0000-00-00` with 1265 only outside strict
  mode (1292 in strict mode); TIME gets `00:00:01` and YEAR 2001.
- 196 BIT(1)/BIT(8) values are returned as text `'1'` instead of byte `0x01`;
  integer writes show the same BIT result format.
- Booleans in arithmetic (`(1=1)+0.5`), aggregates (AVG/MIN/MAX/STD/BIT_OR),
  ROUND/ABS and mixed-type GREATEST/COALESCE/IF still fail with 1305/1105.
- Integer expressions assigned to BINARY/VARBINARY/BLOB are still rejected.
- Duplicate-key failures return 1062 with PG's message text.

Evidence, builds and instances are under ohdev
`/tmp/claude-aux19-boolnum-20261109/`. The first debug core run failed only in
`join_hash` with `could not resize shared memory segment ... No space left on
device`: ohdev's 64 MB `/dev/shm` is shared with other long-running instances.
The debug instance was then switched to `dynamic_shared_memory_type = mmap`
(test host setting only) and the full core suite passed. Tests edited after the
first source sync were synced and `release_fixes.py` was rerun on both builds;
the final source tree in ohdev matches the patch byte for byte.

## Character boolean defaults — 2026-11-10

Independent review of `a000baa` found a remaining wrong-row path:
`VARCHAR DEFAULT TRUE/FALSE` stored `true`/`false` instead of `1`/`0`.
Against an existing unique key `true`, an INSERT omitting the column should
create a new key `1`; ODKU instead updated the old row and REPLACE deleted it.
Both sql_modes, debug/release, ALTER defaults and a real 1.5 upgrade reproduced
the issue. The stored-value bug was old, but v18.6 returned 1062 and kept the
old row in this conflict case, making the 1.9 wrong-row behavior a blocker.

The fix reuses the existing two conversions at the required stages:

- CREATE/ADD/MODIFY/CHANGE/SET DEFAULT use the same boolean-literal 1/0
  conversion as explicit assignments, before saving the default. This also
  fixes ADD COLUMN's fill of existing rows and CHAR(1)/CHAR(4) FALSE defaults.
- The existing implicit boolean-to-character assignment rewrite moves from
  post-parse analysis to the planner hook, after omitted columns and DEFAULT
  have been expanded and before constant folding. It therefore handles old
  defaults without a catalog/data migration. MySQL session/parser guards and
  the distinction between implicit assignments and explicit casts remain.
- The MySQL LIKE grammar now copies defaults. The inherited PG14 grammar
  excluded all defaults; PG18 copies AUTO_INCREMENT as an independent identity.
  Tests cover both default preservation and separate identity counters.

No kernel, SQL catalog, cast, extension-version or stored-key changes are
needed. Native PG character defaults/assignments keep `true`/`false`; genuine
quoted defaults retain their text. Existing rows are not rewritten. The
previously disclosed database-wide numeric/binary assignment casts remain.

| Verification | Debug | Release |
|---|---|---|
| PostgreSQL core regression | 231/231 | 231/231 |
| tests/run.sh | 26/26, no skips | 26/26, no skips |
| TESTING.md integration / Route A / upgrade / recovery invocations | 23/23 | 23/23 |
| release_fixes.py | 922 checks | 922 checks |
| Actual released v18.6 / 1.5 → 1.9, including inherited defaults | passed | passed |
| Fresh vs upgraded catalog | 3476 members, 1859 functions, 88 casts, equal | equal |
| Independent 480 boolean writes + 16 explicit conflict cases vs MySQL 5.7 | 0 value/error/warning differences | 0 value/error/warning differences |
| Independent default/ALTER/commit probes from the NO-GO review | matched MySQL 5.7 | matched MySQL 5.7 |

New regression tests fail on the unmodified `a000baa` binary. They cover
TRUE/FALSE, VARCHAR/CHAR, both sql_modes, implicit/explicit DEFAULT, INSERT
SELECT, IGNORE/ODKU/REPLACE/UPDATE, old row identity, rollback/commit,
SQL/binary prepared execution, custom/generic plans, LIKE, SHOW CREATE replay,
and native PG controls. The real upgrade test creates its defaults with the
released 1.5 binary, then checks both existing and new MySQL sessions after
upgrading. Generator checks reproduce all five tracked outputs byte for byte.

The first tests exposed the LIKE default exclusion. A later SHOW CREATE test
used a native PG fixture with `COLLATE default`, which is not a MySQL collation;
that fixture now declares MySQL collations while retaining the old implicit
boolean default expression. The genuine 1.5 upgrade tests do not use this
simulation. MySQL 5.7's sql_mode deprecation warnings are excluded from fixture
setup comparisons; every tested data write still compares warnings.

The first three-sample performance check showed container noise, so a further
ten samples alternated the two binaries after warmup on the same 300k rows.
Median sorting time was 57.2 ms versus 56.0 ms on `a000baa`; FIND_IN_SET was
0.624 s versus 0.618 s. Samples overlap and the earlier multi-fold sorting
regression did not return. Sorting/comparison implementations are unchanged.

Evidence, exact source/binary manifests, builds and retained data are under
ohdev `/tmp/openhalo-aux19-default-fix-20261110/`. Full suites ran against private
debug/release clusters using mmap dynamic shared memory; existing instances
were not modified. The default-value release blocker is closed by these tests.

## v18.6.1 release candidate — 2026-11-10

The copyright headers and regenerated outputs of `e1f2424` were committed after the
acceptance above, so the release candidate was rebuilt from a `git archive` of
`e1f2424` and the whole suite list was repeated on private debug
(`--enable-debug --enable-cassert`, `-O0`) and release clusters. The release
preparation commits after `e1f2424` change only Markdown files and `.gitignore`.

| Verification | Debug | Release |
|---|---|---|
| Build (`--with-openssl --enable-depend`), compiler warnings | 3, all pre-existing in `mys_gram.y` / `date_func.c` | same 3 |
| PostgreSQL core regression (`installcheck`, `aux_mysql` preloaded) | 231/231 | 231/231 |
| tests/run.sh | 26/26, no skips | 26/26, no skips |
| TESTING.md integration / Route A / upgrade / recovery invocations | 23/23 | 23/23 |
| release_fixes.py | 922 checks | 922 checks |
| Actual released v18.6 / 1.5 → 1.9 (`release_real_upgrade.py`) | passed | passed |
| Fresh vs upgraded catalog | 3476 members, 1859 functions, 88 casts, equal | equal |
| `tests/check_generated.py` (repository checkout) | five outputs byte-identical | — |

The suite list, flags and fixtures are those of the previous section's driver; the 1.6
fixture is byte-identical to the one used there. Both upgraded catalogs have the
normalized digest `57c443ddd7d0e763df143c815a79cedc29353ddec17f74baa8bb3680fd8a49aa`.
Builds, data directories, logs and per-case `status.json` are under ohdev
`/root/claude-release-18.6.1/`; existing instances were not modified.
