-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
\pset tuples_only on
create extension vector;
create table v (id int, e vector(3));
insert into v select i, array[random(), random(), random()]::vector from generate_series(1, 5000) i;
create index on v using hnsw (e vector_l2_ops);
set enable_seqscan = off;
select 'knn', count(*) from (select id from v order by e <-> '[0.5,0.5,0.5]' limit 5) s;
