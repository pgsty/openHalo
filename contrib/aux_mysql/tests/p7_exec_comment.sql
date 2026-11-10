-- MySQL executable comments: the text runs when no version is given or the | Copyright (c) 2026 PGSTY PTE. LTD.
-- version is not newer than the reported server version (5.7.32).
select 1 /*! + 1 */ as a, 1 /*!50000 + 1 */ as b, 1 /*!50732 + 1 */ as c,
       1 /*!50733 + 1 */ as d, 1 /*!80016 + 1 */ as e, 1 /*!+1*/ as f;
select 2*/*c*/3 as g, /*!40001 'a*/b' */ as h, /*+ hint */ 5 as j;

-- statements mysqldump wraps in executable comments
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
select @@sql_mode as dump_mode;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
drop view if exists xe_v;
drop table if exists xe_t;
create table xe_t (id int primary key, v decimal(8,2)) /*!40100 DEFAULT CHARSET=utf8mb4 */;
/*!40000 ALTER TABLE `xe_t` DISABLE KEYS */;
insert into xe_t values (1, 1.5), (2, 2.5);
/*!40000 ALTER TABLE `xe_t` ENABLE KEYS */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 DEFINER=`root`@`localhost` SQL SECURITY DEFINER */
/*!50001 VIEW `xe_v` AS select sum(`xe_t`.`v`) AS `total` from `xe_t` */;
select * from xe_v;
drop view xe_v;
drop table xe_t;
