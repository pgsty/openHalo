-- openHalo upstream (2c26131..90fc8d7) behaviour carried into aux_mysql.
-- Expected output: expected/p5_upstream.out (tests/run.sh compares it).
drop user if exists u5;

-- 0d621b7 / cdbd589 / 1b250eb: CREATE USER ... IDENTIFIED BY, ALTER USER, DROP USER
create user u5 identified by 'Pass.5a';
alter user u5 password 'Pass.5b';
drop user u5;

-- f3e3e15: ALTER SYSTEM answers with an OK packet
alter system set work_mem = '8MB';
alter system reset work_mem;

-- 264834a: SHOW ENGINE ... STATUS is rejected with a clear message
show engine innodb status;

-- 3e43ac3: SHOW CHARACTER SET WHERE ...
show character set where charset = 'utf8mb4';

-- 45a0039: @@datadir comes from the server's data directory
select @@datadir is not null and @@datadir <> '' as has_datadir;

-- d86ca29: uuid() without uuid-ossp
select length(uuid()) as len, uuid() regexp '^[0-9a-f-]{36}$' as fmt;

-- 6d871d5 (14.18): column collations
--   *_ci -> case-insensitive, other names -> case-sensitive,
--   no COLLATE -> case-insensitive unless the table's COLLATE= is not *_ci,
--   MODIFY without COLLATE -> case-insensitive, COLLATE on non-strings ignored
create table c1 (a varchar(10), b varchar(10) collate utf8mb4_bin,
                 c text collate utf8mb4_general_ci, d int collate utf8mb4_bin);
insert into c1 values ('abc', 'abc', 'abc', 1);
select (select count(*) from c1 where a = 'ABC') as a_ci,
       (select count(*) from c1 where b = 'ABC') as b_bin,
       (select count(*) from c1 where c = 'ABC') as c_ci;
create table c2 (a varchar(10), b char(5) collate utf8mb4_unicode_ci)
  default charset = utf8mb4 collate = utf8mb4_bin;
insert into c2 values ('abc', 'abc');
select (select count(*) from c2 where a = 'ABC') as a_tbl_bin,
       (select count(*) from c2 where b = 'ABC') as b_ci;
alter table c2 modify a varchar(20);
alter table c2 add column e varchar(5) collate latin1_bin;
insert into c2 values ('x', 'x', 'xyz');
select (select count(*) from c2 where a = 'ABC') as a_modified,
       (select count(*) from c2 where e = 'XYZ') as e_bin;
-- f3518a1: Collation column / COLLATE in SHOW CREATE TABLE (portable MySQL names)
show full columns from c1;
show create table c2;
-- SHOW CREATE TABLE output replays
create table c3 (a varchar(5) collate case_sensitive, b varchar(5) collate case_insensitive);
insert into c3 values ('abc', 'abc');
select (select count(*) from c3 where a = 'ABC') as a_cs, (select count(*) from c3 where b = 'ABC') as b_ci;

-- aa679e6 / e4f58ea: type display (year, binary, varbinary, bit)
create table ty (y year, bi binary(4), vb varbinary(8), bt bit(3));
show columns from ty;

-- 154ce7a: AUTO_INCREMENT
--   ALTER TABLE ... AUTO_INCREMENT on an empty table resets the counter
create table ai (id int auto_increment primary key, v int);
insert into ai(v) values (1), (2), (3);
delete from ai;
alter table ai auto_increment = 1;
insert into ai(v) values (4);
alter table ai auto_increment = 1;
insert into ai(v) values (5);
select id from ai order by id;
--   start values beyond int4
create table ai2 (id bigint auto_increment primary key, v int) auto_increment = 3000000000;
insert into ai2(v) values (1);
alter table ai2 auto_increment = 5000000000;
insert into ai2(v) values (2);
select id from ai2 order by id;
--   MODIFY / CHANGE keep, drop or add AUTO_INCREMENT
create table am (id int auto_increment primary key, v int);
insert into am(v) values (1), (2);
alter table am modify id bigint auto_increment;
insert into am(v) values (3);
alter table am change id id2 bigint not null auto_increment;
insert into am(id2, v) values (10, 4);
insert into am(v) values (5);
select id2, v from am order by id2;
alter table am modify id2 bigint not null;
insert into am(v) values (6);
create table an (id int not null primary key, v int);
insert into an values (5, 1), (7, 2);
alter table an modify id int not null auto_increment;
insert into an(v) values (3);
select id, v from an order by id;
--   mysql.setval() only moves the counter forward
select mysql.setval('test.ai_id_seq', 100, true) as r1;
select mysql.setval('test.ai_id_seq', 50, true) as r2;
insert into ai(v) values (6);
select max(id) from ai;

-- 154ce7a: the connection survives a syntax error
selec 1;
select 'alive' as conn;

-- ecd1942: with autocommit=0, COMMIT / ROLLBACK start a new transaction
create table tx (v int);
set autocommit = 0;
insert into tx values (1);
commit;
insert into tx values (2);
rollback;
insert into tx values (3);
commit;
set autocommit = 1;
select v from tx order by v;
