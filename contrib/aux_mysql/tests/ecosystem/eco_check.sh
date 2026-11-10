#!/bin/bash
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
# Ecosystem check: load the stock PG 18 builds of common extensions (PGDG / Pigsty
# RPMs installed under /usr/pgsql-18, i.e. compiled against official PG 18
# headers) into another kernel's install, unchanged, and smoke-test each one in
# a fresh instance.  Run inside the ohdev container.
#
#   eco_check.sh <name> <kernel prefix> <port> [extensions...]
#   e.g. eco_check.sh unified /root/unified/inst-unified 5600
#
# The kernel prefix is copied to $ECO_ROOT/<name> (default /root/eco; the
# original is not touched).  ECO_PRELOAD is preloaded in addition to what an
# extension needs (OpenHalo 18: ECO_PRELOAD=aux_mysql, its default setup).
# Prints one line per extension: PASS, FAIL (first error) or CRASH.
set -u
NAME=$1 SRC=$2 PORT=$3
shift 3
ECO_ROOT=${ECO_ROOT:-/root/eco}
ECO_PRELOAD=${ECO_PRELOAD:-}
EXTS=${*:-postgis vector citus timescaledb pg_cron}
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=/usr/pgsql-18
PKGS="postgis36_18 pgvector_18 citus_18 pg_cron_18 timescaledb-tsl_18"

if [ "$SRC" = "$STOCK" ]; then
  P=$STOCK
else
  P=$ECO_ROOT/$NAME
  if [ ! -d "$P" ]; then
    mkdir -p "$ECO_ROOT" && cp -a "$SRC" "$P"
    LIB=$("$P/bin/pg_config" --pkglibdir)
    SHARE=$("$P/bin/pg_config" --sharedir)
    for pkg in $PKGS; do
      rpm -ql "$pkg" | while read -r f; do
        [ -f "$f" ] || continue
        case "$f" in
          $STOCK/lib/bitcode/*) ;;
          $STOCK/lib/*) rel=${f#$STOCK/lib/}; mkdir -p "$LIB/$(dirname "$rel")"; cp -f "$f" "$LIB/$rel" ;;
          $STOCK/share/*) rel=${f#$STOCK/share/}; mkdir -p "$SHARE/$(dirname "$rel")"; cp -f "$f" "$SHARE/$rel" ;;
        esac
      done
    done
  fi
  chown -R pg:pg "$P"
fi
mkdir -p "$ECO_ROOT" && chown pg:pg "$ECO_ROOT"

preload_of() {
  local p
  case $1 in
    citus) p=citus ;;
    timescaledb) p=timescaledb ;;
    pg_cron) p=pg_cron ;;
    *) p="" ;;
  esac
  # citus must come first in shared_preload_libraries
  [ "$1" = citus ] && echo "${p}${ECO_PRELOAD:+, $ECO_PRELOAD}" || echo "${ECO_PRELOAD}${ECO_PRELOAD:+${p:+, }}${p}"
}

for ext in $EXTS; do
  D=$ECO_ROOT/data-$NAME-$ext
  LOG=$ECO_ROOT/log-$NAME-$ext.txt
  OUT=$ECO_ROOT/out-$NAME-$ext.txt
  rm -rf "$D" "$LOG" "$OUT" && mkdir -p "$D" && chown pg:pg "$D"
  # a crashed postmaster can leave its socket lock behind; drop it if nothing listens
  ss -ltn | grep -q ":$PORT " || rm -f /tmp/.s.PGSQL.$PORT /tmp/.s.PGSQL.$PORT.lock
  su pg -c "$P/bin/initdb -D $D -U postgres -E UTF8 --locale=C" > /dev/null 2>&1 || { echo "$NAME $ext: INITDB FAILED"; continue; }
  cat >> "$D/postgresql.conf" <<CONF
port = $PORT
listen_addresses = 'localhost'
unix_socket_directories = '/tmp'
shared_preload_libraries = '$(preload_of "$ext")'
max_worker_processes = 32
cron.database_name = 'postgres'
timescaledb.license = 'timescale'
timescaledb.telemetry_level = off
CONF
  # aux_mysql loaded for the check: no MySQL listener needed
  case "$ECO_PRELOAD" in *aux_mysql*) echo "mysql.port = 0" >> "$D/postgresql.conf" ;; esac
  su pg -c "$P/bin/pg_ctl -D $D -l $LOG -w start" > /dev/null 2>&1 || { echo "$NAME $ext: START FAILED ($(grep -m1 -E 'FATAL|PANIC' "$LOG"))"; continue; }
  su pg -c "$P/bin/psql -h /tmp -p $PORT -U postgres -X -q -v ON_ERROR_STOP=1 -f $HERE/$ext.sql" > "$OUT" 2>&1
  rc=$?
  su pg -c "$P/bin/pg_ctl -D $D -m fast -w -t 30 stop" > /dev/null 2>&1 || \
    su pg -c "$P/bin/pg_ctl -D $D -m immediate -w stop" > /dev/null 2>&1
  [ -f "$D/postmaster.pid" ] && kill -9 "$(head -1 "$D/postmaster.pid")" 2>/dev/null && sleep 1
  if grep -q -E "TRAP:|terminated by signal|PANIC" "$LOG"; then
    echo "$NAME $ext: CRASH ($(grep -m1 -E 'TRAP:|terminated by signal|PANIC' "$LOG" | cut -c1-160))"
  elif [ $rc -ne 0 ]; then
    echo "$NAME $ext: FAIL ($(grep -m1 -E 'ERROR|FATAL|error' "$OUT" | cut -c1-160))"
  else
    echo "$NAME $ext: PASS ($(tr '\n' ' ' < "$OUT" | tr -s ' ' | cut -c1-120))"
  fi
done
