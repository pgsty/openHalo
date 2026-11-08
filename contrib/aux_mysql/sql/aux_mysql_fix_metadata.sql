-- Hand-written 1.9 objects; also appended by port_aux_sql.py for fresh installs.
-- No CASCADE: an existing dependent view must be handled explicitly at upgrade.
-- Missing-parent/deferred foreign keys can be UNIONed into each public view.
CREATE OR REPLACE FUNCTION mysql.show_system_variable(pg_catalog.text, pg_catalog.bool)
RETURNS pg_catalog.text AS '$libdir/aux_mysql', 'mys_show_system_variable'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED;

-- The historical names are kept because the raw grammar references them.
CREATE OR REPLACE VIEW mys_informa_schema.empty_session_variables AS
SELECT variable_name::varchar(128) AS variable_name,
       mysql.show_system_variable(variable_name, sess_global_type <> 1)::varchar(1024) AS value
FROM mys_informa_schema.base_variables;
CREATE OR REPLACE VIEW mys_informa_schema.empty_global_variables AS
SELECT variable_name::varchar(128) AS variable_name,
       mysql.show_system_variable(variable_name, false)::varchar(1024) AS value
FROM mys_informa_schema.base_variables WHERE sess_global_type IN (0, 1, 3);

DO $fix$
DECLARE v text;
BEGIN
    FOREACH v IN ARRAY ARRAY['table_constraints', 'key_column_usage', 'referential_constraints'] LOOP
        IF EXISTS (SELECT FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
                   WHERE n.nspname='mys_informa_schema' AND c.relname=v AND c.relkind='r') THEN
            EXECUTE pg_catalog.format('DROP TABLE mys_informa_schema.%I', v);
        END IF;
    END LOOP;
END $fix$;

CREATE OR REPLACE VIEW mys_informa_schema.table_constraints AS
SELECT 'def'::varchar(256) AS constraint_catalog,
       n.nspname::varchar(256) AS constraint_schema,
       mysql.index_key_name(i.indexrelid)::varchar(256) AS constraint_name,
       n.nspname::varchar(256) AS table_schema, t.relname::varchar(256) AS table_name,
       (CASE WHEN i.indisprimary THEN 'PRIMARY KEY' ELSE 'UNIQUE' END)::varchar(256) AS constraint_type
FROM pg_catalog.pg_index i
JOIN pg_catalog.pg_class t ON t.oid=i.indrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
WHERE i.indisunique AND i.indisvalid AND i.indisready AND i.indpred IS NULL
  AND n.nspname NOT IN ('pg_catalog','information_schema','mysql','mys_informa_schema','mys_sys','sys')
  AND n.nspname !~ '^pg_(toast|temp)'
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
UNION ALL
SELECT 'def'::varchar(256), n.nspname::varchar(256), c.conname::varchar(256),
       n.nspname::varchar(256), t.relname::varchar(256), 'FOREIGN KEY'::varchar(256)
FROM pg_catalog.pg_constraint c
JOIN pg_catalog.pg_class t ON t.oid=c.conrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
WHERE c.contype='f' AND c.conparentid=0
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
UNION ALL
SELECT 'def'::varchar(256), f.table_schema::varchar(256), f.constraint_name::varchar(256),
       f.table_schema::varchar(256), f.table_name::varchar(256), 'FOREIGN KEY'::varchar(256)
FROM mysql.pending_foreign_keys f;

CREATE OR REPLACE VIEW mys_informa_schema.key_column_usage AS
SELECT 'def'::varchar(256) AS constraint_catalog, n.nspname::varchar(256) AS constraint_schema,
       mysql.index_key_name(i.indexrelid)::varchar(256) AS constraint_name,
       'def'::varchar(256) AS table_catalog, n.nspname::varchar(256) AS table_schema,
       t.relname::varchar(256) AS table_name, k.column_name::varchar(256) AS column_name,
       k.seq::bigint AS ordinal_position, NULL::bigint AS position_in_unique_constraint,
       NULL::varchar(256) AS referenced_table_schema, NULL::varchar(256) AS referenced_table_name,
       NULL::varchar(256) AS referenced_column_name
FROM pg_catalog.pg_index i
JOIN pg_catalog.pg_class t ON t.oid=i.indrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
CROSS JOIN LATERAL mysql.index_key_parts(i.indexrelid) k
WHERE i.indisunique AND i.indisvalid AND i.indisready AND i.indpred IS NULL
  AND n.nspname NOT IN ('pg_catalog','information_schema','mysql','mys_informa_schema','mys_sys','sys')
  AND n.nspname !~ '^pg_(toast|temp)'
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
UNION ALL
SELECT 'def'::varchar(256), n.nspname::varchar(256), c.conname::varchar(256),
       'def'::varchar(256), n.nspname::varchar(256), t.relname::varchar(256), a.attname::varchar(256),
       pos.n::bigint,
       (SELECT x.ord FROM pg_catalog.unnest(i.indkey) WITH ORDINALITY x(attnum,ord)
        WHERE x.attnum=c.confkey[pos.n] AND x.ord<=i.indnkeyatts LIMIT 1)::bigint,
       rn.nspname::varchar(256), rt.relname::varchar(256), ra.attname::varchar(256)
FROM pg_catalog.pg_constraint c
JOIN pg_catalog.pg_class t ON t.oid=c.conrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
JOIN pg_catalog.pg_class rt ON rt.oid=c.confrelid
JOIN pg_catalog.pg_namespace rn ON rn.oid=rt.relnamespace
JOIN pg_catalog.pg_index i ON i.indexrelid=c.conindid
CROSS JOIN LATERAL pg_catalog.generate_subscripts(c.conkey,1) pos(n)
JOIN pg_catalog.pg_attribute a ON a.attrelid=t.oid AND a.attnum=c.conkey[pos.n]
JOIN pg_catalog.pg_attribute ra ON ra.attrelid=rt.oid AND ra.attnum=c.confkey[pos.n]
WHERE c.contype='f' AND c.conparentid=0
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
UNION ALL
SELECT 'def'::varchar(256),f.table_schema::varchar(256),f.constraint_name::varchar(256),
       'def'::varchar(256),f.table_schema::varchar(256),f.table_name::varchar(256),
       f.column_names[pos.n]::varchar(256),pos.n::bigint,pos.n::bigint,
       f.referenced_table_schema::varchar(256),f.referenced_table_name::varchar(256),
       f.referenced_column_names[pos.n]::varchar(256)
FROM mysql.pending_foreign_keys f
CROSS JOIN LATERAL pg_catalog.generate_subscripts(f.column_names,1) pos(n);

CREATE OR REPLACE VIEW mys_informa_schema.referential_constraints AS
SELECT 'def'::varchar(64) AS constraint_catalog, n.nspname::varchar(64) AS constraint_schema,
       c.conname::varchar(64) AS constraint_name, 'def'::varchar(64) AS unique_constraint_catalog,
       rn.nspname::varchar(64) AS unique_constraint_schema,
       mysql.index_key_name(c.conindid)::varchar(64) AS unique_constraint_name,
       (CASE c.confmatchtype WHEN 'f' THEN 'FULL' WHEN 'p' THEN 'PARTIAL' ELSE 'NONE' END)::varchar(64) AS match_option,
       (CASE c.confupdtype WHEN 'a' THEN 'RESTRICT' WHEN 'r' THEN 'RESTRICT' WHEN 'c' THEN 'CASCADE'
                          WHEN 'n' THEN 'SET NULL' WHEN 'd' THEN 'SET DEFAULT' END)::varchar(64) AS update_rule,
       (CASE c.confdeltype WHEN 'a' THEN 'RESTRICT' WHEN 'r' THEN 'RESTRICT' WHEN 'c' THEN 'CASCADE'
                          WHEN 'n' THEN 'SET NULL' WHEN 'd' THEN 'SET DEFAULT' END)::varchar(64) AS delete_rule,
       t.relname::varchar(64) AS table_name, rt.relname::varchar(64) AS referenced_table_name
FROM pg_catalog.pg_constraint c
JOIN pg_catalog.pg_class t ON t.oid=c.conrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
JOIN pg_catalog.pg_class rt ON rt.oid=c.confrelid
JOIN pg_catalog.pg_namespace rn ON rn.oid=rt.relnamespace
WHERE c.contype='f' AND c.conparentid=0
  AND (pg_catalog.pg_has_role(t.relowner, 'USAGE')
       OR pg_catalog.has_table_privilege(t.oid, 'SELECT, INSERT, UPDATE, DELETE, REFERENCES, TRIGGER')
       OR pg_catalog.has_any_column_privilege(t.oid, 'SELECT, INSERT, UPDATE, REFERENCES'))
UNION ALL
SELECT 'def'::varchar(64),f.table_schema::varchar(64),f.constraint_name::varchar(64),
       'def'::varchar(64),f.referenced_table_schema::varchar(64),NULL::varchar(64),
       f.match_option::varchar(64),f.update_rule::varchar(64),f.delete_rule::varchar(64),
       f.table_name::varchar(64),f.referenced_table_name::varchar(64)
FROM mysql.pending_foreign_keys f;

GRANT SELECT ON mys_informa_schema.table_constraints,
    mys_informa_schema.key_column_usage, mys_informa_schema.referential_constraints TO PUBLIC;
