-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- aux_mysql 1.8 -> 1.9: data integrity and schema export corrections.
-- The Makefile appends the shared SQL fragments in dependency order.
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.9'" to load this file. \quit
SET LOCAL search_path = mysql, pg_catalog, public;
SET LOCAL check_function_bodies = off;

-- Released 1.5 did not yet expose session diagnostics through SQL.
CREATE OR REPLACE FUNCTION mysql.show_diagnostics(boolean)
    RETURNS TABLE("Level" text, "Code" integer, "Message" text)
    AS '$libdir/aux_mysql', 'mys_show_diagnostics'
    LANGUAGE C STRICT;
