-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
SET search_path=mysql,pg_catalog;
SELECT review_fix_binary.assert(hex(s)='410142C3A9', 'pg_dump restore lost character bytes') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert(hex(b)='61620000', 'pg_dump restore lost BINARY padding') FROM review_fix_binary.b WHERE id=1;
SELECT 'binary fix checks and pg_dump roundtrip passed';
DROP SCHEMA review_fix_binary CASCADE;
