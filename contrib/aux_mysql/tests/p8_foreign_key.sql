-- FOREIGN KEY (issue #2): MySQL foreign keys are enforced and their actions
-- run; FOREIGN_KEY_CHECKS=0 turns both off for the session, as in InnoDB.
drop table if exists fk_item, fk_order, fk_cust, fk_tree;
create table fk_cust (id int primary key, name varchar(20));
create table fk_order (id int primary key, cust int, key (cust),
  constraint fk_order_cust foreign key (cust) references fk_cust (id)
  on delete cascade on update cascade);
create table fk_item (id int primary key, ord int, key (ord),
  foreign key (ord) references fk_order (id) on delete set null);
insert into fk_cust values (1, 'a'), (2, 'b');
insert into fk_order values (10, 1), (20, 2);
insert into fk_item values (100, 10), (200, 20);

-- ON DELETE CASCADE, then SET NULL one level down; ON UPDATE CASCADE
delete from fk_cust where id = 1;
select * from fk_order order by id;
select * from fk_item order by id;
update fk_cust set id = 3 where id = 2;
select * from fk_order order by id;

-- FOREIGN_KEY_CHECKS=0: neither checks nor actions
set foreign_key_checks = 0;
insert into fk_order values (30, 99);
delete from fk_cust where id = 3;
select * from fk_order order by id;
set foreign_key_checks = 1;
select @@foreign_key_checks;

-- mysqldump loads self-referencing rows in key order under checks off
create table fk_tree (id int primary key, parent int, key (parent),
  foreign key (parent) references fk_tree (id));
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
insert into fk_tree values (1, 2), (2, null);
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
select @@foreign_key_checks;
select * from fk_tree order by id;
drop table fk_item, fk_order, fk_cust, fk_tree;
