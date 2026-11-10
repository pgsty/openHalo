-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
\pset tuples_only on
create extension citus;
select citus_set_coordinator_host('localhost', current_setting('port')::int);
select citus_set_node_property('localhost', current_setting('port')::int, 'shouldhaveshards', true);
create table d (id bigint primary key, v text);
select create_distributed_table('d', 'id');
insert into d select i, md5(i::text) from generate_series(1, 10000) i;
select 'distributed', count(*) from d;
create extension citus_columnar;
create table c (id int, v text) using columnar;
insert into c select i, md5(i::text) from generate_series(1, 10000) i;
select 'columnar', count(*), max(id) from c;
