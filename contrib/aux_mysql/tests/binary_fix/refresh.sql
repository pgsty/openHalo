-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Run ONLY in run.sh's private test cluster, never in the integrated/shared
-- database. A transactional rename simulates the 1.9 type appearing on an
-- existing connection, exercising real pg_type invalidations in that backend.
SET search_path=mysql,pg_catalog;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CAST('a' AS BINARY(3)))$q$)='610000','1.9 initially available');
BEGIN;
ALTER TYPE mysql.binary_n RENAME TO review_fix_saved_binary_n;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CAST('a' AS BINARY(3)))$q$)='61','pre-upgrade CAST keeps bytea');
ALTER TYPE mysql.review_fix_saved_binary_n RENAME TO binary_n;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CAST('a' AS BINARY(3)))$q$)='610000','existing connection adopted 1.9');
ROLLBACK;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CAST('a' AS BINARY(3)))$q$)='610000','rollback invalidation refreshed');
