-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Standalone-only bytea-layout fixtures for the binary worker interface.
-- Not extension SQL; never run against a shared or integrated database.
DO $fixture$
DECLARE
    n text; cmp text; pair text[]; operation text; suffix text;
BEGIN
    FOREACH n IN ARRAY ARRAY['binary_n','varbinary_n'] LOOP
        EXECUTE format('CREATE TYPE mysql.%I',n);
        EXECUTE format('CREATE FUNCTION mysql.%I(cstring) RETURNS mysql.%I AS ''byteain'' LANGUAGE internal IMMUTABLE STRICT',n||'_in',n);
        EXECUTE format('CREATE FUNCTION mysql.%I(mysql.%I) RETURNS cstring AS ''byteaout'' LANGUAGE internal IMMUTABLE STRICT',n||'_out',n);
        EXECUTE format('CREATE TYPE mysql.%I(INPUT=mysql.%I,OUTPUT=mysql.%I,INTERNALLENGTH=variable,ALIGNMENT=int4,STORAGE=extended,COLLATABLE=%s)',
            n,n||'_in',n||'_out','false');
        EXECUTE format('CREATE CAST (bytea AS mysql.%I) WITHOUT FUNCTION AS ASSIGNMENT',n);
        EXECUTE format('CREATE CAST (mysql.%I AS bytea) WITHOUT FUNCTION AS ASSIGNMENT',n);
        cmp := 'byteacmp';
        EXECUTE format('CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS int4 AS %L LANGUAGE internal IMMUTABLE STRICT',n||'_cmp',n,n,cmp);
        FOREACH pair SLICE 1 IN ARRAY ARRAY[['lt','<'],['le','<='],['eq','='],['ge','>='],['gt','>']] LOOP
            suffix := pair[1]; operation := pair[2];
            EXECUTE format('CREATE FUNCTION mysql.%I(mysql.%I,mysql.%I) RETURNS bool LANGUAGE sql IMMUTABLE STRICT SET search_path=pg_catalog AS %L',
                n||'_'||suffix,n,n,format('SELECT mysql.%I($1,$2) %s 0',n||'_cmp',operation));
            EXECUTE format('CREATE OPERATOR mysql.%s (LEFTARG=mysql.%I,RIGHTARG=mysql.%I,FUNCTION=mysql.%I)',operation,n,n,n||'_'||suffix);
        END LOOP;
        EXECUTE format('CREATE OPERATOR CLASS mysql.%I DEFAULT FOR TYPE mysql.%I USING btree AS '
            'OPERATOR 1 mysql.<, OPERATOR 2 mysql.<=, OPERATOR 3 mysql.=, OPERATOR 4 mysql.>=, OPERATOR 5 mysql.>, FUNCTION 1 mysql.%I(mysql.%I,mysql.%I)',
            n||'_ops',n,n||'_cmp',n,n);
    END LOOP;
END $fixture$;
