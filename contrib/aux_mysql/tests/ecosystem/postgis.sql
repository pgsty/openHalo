-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
\pset tuples_only on
create extension postgis;
create table g (id int, geom geometry(Point, 4326));
insert into g select i, ST_SetSRID(ST_MakePoint(random() * 360 - 180, random() * 180 - 90), 4326) from generate_series(1, 5000) i;
create index on g using gist (geom);
analyze g;
set enable_seqscan = off;
select 'dwithin', count(*) > 0 from g where ST_DWithin(geom, ST_SetSRID(ST_MakePoint(0, 0), 4326), 30);
select 'buffer', round(ST_Area(ST_Buffer(ST_MakePoint(0, 0), 1))::numeric, 2);
select 'proj', ST_AsText(ST_SnapToGrid(ST_Transform(ST_SetSRID(ST_MakePoint(120, 30), 4326), 3857), 1));
