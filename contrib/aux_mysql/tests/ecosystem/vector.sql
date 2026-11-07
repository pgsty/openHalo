\pset tuples_only on
create extension vector;
create table v (id int, e vector(3));
insert into v select i, array[random(), random(), random()]::vector from generate_series(1, 5000) i;
create index on v using hnsw (e vector_l2_ops);
set enable_seqscan = off;
select 'knn', count(*) from (select id from v order by e <-> '[0.5,0.5,0.5]' limit 5) s;
