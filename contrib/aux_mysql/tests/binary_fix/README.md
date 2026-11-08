# Binary and character compatibility checks

These tests cover fixed and varying binary lengths, hexadecimal numeric/string
contexts, NUL-preserving TEXT, typed string functions, indexed comparisons,
user locks, protocol bytes and logical backups.

## Integrated MySQL protocol

Use a disposable instance with matching OpenHalo kernel and aux_mysql 1.9:

```sh
MYSQL_USER=dbuser_mysql MYSQL_PASSWORD=fixture-password \
  python3 tests/binary_fix/mysql_check.py --port 3792
```

The script creates a unique schema and removes it on exit. It tests text and raw
prepared packets, binary padding/truncation and defaults, CONCAT/CONCAT_WS/ELT,
lazy IF/IFNULL, binary btree equality/range scans, NUL-aware regex/LIKE/search and
lock arguments. It changes session settings only and releases its locks.

LOCATE/INSTR checks include significant trailing spaces, Unicode weights,
character positions, NUL, overlapping matches and 65,535-character searches.
The dedicated MySQL general_ci/bin path uses linear KMP matching with memory
proportional to the needle. Its generous five-second timeout detects the former
quadratic/cubic behavior without requiring a microbenchmark threshold.

## Private PostgreSQL module

The standalone harness requires PG18 server headers/binaries, a C compiler,
Python 3, flex, bison and Perl:

```sh
BINARY_FIX_READONLY_REPO=/path/to/openhalo \
PG_CONFIG=/path/to/pg18/bin/pg_config \
BUILD_DIR=/tmp/aux-binary-check \
  tests/binary_fix/run.sh
```

The read-only repository must contain branch `pg14`. Alternatively provide
`BINARY_FIX_PG14_GRAMMAR=/path/to/pg14/mys_gram.y`. All objects, generated parser
files, logs, dumps and cluster data go under BUILD_DIR (a temporary directory by
default). A private Unix socket is used; the cluster stops on exit. No shared
installation is modified.

The test bundle compiles the production binary/lock implementations, regenerated
MySQL grammar and actual prefix collation helper. Its shim supplies protocol,
sql_mode and IGNORE flags. Tests verify input/output bytes, strict and lenient
coercions, casts, comparison/index semantics, UTF-8/NUL operations, ICU behavior,
libpq text/binary I/O and pg_dump/pg_restore identity. `refresh.sql` exercises
catalog cache invalidation; the integrated `release_online_upgrade.py` separately
checks a real connection across ALTER EXTENSION.

The standalone harness supplements the wire suite; it does not validate the full
OpenHalo listener or integration with executor/utility hooks. Full debug/release
acceptance is documented in [../../TESTING.md](../../TESTING.md).

## Representation boundaries

New BINARY/VARBINARY types store exact byte counts; new TEXT-family columns use
NUL-safe character storage. Existing binary domains have no recoverable length,
and old PG TEXT columns retain their representation until explicitly migrated.
CHAR/VARCHAR remain native PG character types. Use original schema definitions
for such migrations and verify bytes after conversion.

The dedicated general_ci/bin collations implement MySQL 5.7 simple weights and
PAD SPACE comparison; substring search preserves spaces. Legacy generic ICU
collations retain their prior semantics and search path. Arbitrary MySQL
function/type combinations and unsigned integers above signed int8 are outside
this harness's coverage. PostgreSQL callers of legacy SQL helpers do not pass
through the MySQL grammar's typed string lowering.
