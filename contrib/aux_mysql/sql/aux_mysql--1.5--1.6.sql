-- aux_mysql 1.5 -> 1.6
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.6'" to load this file. \quit

-- aux_mysql's own row triggers (AUTO_INCREMENT counter, ON UPDATE
-- CURRENT_TIMESTAMP) must also fire with FOREIGN_KEY_CHECKS=0, which sets
-- session_replication_role = replica.  New tables get ENABLE ALWAYS when the
-- trigger is created; this covers tables created by earlier versions.
DO $$
DECLARE
    t record;
BEGIN
    FOR t IN SELECT tg.tgrelid::pg_catalog.regclass AS rel, tg.tgname
               FROM pg_catalog.pg_trigger tg
              WHERE tg.tgname IN ('aux_mysql_on_update', 'aux_mysql_auto_increment')
                AND tg.tgenabled = 'O' AND tg.tgparentid = 0
    LOOP
        EXECUTE pg_catalog.format('ALTER TABLE %s ENABLE ALWAYS TRIGGER %I', t.rel, t.tgname);
    END LOOP;
END $$;

-- As in MySQL, new connections take GLOBAL foreign_key_checks.
UPDATE mys_informa_schema.base_variables SET sess_def_val_from_global = true
 WHERE variable_name = 'foreign_key_checks';
