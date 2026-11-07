#!/bin/bash
# MySQL storage-engine options on kernels with table access methods:
#   ENGINE=OrioleDB  -> USING orioledb      (OrioleDB kernel + orioledb)
#   ENCRYPTION='Y'   -> USING tde_heap      (Percona kernel + pg_tde)
# For TDE it also checks the data files on disk for the plaintext markers.
#
#   engines_check.sh <prefix> <pgport> <mysqlport> <pgdata>
set -u
BIN=$1/bin PGPORT=$2 MYPORT=$3 DATA=$4
P="$BIN/psql -h /tmp -p $PGPORT -U postgres -At"
M="mysql -h127.0.0.1 -P$MYPORT -udbuser_mysql -pMysql.123 test"

am_of() { $P -c "select a.amname from pg_class c join pg_am a on a.oid = c.relam where c.oid = 'test.$1'::regclass"; }

if $P -c "select 1 from pg_am where amname = 'tde_heap'" | grep -q 1; then
  $M -e "drop table if exists secret, later_t, clear_t;
         create table secret (id int auto_increment primary key, card varchar(32)) encryption='Y';
         create table later_t (id int primary key, card varchar(32));
         create table clear_t (id int primary key, card varchar(32));
         insert into secret (card) values ('SECRET-MARKER-1');
         insert into later_t values (1, 'LATER-MARKER-2');
         insert into clear_t values (1, 'CLEAR-MARKER-3');
         alter table later_t encryption='Y';"
  $P -c "checkpoint" >/dev/null
  for t in secret later_t clear_t; do
    f="$DATA/$($P -c "select pg_relation_filepath('test.$t')")"
    echo "$t: access_method=$(am_of $t) pg_tde_is_encrypted=$($P -c "select pg_tde_is_encrypted('test.$t')")" \
         "plaintext_in_datafile=$(grep -c -a MARKER "$f")" \
         "mysql_reads=$($M -N -e "select card from $t")"
  done
fi

if $P -c "select 1 from pg_am where amname = 'orioledb'" | grep -q 1; then
  $M -e "drop table if exists o_t; create table o_t (id int auto_increment primary key, v varchar(10)) engine=OrioleDB;
         insert into o_t (v) values ('x'), ('y');"
  echo "o_t: access_method=$(am_of o_t) rows=$($M -N -e "select count(*) from o_t")"
fi

# MySQL ALTERs that rewrite an encrypted table must keep it encrypted
if $P -c "select 1 from pg_am where amname = 'tde_heap'" | grep -q 1; then
  $M -e "alter table secret modify card varchar(64) not null; alter table secret add column extra int;
         insert into secret (card) values ('SECRET-MARKER-4')"
  $P -c "checkpoint" >/dev/null
  f="$DATA/$($P -c "select pg_relation_filepath('test.secret')")"
  echo "secret after MODIFY: access_method=$(am_of secret) pg_tde_is_encrypted=$($P -c "select pg_tde_is_encrypted('test.secret')")" \
       "plaintext_in_datafile=$(grep -c -a MARKER "$f")" "rows=$($M -N -e "select count(*) from secret")"
fi
