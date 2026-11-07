#!/bin/bash
# sysbench OLTP scenarios over the MySQL protocol (server-side prepared
# statements, the sysbench default).
#
#   MYSQL_PORT=3306 THREADS=8 TIME=20 tests/sysbench.sh
#
# Needs schema "test" and user dbuser_mysql / Mysql.123 (kernel_instance.sh).
# Ignored errors: 1213/1205 are in sysbench's default list; 1062 comes from
# concurrent DELETE id=?; INSERT id=? on the same row, which PostgreSQL's
# READ COMMITTED does not serialize the way InnoDB locking reads do; 1020 is
# the matching "record has changed" case.
set -eu
PORT=${MYSQL_PORT:-3306}
THREADS=${THREADS:-8}
TIME=${TIME:-20}
TABLES=${TABLES:-4}
SIZE=${SIZE:-20000}
COMMON="--db-driver=mysql --mysql-host=127.0.0.1 --mysql-port=$PORT --mysql-user=dbuser_mysql
  --mysql-password=Mysql.123 --mysql-db=test --tables=$TABLES --table-size=$SIZE"
fail=0
sysbench oltp_read_write $COMMON cleanup >/dev/null 2>&1 || true
sysbench oltp_read_write $COMMON prepare >/dev/null
for t in oltp_point_select oltp_read_only oltp_read_write oltp_write_only; do
  out=$(sysbench $t $COMMON --threads=$THREADS --time=$TIME --report-interval=0 \
        --mysql-ignore-errors=1062,1213,1205,1020 run 2>&1) || { echo "== $t: FAILED"; echo "$out" | tail -5; fail=1; continue; }
  tps=$(echo "$out" | awk '/transactions:/ {gsub(/[(]/,"",$3); print $3}')
  qps=$(echo "$out" | awk '/queries:/ && /per sec/ {gsub(/[(]/,"",$3); print $3}')
  errs=$(echo "$out" | awk '/ignored errors:/ {print $3}')
  echo "== $t: ${tps} tps, ${qps} qps, ignored errors ${errs}"
done
sysbench oltp_read_write $COMMON cleanup >/dev/null
exit $fail
