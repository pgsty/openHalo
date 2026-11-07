-- MySQL value semantics (P0-4)

-- "/" yields DECIMAL (scale + 4), DIV is integer division, x/0 and x%0 are NULL
select 5/2 as a, 7/7 as b, 1/3 as c, -7/2 as d, 5.0/2 as e, 2.5/0.5 as f, 10/4.000 as g;
select 5 div 2 as a, -7 div 2 as b, 5.5 div 2 as c, 7 div 0 as d;
select 1/0 as a, 5 % 0 as b, mod(5, 0) as c, 5 % 3 as d, 5.5 % 2 as e, "5"/"2" as f, 1.0/0 as g;

-- case-insensitive comparison for string columns (default collation), _bin stays binary
drop table if exists ci;
create table ci (id int primary key, name varchar(20), code varchar(20) collate utf8mb4_bin,
                 t text, unique key uk_name (name));
insert into ci values (1, "Alice", "Bob", "Hello World");
select count(*) as eq from ci where name = "alice";
select count(*) as lk from ci where name like "ALI%";
select count(*) as bin_eq from ci where code = "bob";
select count(*) as re from ci where name regexp "^al";
select count(*) as rl from ci where t rlike "world$";
select locate("WORLD", t) as loc, instr(t, "world") as ins, t like "%WORLD" as tl from ci;
insert into ci values (2, "ALICE", "x", "y");
select name, count(*) from ci group by name;
drop table ci;

-- ON UPDATE CURRENT_TIMESTAMP
drop table if exists ts;
create table ts (id int primary key, v int,
                 updated_at datetime(3) default "2000-01-01 00:00:00" on update current_timestamp(3));
insert into ts (id, v) values (1, 1), (2, 2);
update ts set v = 10 where id = 1;
select id, updated_at > "2020-01-01" as auto_updated from ts order by id;
update ts set v = 20, updated_at = "2001-01-01 00:00:00" where id = 2;
select id, updated_at from ts where id = 2;
update ts set v = v where id = 2;
select id, updated_at from ts where id = 2;
alter table ts modify updated_at datetime(3) default "2000-01-01 00:00:00";
update ts set v = 30 where id = 2;
select id, updated_at from ts where id = 2;
alter table ts add column touched timestamp null on update current_timestamp;
update ts set v = 40 where id = 2;
select id, touched is not null as touched from ts where id = 2;
drop table ts;

-- AUTO_INCREMENT continues after explicit values; ALTER TABLE ... AUTO_INCREMENT
drop table if exists ai;
create table ai (id bigint not null auto_increment primary key, v int);
insert into ai (v) values (1);
insert into ai (id, v) values (100, 2);
insert into ai (v) values (3);
insert into ai (id, v) values (50, 4);
insert into ai (v) values (5);
alter table ai auto_increment = 1000;
insert into ai (v) values (6);
update ai set id = 2000 where v = 6;
insert into ai (v) values (7);
select id, v from ai order by id;
drop table ai;

-- strict mode (default) rejects bad values; non-strict converts them with warnings
drop table if exists ns;
create table ns (i int, t tinyint, u tinyint unsigned, n decimal(5,2), f double, s varchar(5), c char(3));
insert into ns values ("12abc", 300, -5, 12345.678, "1.5e2x", "toolongvalue", "abcdef");
set sql_mode = "";
insert into ns values ("12abc", 300, -5, 12345.678, "1.5e2x", "toolongvalue", "abcdef");
insert into ns (i, s) values ("abc", "ok"), (" 7 ", "中文字符测试");
update ns set t = 1000 where i = 7;
select * from ns order by i;
set sql_mode = "STRICT_TRANS_TABLES,ERROR_FOR_DIVISION_BY_ZERO";
insert into ns (i) values (1/0);
set sql_mode = "STRICT_TRANS_TABLES";
insert into ns (i) values (1/0);
select count(*) as null_rows from ns where i is null;
set sql_mode = "ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES,NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION";
drop table ns;
