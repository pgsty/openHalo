-- MySQL transaction rules: autocommit = 0, implicit commits, multi-statement autocommit | Copyright (c) 2026 PGSTY PTE. LTD.
drop table if exists tx;
create table tx (id int primary key, note varchar(32));

-- autocommit = 0 starts a transaction lazily; ROLLBACK undoes it
set autocommit = 0;
select @@autocommit;
insert into tx values (1, 'rolled back');
rollback;
select 'after rollback', count(*) from tx;

-- COMMIT makes it permanent
insert into tx values (1, 'committed');
commit;
rollback;
select 'after commit', count(*) from tx;

-- BEGIN implicitly commits the open transaction
insert into tx values (2, 'implicit commit by BEGIN');
begin;
rollback;
select 'after begin+rollback', count(*) from tx;

-- DDL implicitly commits (before and after)
insert into tx values (3, 'implicit commit by DDL');
create table tx_ddl (id int);
rollback;
select 'after ddl+rollback', count(*) from tx;
select 'ddl survived', count(*) from tx_ddl;
drop table tx_ddl;

-- SET autocommit = 1 commits
insert into tx values (4, 'commit by set autocommit=1');
set autocommit = 1;
rollback;
select 'after set autocommit=1', count(*) from tx;

-- SET autocommit = FALSE / ON forms
set session autocommit = false;
select @@autocommit;
set autocommit = on;
select @@autocommit;

-- autocommit = 1: each statement of a multi-statement query commits on its own
DELIMITER //
insert into tx values (5, 'kept'); insert into tx values (1, 'duplicate'); insert into tx values (6, 'not run') //
DELIMITER ;
select 'multi-statement', group_concat(id order by id) from tx;

-- statement-level rollback: the failing INSERT is undone, the transaction goes on,
-- and BEGIN commits it (MySQL keeps 7)
begin;
insert into tx values (7, 'in failed txn');
insert into tx values (1, 'duplicate');
begin;
insert into tx values (8, 'new txn');
commit;
select 'after error+begin', group_concat(id order by id) from tx;

-- also for syntax errors and unknown tables
begin;
insert into tx values (9, 'before errors');
selec 1;
select * from no_such_table;
insert into tx values (10, 'after errors');
commit;
select 'after syntax error', group_concat(id order by id) from tx;

-- SET does not start a transaction under autocommit = 0
set autocommit = 0;
set @a = 1;
begin;
commit;
set autocommit = 1;

-- COMMIT of a transaction PostgreSQL had to abort is an error, not an OK
select set_config('mysql.statement_rollback', 'off', false) as statement_rollback;
begin;
insert into tx values (1, 'duplicate');
commit;
select set_config('mysql.statement_rollback', 'on', false) as statement_rollback;
drop table tx;
