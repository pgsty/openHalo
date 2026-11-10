-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
\pset tuples_only on
create extension pg_cron;
select 'scheduled', cron.schedule('nightly', '0 3 * * *', 'select 1') > 0;
select 'jobs', count(*) from cron.job;
