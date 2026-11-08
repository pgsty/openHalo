-- aux_mysql 1.8 -> 1.9: data integrity and schema export corrections.
-- The Makefile appends the shared SQL fragments in dependency order.
\echo Use "ALTER EXTENSION aux_mysql UPDATE TO '1.9'" to load this file. \quit
SET LOCAL search_path = mysql, pg_catalog, public;
SET LOCAL check_function_bodies = off;
