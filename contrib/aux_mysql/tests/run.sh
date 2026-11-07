#!/bin/bash
# Run the aux_mysql smoke tests against a running instance (inside the ohdev container).
#   docker exec ohdev bash /w/openhalo-pg18/tests/run.sh
# Driver tests (Go / Connector/J) run when their toolchains are present;
# SECTIONS selects which parts of them run (default: all).
set -u
cd "$(dirname "$0")"
SECTIONS=${SECTIONS:-"auth stmt txn sem"}
# target instance (create one with kernel_instance.sh)
PGBIN=${PGBIN:-/usr/local/pgsql/bin}
PGPORT=${PGPORT:-5432}
export MYSQL_PORT=${MYSQL_PORT:-3306}
OSUSER=${OSUSER:-pg}
TMPDIR=${TMPDIR:-/tmp}
PSQL="$PGBIN/psql -h /tmp -p $PGPORT -U postgres -qAt"
M="mysql -h127.0.0.1 -P$MYSQL_PORT -udbuser_mysql -pMysql.123"
JAR=/root/drivers/mysql-connector-j-8.4.0.jar
fail=0
echo "== reset schema test"
su $OSUSER -c "$PSQL -c 'drop schema if exists test cascade' -c 'create schema test authorization dbuser_mysql'" >/dev/null 2>&1
for f in m1_pgsyntax.sql m2_basic.sql m3_ddl.sql m4_more.sql; do
  out=$($M test --force -t < $f 2>&1)
  errs=$(echo "$out" | grep -c "^ERROR")
  echo "== $f: $errs error(s)"
  echo "$out" | grep "^ERROR" | sed 's/^/   /'
done

# expected-output tests: <name>.sql is compared with expected/<name>.out
for f in p*_*.sql; do
  [ -f "$f" ] || continue
  name=${f%.sql}
  out=$($M test --force -t < $f 2>&1)
  if diff -u expected/$name.out <(echo "$out") > $TMPDIR/$name.diff; then
    echo "== $f: ok"
  else
    echo "== $f: FAILED"; head -40 $TMPDIR/$name.diff | sed 's/^/   /'; fail=1
  fi
done

echo "== pymysql app"
python3 app_pymysql.py >$TMPDIR/app.out 2>&1 && echo "   ok" || { echo "   FAILED"; tail -5 $TMPDIR/app.out; fail=1; }

if echo "$SECTIONS" | grep -qw stmt; then
  echo "== raw COM_STMT_* protocol"
  python3 stmt_raw.py > $TMPDIR/stmt_raw.out 2>&1 && echo "   ok" || { echo "   FAILED"; grep -E "FAIL|Error" $TMPDIR/stmt_raw.out | sed 's/^/   /'; fail=1; }
fi

echo "== caching_sha2_password (mysql CLI)"
su $OSUSER -c "$PSQL -c 'select mysql.flush_auth_cache()'" >/dev/null
for opt in "" "" "--ssl"; do   # RSA full auth, fast auth, TLS
  r=$($M $opt test -N -e "select 'ok'" 2>&1)
  [ "$r" = "ok" ] && echo "   ok ${opt:-plain}" || { echo "   FAILED ${opt:-plain}: $r"; fail=1; }
done
r=$(mysql -h127.0.0.1 -P$MYSQL_PORT -udbuser_mysql -pbad test -e "select 1" 2>&1)
echo "$r" | grep -q "ERROR 1045" && echo "   ok wrong password" || { echo "   FAILED wrong password: $r"; fail=1; }
r=$(mysqlslap -h127.0.0.1 -P$MYSQL_PORT -udbuser_mysql -pMysql.123 --create-schema=test --concurrency=8 \
    --iterations=1 --number-of-queries=800 --query="select 1" 2>&1)
echo "$r" | grep -q "Average" && echo "   ok mysqlslap" || { echo "   FAILED mysqlslap: $r"; fail=1; }

if [ -x /root/drivers/gotest ]; then
  for s in $SECTIONS; do
    [ "$s" = sem ] && continue
    /root/drivers/gotest $s > $TMPDIR/go.out 2>&1 && echo "== go $s: ok" || { echo "== go $s: FAILED"; grep FAIL $TMPDIR/go.out | sed 's/^/   /'; fail=1; }
  done
fi
if [ -f $JAR ] && [ -f /root/drivers/DriverTest.class ]; then
  for s in $SECTIONS; do
    [ "$s" = sem ] && continue
    java -cp $JAR:/root/drivers DriverTest $s > $TMPDIR/java.out 2>&1 && echo "== java $s: ok" || { echo "== java $s: FAILED"; grep -E "FAIL|Exception" $TMPDIR/java.out | head -10 | sed 's/^/   /'; fail=1; }
  done
fi
exit $fail
