#!/bin/bash
# Rebuild pgvector / Citus / TimescaleDB from source against another kernel and
# install them into a fresh copy of that kernel, so eco_check.sh can tell
# "the stock binary breaks, a rebuild is enough" from "the source needs changes".
# Run inside the ohdev container.
#
#   rebuild.sh <name> <kernel prefix> [vector] [citus] [timescaledb]
#   e.g. rebuild.sh unified-rb /root/unified/inst-unified vector citus timescaledb
#        eco_check.sh unified-rb /root/unified/inst-unified 5610
#
# The prefix is copied to /root/eco/<name> (same layout eco_check.sh uses, with
# the stock RPM payloads), then the rebuilt extensions are installed over it.
set -eu
NAME=$1 SRC=$2
shift 2
EXTS=${*:-vector citus timescaledb}
HERE=$(cd "$(dirname "$0")" && pwd)
SRCROOT=/root/ecosrc
P=/root/eco/$NAME
B=$SRCROOT/build-$NAME
PGV=v0.8.7 CITUS=v14.3.0 TSDB=2.31.0   # same versions as the stock RPMs

mkdir -p "$SRCROOT"
[ -d $SRCROOT/pgvector ]    || git clone -q --depth 1 -b $PGV   https://github.com/pgvector/pgvector.git      $SRCROOT/pgvector
[ -d $SRCROOT/citus ]       || git clone -q --depth 1 -b $CITUS https://github.com/citusdata/citus.git       $SRCROOT/citus
[ -d $SRCROOT/timescaledb ] || git clone -q --depth 1 -b $TSDB  https://github.com/timescale/timescaledb.git  $SRCROOT/timescaledb

# create the prefix copy with the stock payloads (eco_check.sh does this on first use)
[ -d "$P" ] || bash "$HERE/eco_check.sh" "$NAME" "$SRC" 5699 pg_cron > /dev/null
PGC=$P/bin/pg_config
rm -rf "$B" && mkdir -p "$B"

for ext in $EXTS; do
  case $ext in
    vector)
      cp -a $SRCROOT/pgvector "$B/" && cd "$B/pgvector"
      make PG_CONFIG=$PGC -j8 > "$B/vector.log" 2>&1 && make PG_CONFIG=$PGC install >> "$B/vector.log" 2>&1 ;;
    citus)
      cp -a $SRCROOT/citus "$B/" && cd "$B/citus"
      { ./configure PG_CONFIG=$PGC && make -j16 && make install; } > "$B/citus.log" 2>&1 ;;
    timescaledb)
      cp -a $SRCROOT/timescaledb "$B/" && cd "$B/timescaledb"
      { ./bootstrap -DPG_CONFIG=$PGC -DREGRESS_CHECKS=OFF -DTAP_CHECKS=OFF -DWARNINGS_AS_ERRORS=OFF \
          -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSEND_TELEMETRY_DEFAULT=NO && \
        cd build && make -j16 && make install; } > "$B/timescaledb.log" 2>&1 ;;
  esac && echo "$NAME $ext: BUILD OK" || { echo "$NAME $ext: BUILD FAILED ($(grep -m1 -E 'error' "$B/$ext.log" | cut -c1-200))"; }
done
chown -R pg:pg "$P"
