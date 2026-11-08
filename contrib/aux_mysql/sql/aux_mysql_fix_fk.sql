-- Handwritten 1.9 pending-FK objects. Include before aux_mysql_fix_metadata.sql
-- and before the newly generated aux_mysql_fix_show.sql.
CREATE OR REPLACE FUNCTION mysql.pending_fk_guard()
RETURNS trigger AS '$libdir/aux_mysql','mys_pending_fk_guard'
LANGUAGE C;

-- The guard's argument belongs to the child table and is dumped POST_DATA.
-- This preserves historical unchecked rows during normal pg_dump/restore.
-- OIDs here are resolved at query time, never saved in dump metadata.
CREATE OR REPLACE VIEW mysql.pending_foreign_keys AS
SELECT t.oid AS child_oid, n.nspname::text AS table_schema, t.relname::text AS table_name,
       k.value->>'name' AS constraint_name,
       k.value->>'schema' AS referenced_table_schema, k.value->>'table' AS referenced_table_name,
       ARRAY(SELECT pg_catalog.jsonb_array_elements_text(k.value->'columns')) AS column_names,
       ARRAY(SELECT pg_catalog.jsonb_array_elements_text(k.value->'refcolumns')) AS referenced_column_names,
       CASE k.value->>'update' WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL'
            WHEN 'd' THEN 'SET DEFAULT' ELSE 'RESTRICT' END AS update_rule,
       CASE k.value->>'delete' WHEN 'c' THEN 'CASCADE' WHEN 'n' THEN 'SET NULL'
            WHEN 'd' THEN 'SET DEFAULT' ELSE 'RESTRICT' END AS delete_rule,
       CASE k.value->>'match' WHEN 'f' THEN 'FULL' WHEN 'p' THEN 'PARTIAL' ELSE 'NONE' END AS match_option
FROM pg_catalog.pg_trigger tr
JOIN pg_catalog.pg_class t ON t.oid=tr.tgrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
CROSS JOIN LATERAL pg_catalog.jsonb_array_elements(
    (pg_catalog.convert_from(pg_catalog.substr(tr.tgargs,1,pg_catalog.octet_length(tr.tgargs)-1),
                             pg_catalog.getdatabaseencoding())::jsonb)->'keys') k(value)
WHERE tr.tgfoid='mysql.pending_fk_guard()'::regprocedure AND tr.tgnargs=1 AND tr.tgparentid=0
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'));

CREATE OR REPLACE FUNCTION mysql.pending_fk_definitions(pg_catalog.oid)
RETURNS SETOF pg_catalog.text LANGUAGE sql STABLE SET search_path=pg_catalog AS $$
SELECT 'CONSTRAINT `' || replace(f.constraint_name,'`','``') || '` FOREIGN KEY (' ||
       (SELECT string_agg('`'||replace(col,'`','``')||'`',', ' ORDER BY ord)
        FROM unnest(f.column_names) WITH ORDINALITY x(col,ord)) || ') REFERENCES `' ||
       replace(f.referenced_table_schema,'`','``') || '`.`' ||
       replace(f.referenced_table_name,'`','``') || '`' ||
       CASE WHEN cardinality(f.referenced_column_names)>0 THEN ' (' ||
            (SELECT string_agg('`'||replace(col,'`','``')||'`',', ' ORDER BY ord)
             FROM unnest(f.referenced_column_names) WITH ORDINALITY x(col,ord)) || ')'
            ELSE '' END || ' ON DELETE ' || f.delete_rule || ' ON UPDATE ' || f.update_rule
FROM mysql.pending_foreign_keys f WHERE f.child_oid=$1 ORDER BY f.constraint_name
$$;

GRANT SELECT ON mysql.pending_foreign_keys TO PUBLIC;
