-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- PostgreSQL connection, table owner/superuser, one reviewed schema at a time:
-- psql -X -v review_schema=app -f tools/review_fix_migration.sql
-- Add -v check_fk=1 to execute orphan counts (read-only, may scan large tables).
-- After repairing orphans, add -v enforce_fk=1 / -v validate_fk=1 to execute
-- the corresponding transactional repair. Enforcement validates existing rows;
-- failure rolls back, never marks an orphaned FK as successfully repaired.
-- Optional -v generated_manifest=/absolute/path/manifest.sql includes a trusted
-- SQL file inserting source definitions into pg_temp.review_fix_generated_source.
\set ON_ERROR_STOP on
\if :{?review_schema}
\else
  \echo 'Required: -v review_schema=the_application_schema'
  \quit
\endif
\if :{?check_fk}
\else
  \set check_fk 0
\endif
\if :{?enforce_fk}
\else
  \set enforce_fk 0
\endif
\if :{?validate_fk}
\else
  \set validate_fk 0
\endif
SET search_path=pg_catalog;

-- Old binary domains carry no declared length; native TEXT carries no history
-- proving whether the column came from MySQL. Compare with original DDL.
SELECT a.attrelid::regclass AS table_name,a.attname,
       format_type(a.atttypid,a.atttypmod) AS catalog_type,
       CASE WHEN tn.nspname='mysql' THEN 'SOURCE DDL REQUIRED: legacy binary length was discarded'
            ELSE 'VERIFY: native PG character storage rejects NUL' END AS migration_check
FROM pg_attribute a JOIN pg_class t ON t.oid=a.attrelid
JOIN pg_namespace n ON n.oid=t.relnamespace
JOIN pg_type ty ON ty.oid=a.atttypid JOIN pg_namespace tn ON tn.oid=ty.typnamespace
WHERE n.nspname=:'review_schema' AND a.attnum>0 AND NOT a.attisdropped
  AND t.relkind IN ('r','p')
  AND ((tn.nspname='mysql' AND ty.typname IN ('binary','varbinary'))
       OR (tn.nspname='pg_catalog' AND ty.typname IN ('text','varchar','bpchar')))
ORDER BY a.attrelid,a.attnum;

-- A discarded short/PRIMARY prefix cannot be inferred from a whole-column
-- index. Inventory every key; do not guess which ordinary btrees need repair.
SELECT i.indrelid::regclass AS table_name,i.indexrelid::regclass AS index_name,
       am.amname,i.indisunique,i.indisprimary,i.indisvalid,i.indisready,
       pg_get_indexdef(i.indexrelid) AS definition,
       CASE WHEN am.amname='mysql_prefix' THEN '1.9 exact prefix index'
            ELSE 'COMPARE WITH SOURCE: prefix lengths, old MD5/hash keys and collation rules' END AS migration_check
FROM pg_index i JOIN pg_class t ON t.oid=i.indrelid
JOIN pg_namespace n ON n.oid=t.relnamespace
JOIN pg_class idx ON idx.oid=i.indexrelid JOIN pg_am am ON am.oid=idx.relam
WHERE n.nspname=:'review_schema' ORDER BY i.indrelid,i.indexrelid;

-- Inventory distinguishes enforcement (new writes/actions) from validation
-- (all preexisting rows). Metadata visibility is not evidence of either.
SELECT c.oid, c.conrelid::regclass AS child, c.conname,
       c.confrelid::regclass AS parent, c.conenforced, c.convalidated,
       pg_get_constraintdef(c.oid) AS definition,
       obj_description(c.oid,'pg_constraint') AS comment
FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid
JOIN pg_namespace n ON n.oid=t.relnamespace
WHERE c.contype='f' AND c.conparentid=0 AND n.nspname=:'review_schema'
ORDER BY c.conrelid::regclass::text,c.conname;

CREATE TEMP VIEW review_fix_fk_probe AS
SELECT c.oid,
       format('SELECT %L AS foreign_key, count(*) AS orphan_rows FROM %s ch WHERE (%s AND NOT EXISTS (SELECT FROM %s pa WHERE %s))%s',
              format('%s.%I',c.conrelid::regclass,c.conname), c.conrelid::regclass,
              k.all_nonnull,c.confrelid::regclass,k.equalities,
              CASE WHEN c.confmatchtype='f' THEN format(' OR (NOT (%s) AND NOT (%s))',k.all_nonnull,k.all_null) ELSE '' END) AS probe_sql
FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid
JOIN pg_namespace ns ON ns.oid=t.relnamespace
CROSS JOIN LATERAL (
    SELECT string_agg(format('ch.%I IS NOT NULL',a.attname),' AND ' ORDER BY p.n) AS all_nonnull,
           string_agg(format('ch.%I IS NULL',a.attname),' AND ' ORDER BY p.n) AS all_null,
           string_agg(format('pa.%I OPERATOR(%I.%s) ch.%I',ra.attname,onsp.nspname,op.oprname,a.attname),
                      ' AND ' ORDER BY p.n) AS equalities
    FROM generate_subscripts(c.conkey,1) p(n)
    JOIN pg_attribute a ON a.attrelid=c.conrelid AND a.attnum=c.conkey[p.n]
    JOIN pg_attribute ra ON ra.attrelid=c.confrelid AND ra.attnum=c.confkey[p.n]
    JOIN pg_operator op ON op.oid=c.conpfeqop[p.n]
    JOIN pg_namespace onsp ON onsp.oid=op.oprnamespace
) k
WHERE c.contype='f' AND c.conparentid=0 AND ns.nspname=:'review_schema';
SELECT * FROM review_fix_fk_probe ORDER BY oid;
\if :check_fk
  SELECT probe_sql FROM review_fix_fk_probe ORDER BY oid \gexec
\endif

-- PG18 changes enforcement in place: retain OID, comments and dependencies.
-- Unlike dropping/readding NOT VALID, this does not leave unvalidated orphans.
\if :enforce_fk
  BEGIN;
  SELECT format('ALTER TABLE %s ALTER CONSTRAINT %I ENFORCED',c.conrelid::regclass,c.conname)
  FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid
  JOIN pg_namespace n ON n.oid=t.relnamespace
  WHERE c.contype='f' AND c.conparentid=0 AND NOT c.conenforced AND n.nspname=:'review_schema'
  ORDER BY c.conrelid,c.oid \gexec
  COMMIT;
\endif
\if :validate_fk
  BEGIN;
  SELECT format('ALTER TABLE %s VALIDATE CONSTRAINT %I',c.conrelid::regclass,c.conname)
  FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid
  JOIN pg_namespace n ON n.oid=t.relnamespace
  WHERE c.contype='f' AND c.conparentid=0 AND c.conenforced AND NOT c.convalidated AND n.nspname=:'review_schema'
  ORDER BY c.conrelid,c.oid \gexec
  COMMIT;
\endif

CREATE TEMP TABLE IF NOT EXISTS review_fix_generated_source(
    table_schema text NOT NULL, table_name text NOT NULL, column_name text NOT NULL,
    type_sql text NOT NULL, expression_sql text, kind text NOT NULL CHECK(kind IN ('STORED','VIRTUAL')),
    attributes_sql text NOT NULL DEFAULT '', provenance text NOT NULL,
    PRIMARY KEY(table_schema,table_name,column_name)
);
\if :{?generated_manifest}
  \i :generated_manifest
\endif

-- Ordinary columns cannot reveal whether an old parser discarded AS(expr).
-- Recover their definitions from the original MySQL SHOW CREATE/dump/migrations.
-- No manifest, no claim of completeness, and no automatic expression inference.
SELECT count(*) AS sourced_generated_definitions,
       CASE WHEN count(*)=0 THEN 'BLOCKED: no source manifest; discarded expressions are not discoverable from this catalog'
            ELSE 'Compare every expression/type/attribute with the authoritative source before executing repair_sql' END AS status
FROM review_fix_generated_source WHERE table_schema=:'review_schema';
SELECT e.*, a.attgenerated AS catalog_generated,
       pg_get_expr(d.adbin,d.adrelid) AS catalog_expression_or_default,
       CASE WHEN e.expression_sql IS NULL OR e.expression_sql='' THEN 'BLOCKED: expression missing'
            WHEN t.oid IS NULL OR a.attnum IS NULL THEN 'BLOCKED: table/column missing'
            WHEN a.attgenerated='' THEN 'REBUILD REQUIRED: ordinary column; old default is not proof of generation'
            ELSE 'VERIFY: expression exists; rebuild to install current coercion semantics' END AS status,
       CASE WHEN t.oid IS NOT NULL AND a.attnum IS NOT NULL AND coalesce(e.expression_sql,'')<>'' THEN
            format('ALTER TABLE `%s`.`%s` MODIFY `%s` %s GENERATED ALWAYS AS (%s) %s %s;',
                   replace(e.table_schema,'`','``'),replace(e.table_name,'`','``'),replace(e.column_name,'`','``'),
                   e.type_sql,e.expression_sql,e.kind,e.attributes_sql) END AS repair_sql
FROM review_fix_generated_source e
LEFT JOIN pg_namespace n ON n.nspname=e.table_schema
LEFT JOIN pg_class t ON t.relnamespace=n.oid AND t.relname=e.table_name
LEFT JOIN pg_attribute a ON a.attrelid=t.oid AND a.attname=e.column_name AND NOT a.attisdropped
LEFT JOIN pg_attrdef d ON d.adrelid=t.oid AND d.adnum=a.attnum
WHERE e.table_schema=:'review_schema' ORDER BY e.table_name,a.attnum;

-- Snapshot every current generated expression, including those absent from the
-- manifest. Existing values are not evidence that old coercions were correct.
SELECT a.attrelid::regclass AS table_name,a.attname,a.attgenerated,
       format_type(a.atttypid,a.atttypmod) AS type,pg_get_expr(d.adbin,d.adrelid) AS expression
FROM pg_attribute a JOIN pg_class t ON t.oid=a.attrelid JOIN pg_namespace n ON n.oid=t.relnamespace
LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
WHERE n.nspname=:'review_schema' AND a.attgenerated<>'' AND NOT a.attisdropped
ORDER BY a.attrelid,a.attnum;

-- Older IMMUTABLE wrappers could hide random/time/session or external reads.
-- Print their bodies for source review; a volatility flag is not proof of
-- determinism, and an already discarded/folded expression cannot be recovered.
SELECT a.attrelid::regclass AS table_name,a.attname,
       p.oid::regprocedure AS generator,pg_get_functiondef(p.oid) AS definition
FROM pg_attribute a JOIN pg_class t ON t.oid=a.attrelid
JOIN pg_namespace n ON n.oid=t.relnamespace
JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
JOIN pg_depend dep ON dep.classid='pg_attrdef'::regclass AND dep.objid=d.oid
                 AND dep.refclassid='pg_proc'::regclass
JOIN pg_proc p ON p.oid=dep.refobjid AND p.proname ~ '^aux_mysql_gen_'
WHERE n.nspname=:'review_schema' AND a.attgenerated<>'' AND NOT a.attisdropped
ORDER BY a.attrelid,a.attnum,p.oid;

SELECT c.conrelid::regclass AS child,c.conname,c.conenforced,c.convalidated
FROM pg_constraint c JOIN pg_class t ON t.oid=c.conrelid JOIN pg_namespace n ON n.oid=t.relnamespace
WHERE n.nspname=:'review_schema' AND c.contype='f' AND c.conparentid=0 ORDER BY c.conrelid,c.conname;
