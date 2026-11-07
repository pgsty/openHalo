\pset tuples_only on
create extension pg_cron;
select 'scheduled', cron.schedule('nightly', '0 3 * * *', 'select 1') > 0;
select 'jobs', count(*) from cron.job;
