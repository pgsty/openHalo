#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
base="$PWD/tests/binary_fix"
build="${BUILD_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/review_fix_binary.XXXXXX")}"
mkdir -p "$build"
build="$(cd "$build" && pwd)"
sock="$(mktemp -d /tmp/review_fix_binary_socket.XXXXXX)"
export BINARY_FIX_BUILD_DIR="$build"
pgbin="$("${PG_CONFIG:-pg_config}" --bindir)"
finish() {
    if [ -f "$build/pgdata/postmaster.pid" ]; then
        "$pgbin/pg_ctl" -D "$build/pgdata" -m fast stop > "$build/stop.log"
    fi
    rmdir "$sock" 2>/dev/null || true
}
trap finish EXIT
# All generated inputs, objects, shared libraries, logs and pgdata go to BUILD_DIR.
python3 "$base/build.py" > "$build/build.log" 2>&1
if [ ! -f "$build/pgdata/PG_VERSION" ]; then
    "$pgbin/initdb" -D "$build/pgdata" --no-locale -E UTF8 -A trust > "$build/initdb.log"
fi
"$pgbin/pg_ctl" -D "$build/pgdata" -l "$build/server.log" \
    -o "-k $sock -p 60463 -c listen_addresses=''" start > "$build/start.log"
export PGHOST="$sock" PGPORT=60463 PGDATABASE=postgres PGUSER="$(id -un)"
python3 "$base/prepare.py" "$build/review_fix_binary.so"
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$build/setup.sql" > "$build/setup.log" 2>&1
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$base/check.sql" > "$build/check.log" 2>&1
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$base/refresh.sql" > "$build/refresh.log" 2>&1
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$base/string_contexts.sql" > "$build/string_contexts.log" 2>&1
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$base/locate.sql" > "$build/locate.log" 2>&1
python3 "$base/pg_protocol.py" > "$build/protocol.log" 2>&1
"$pgbin/pg_dump" --schema=review_fix_binary --format=custom -f "$build/roundtrip.dump"
"$pgbin/psql" -X -c 'DROP SCHEMA review_fix_binary CASCADE' > "$build/drop.log" 2>&1
"$pgbin/pg_restore" -e -d postgres "$build/roundtrip.dump" > "$build/restore.log" 2>&1
"$pgbin/psql" -X -v ON_ERROR_STOP=1 -f "$base/roundtrip.sql" > "$build/roundtrip.log" 2>&1
printf 'PASS: binary/hexlit/TEXT grammar, storage, indexes, PG text/binary protocol and pg_dump roundtrip. Logs: %s\n' "$build"
