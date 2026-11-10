-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Read-only on OpenHalo14. Inventory all databases separately using psql.
-- TIME entries need a value-range review; this script does not scan user data.
WITH RECURSIVE type_chain AS (
    SELECT oid AS original_oid, oid, typbasetype, typname, typtype FROM pg_type
    UNION ALL
    SELECT chain.original_oid, base.oid, base.typbasetype, base.typname, base.typtype
    FROM type_chain chain JOIN pg_type base ON base.oid=chain.typbasetype
), type_flags AS (
    SELECT original_oid,
           bool_or(typtype='e' OR typname IN ('enum','set') OR EXISTS (
               SELECT 1 FROM pg_constraint con
               WHERE con.contypid=type_chain.oid AND con.conname='halo_set_check'
           )) AS enum_or_set,
           bool_or(typname='time') AS duration,
           bool_or(typname IN ('float4','float8')) AS floating
    FROM type_chain GROUP BY original_oid
)
SELECT n.nspname AS schema_name, c.relname AS table_name, a.attname AS column_name,
       format_type(a.atttypid, a.atttypmod) AS old_type,
       CASE WHEN t.enum_or_set THEN
              'REVIEW: ENUM/SET requires explicit logical type/constraint conversion'
            WHEN t.duration THEN
              'REVIEW: negative or >24-hour durations need interval or an application duration representation'
            WHEN t.floating THEN
              'REVIEW: FLOAT now uses float4; preserve old precision with DOUBLE if required'
       END AS migration_action
FROM pg_attribute a JOIN pg_class c ON a.attrelid=c.oid
JOIN pg_namespace n ON n.oid=c.relnamespace JOIN type_flags t ON t.original_oid=a.atttypid
WHERE a.attnum>0 AND NOT a.attisdropped AND c.relkind IN ('r','p')
  AND n.nspname NOT IN ('pg_catalog','information_schema','mysql','mys_informa_schema','mys_sys','performance_schema')
  AND (t.enum_or_set OR t.duration OR t.floating)
ORDER BY 1,2,a.attnum;
