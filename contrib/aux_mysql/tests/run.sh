#!/bin/bash
# Smoke and driver checks. Exit 0: all passed; 1: failed; 2: incomplete (SKIP).
# Every invocation has its own logs; driver executables are rebuilt from source.
set -uo pipefail
cd "$(dirname "$0")"
SECTIONS=${SECTIONS:-"auth stmt txn sem"}
PGBIN=${PGBIN:-/usr/local/pgsql/bin}
export MYSQL_PORT=${MYSQL_PORT:-3306}
LOG_DIR=${LOG_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/aux_mysql-tests.XXXXXX")}
mkdir -p "$LOG_DIR"
LOG_DIR=$(cd "$LOG_DIR" && pwd)
DRIVER_DIR=${DRIVER_DIR:-$LOG_DIR/drivers}
JAR=${JAR:-/root/drivers/mysql-connector-j-8.4.0.jar}
PSQL=("$PGBIN/psql" -h "${PGHOST:-127.0.0.1}" -p "${PGPORT:-5432}" -U "${PGUSER:-postgres}" -d "${PGDATABASE:-postgres}" -XqAt -v ON_ERROR_STOP=1)
M=(mysql -h127.0.0.1 -P"$MYSQL_PORT" -udbuser_mysql -pMysql.123)
passed=0 failed=0 skipped=0
: > "$LOG_DIR/status.tsv"
record() {
 local state=$1 name=$2
 printf '%s\t%s\n' "$state" "$name" | tee -a "$LOG_DIR/status.tsv"
 case "$state" in PASS) passed=$((passed+1));; FAIL) failed=$((failed+1));; SKIP) skipped=$((skipped+1));; esac
}
run_case() {
 local name=$1
 shift
 if "$@" > "$LOG_DIR/$name.log" 2>&1; then record PASS "$name"
 else record FAIL "$name"; tail -15 "$LOG_DIR/$name.log"; fi
}
for tool in mysql mysqlslap python3; do
 if ! command -v "$tool" >/dev/null; then record SKIP "dependency-$tool"; fi
done
if [ "$skipped" -gt 0 ]; then echo "INCOMPLETE: required tools missing; logs=$LOG_DIR"; exit 2; fi
if ! "${PSQL[@]}" -c 'drop schema if exists test cascade' -c 'create schema test authorization dbuser_mysql' > "$LOG_DIR/reset.log" 2>&1; then
 record FAIL reset; cat "$LOG_DIR/reset.log"; exit 1
fi
for f in m1_pgsyntax.sql m2_basic.sql m3_ddl.sql m4_more.sql; do
 rc=0
 "${M[@]}" test --force -t < "$f" > "$LOG_DIR/$f.log" 2>&1 || rc=$?
 # --force intentionally continues past two negative tests in m1. Every
 # other error (including connection failures) is an unexpected failure.
 sed -nE 's/^ERROR ([0-9]+) .* at line ([0-9]+):.*/\2 \1/p' "$LOG_DIR/$f.log" > "$LOG_DIR/$f.errors"
 awk -v f="$f" '$1==f {print $2, $3}' expected/mysql-errors.tsv > "$LOG_DIR/$f.expected-errors"
 if [ "$rc" -eq 0 ] && diff -u "$LOG_DIR/$f.expected-errors" "$LOG_DIR/$f.errors" > "$LOG_DIR/$f.diff" &&
    ! grep '^ERROR' "$LOG_DIR/$f.log" | grep -v ' at line ' >/dev/null; then record PASS "$f"
 else record FAIL "$f"; cat "$LOG_DIR/$f.diff"; fi
done
for f in p*_*.sql; do
 [ -f "$f" ] || continue
 name=${f%.sql}
 rc=0
 "${M[@]}" test --force -t < "$f" > "$LOG_DIR/$name.log" 2>&1 || rc=$?
 if [ "$rc" -eq 0 ] && diff -u "expected/$name.out" "$LOG_DIR/$name.log" > "$LOG_DIR/$name.diff"; then record PASS "$f"
 else record FAIL "$f"; head -40 "$LOG_DIR/$name.diff"; fi
done
run_case pymysql python3 app_pymysql.py
if python3 -c 'import pymysql, psycopg2' > "$LOG_DIR/ab-dependencies.log" 2>&1; then
 run_case ab-regress env MYSQL_USER="${PGUSER:-postgres}" MYSQL_PASSWORD="${PGPASSWORD:-}" python3 ab_regress.py
else record SKIP ab-python-dependencies; fi
if [[ " $SECTIONS " == *" stmt "* ]]; then run_case raw-stmt python3 stmt_raw.py; fi
run_case flush-auth "${PSQL[@]}" -c 'select mysql.flush_auth_cache()'
for name in rsa fast tls; do
 opts=(); [ "$name" != tls ] || opts+=(--ssl)
 if "${M[@]}" "${opts[@]}" test -N -e "select 'ok'" > "$LOG_DIR/auth-$name.log" 2>&1 &&
    [ "$(cat "$LOG_DIR/auth-$name.log")" = ok ]; then record PASS "auth-$name"
 else record FAIL "auth-$name"; fi
done
if mysql -h127.0.0.1 -P"$MYSQL_PORT" -udbuser_mysql -pbad test -e 'select 1' > "$LOG_DIR/auth-wrong.log" 2>&1; then
 record FAIL auth-wrong
elif grep -q 'ERROR 1045' "$LOG_DIR/auth-wrong.log"; then record PASS auth-wrong
else record FAIL auth-wrong; fi
run_case mysqlslap mysqlslap -h127.0.0.1 -P"$MYSQL_PORT" -udbuser_mysql -pMysql.123 --create-schema=test \
 --concurrency=8 --iterations=1 --number-of-queries=800 --query='select 1'
mkdir -p "$DRIVER_DIR"
if command -v go >/dev/null; then
 if (cd drivers/go && go build -o "$DRIVER_DIR/gotest" .) > "$LOG_DIR/go-build.log" 2>&1; then
  for s in $SECTIONS; do [ "$s" = sem ] || run_case "go-$s" "$DRIVER_DIR/gotest" "$s"; done
 else record FAIL go-build; fi
else record SKIP go-toolchain; fi
if command -v javac >/dev/null && command -v java >/dev/null && [ -f "$JAR" ]; then
 if javac -d "$DRIVER_DIR" -cp "$JAR" drivers/java/DriverTest.java > "$LOG_DIR/java-build.log" 2>&1; then
  for s in $SECTIONS; do [ "$s" = sem ] || run_case "java-$s" java -cp "$JAR:$DRIVER_DIR" DriverTest "$s"; done
 else record FAIL java-build; fi
else record SKIP java-toolchain-or-connector; fi
printf 'SUMMARY passed=%s failed=%s skipped=%s total=%s logs=%s\n' "$passed" "$failed" "$skipped" "$((passed+failed+skipped))" "$LOG_DIR"
[ "$failed" -eq 0 ] || exit 1
[ "$skipped" -eq 0 ] || exit 2
