#!/bin/bash
# Create and start an aux_mysql test instance on any PG18-based kernel
# (inside the ohdev container).  The kernel must carry the protocol-hook
# patch, and aux_mysql must be installed into its prefix.
#
#   kernel_instance.sh <prefix> <pgdata> <pgport> <mysqlport> [initdb options...]
#   e.g. kernel_instance.sh /usr/local/pgsql /home/pg/oh18 5442 3316
#        EXTRA_PRELOAD=orioledb kernel_instance.sh /root/forks/oriole /home/pg/oriole 5452 3326
#
# Runs the server as OS user $OSUSER (default pg).  TLS uses $CERT_DIR/server.{crt,key}
# when present, otherwise a self-signed certificate is generated.
#
# Then: PGBIN=<prefix>/bin PGPORT=<pgport> MYSQL_PORT=<mysqlport> tests/run.sh
set -eu
PREFIX=$1 DATA=$2 PGPORT=$3 MYPORT=$4
shift 4
BIN=$PREFIX/bin
OSUSER=${OSUSER:-pg}
CERT_DIR=${CERT_DIR:-/home/pg/data}
if [ ! -f "$DATA/PG_VERSION" ]; then
  mkdir -p "$DATA" && chown $OSUSER: "$DATA"
  su $OSUSER -c "$BIN/initdb -D $DATA -U postgres -E UTF8 --locale=C --auth-local=trust --auth-host=scram-sha-256 $*" >/dev/null
  if [ -f "$CERT_DIR/server.crt" ] && [ -f "$CERT_DIR/server.key" ]; then
    cp "$CERT_DIR/server.crt" "$CERT_DIR/server.key" "$DATA/"
  else
    openssl req -new -x509 -days 3650 -nodes -subj /CN=localhost \
      -keyout "$DATA/server.key" -out "$DATA/server.crt" >/dev/null 2>&1
  fi
  chown $OSUSER: "$DATA"/server.* && chmod 600 "$DATA/server.key"
  # keep what the kernel preloads itself (IvorySQL: liboracle_parser, ivorysql_ora;
  # OpenHalo 18: aux_mysql)
  PRELOAD=$(su $OSUSER -c "$BIN/postgres -D $DATA -C shared_preload_libraries")
  # EXTRA_PRELOAD: libraries the kernel's own extensions need (orioledb, pg_tde)
  PRELOAD=${PRELOAD:+$PRELOAD, }${EXTRA_PRELOAD:+$EXTRA_PRELOAD, }
  case ", $PRELOAD" in *", aux_mysql, "*) PRELOAD=${PRELOAD%, } ;; *) PRELOAD=${PRELOAD}aux_mysql ;; esac
  cat >> "$DATA/postgresql.conf" <<CONF
listen_addresses = '*'
port = $PGPORT
unix_socket_directories = '/tmp'
shared_preload_libraries = '$PRELOAD'
mysql.port = $MYPORT
ssl = on
logging_collector = off
CONF
  echo "host all all 0.0.0.0/0 scram-sha-256" >> "$DATA/pg_hba.conf"
fi
su $OSUSER -c "$BIN/pg_ctl -D $DATA -l $DATA/../$(basename $DATA).log -w restart" >/dev/null 2>&1 || \
  su $OSUSER -c "$BIN/pg_ctl -D $DATA -l $DATA/../$(basename $DATA).log -w start" >/dev/null
P="$BIN/psql -h /tmp -p $PGPORT -U postgres -qAt -v ON_ERROR_STOP=1"
su $OSUSER -c "$P -c \"select 1 from pg_roles where rolname = 'dbuser_mysql'\"" | grep -q 1 || \
  su $OSUSER -c "$P -c \"create role dbuser_mysql login superuser password 'Mysql.123'\""
# Initialize explicitly for a test fixture; ordinary PG startup creates no SQL objects.
su $OSUSER -c "$P -c 'create extension if not exists aux_mysql cascade'"
su $OSUSER -c "$P -c 'create schema if not exists test authorization dbuser_mysql'"
echo "instance ready: $DATA  pg=$PGPORT  mysql=$MYPORT  ($($BIN/postgres --version))"
