-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
\pset tuples_only on
create extension timescaledb;
create table m (t timestamptz not null, dev int, val float8);
select create_hypertable('m', by_range('t', interval '1 day'));
insert into m select ts, n % 10, random() from generate_series('2026-01-01'::timestamptz, '2026-01-10', interval '1 minute') with ordinality as s(ts, n);
select 'buckets', count(*) from (select time_bucket('1 hour', t), avg(val) from m group by 1) s;
alter table m set (timescaledb.compress, timescaledb.compress_segmentby = 'dev');
select 'compressed', count(*) from (select compress_chunk(c) from show_chunks('m') c) s;
select 'rows', count(*) from m;
