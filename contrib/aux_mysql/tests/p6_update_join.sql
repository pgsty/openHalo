-- Multi-table UPDATE (issue #1): the JOIN form must not crash the backend, | Copyright (c) 2026 PGSTY PTE. LTD.
-- and the comma form must match whole table names / aliases.
drop table if exists uj_t, uj_u, uj_o, uj_oi;
create table uj_t (id int primary key, v int, w int);
create table uj_u (id int primary key, v int);
insert into uj_t values (1, 1, 0), (2, 2, 0), (3, 3, 0);
insert into uj_u values (1, 10), (3, 30);

-- the reported crash: qualified SET target
update uj_t join uj_u on uj_t.id = uj_u.id set uj_t.v = uj_u.v;
select * from uj_t order by id;

-- aliases, INNER, several assignments, extra WHERE
update uj_t as a inner join uj_u b on a.id = b.id set a.v = b.v + 1, a.w = 7 where b.v > 10;
select * from uj_t order by id;

-- an unqualified target column belongs to the left table
update uj_t join uj_u on uj_t.id = uj_u.id set w = w + 1;
select * from uj_t order by id;

-- comma form; aliases sharing a prefix ("o", "oi") used to be rejected
create table uj_o (id int primary key, total int);
create table uj_oi (id int primary key, amount int);
insert into uj_o values (1, 0), (2, 0);
insert into uj_oi values (1, 5), (2, 6);
update uj_o o, uj_oi oi set o.total = oi.amount where o.id = oi.id and oi.amount > 5;
select * from uj_o order by id;
update uj_o, uj_oi set uj_o.total = uj_oi.amount + 100 where uj_o.id = uj_oi.id;
select * from uj_o order by id;
-- qualifiers compare case-insensitively (lower_case_table_names=1; MySQL
-- with lower_case_table_names=0 rejects A.w); IGNORE is accepted with JOIN
update uj_t as a join uj_u b on a.id = b.id set A.w = 9;
update ignore uj_t join uj_u on uj_t.id = uj_u.id set uj_t.v = uj_t.v + 1;
select * from uj_t order by id;
drop table uj_t, uj_u, uj_o, uj_oi;
