-- DECIMAL (issue #5): precision and scale reach SHOW COLUMNS and | Copyright (c) 2026 PGSTY PTE. LTD.
-- information_schema (and so SHOW CREATE TABLE and dumps); a bare DECIMAL
-- is DECIMAL(10,0) as in MySQL.
drop table if exists dec_t;
create table dec_t (a decimal(8,2), b decimal, c numeric(5), d dec(0), e decimal(65,30));
insert into dec_t values (1.2345, 1.5, 12345.6, 7.7, 1.5);
select * from dec_t;
select column_name, column_type, numeric_precision, numeric_scale
  from information_schema.columns
 where table_schema = database() and table_name = 'dec_t' order by ordinal_position;
select cast(1.25 as decimal(5,1)) as c2, cast(2.5 as decimal(0)) as c3;
drop table dec_t;
