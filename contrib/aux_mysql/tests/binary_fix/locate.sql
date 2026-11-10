-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
-- Exercise the actual index owner's PAD SPACE collator and weight helper.
-- This file runs only in the private cluster created by run.sh.
SET search_path=mysql,pg_catalog;
-- Load the private module and its locale hooks before planner constant folding.
SELECT mysql.hex(''::text);
CREATE COLLATION mysql.utf8mb4_general_ci
    (provider=icu,locale='und-u-ks-level1',deterministic=false);
CREATE COLLATION mysql.utf8mb4_bin
    (provider=icu,locale='und',deterministic=false);
SELECT review_fix_binary.assert('a'::text COLLATE mysql.utf8mb4_general_ci = 'a '::text,'fixture uses PAD SPACE general_ci');
SELECT review_fix_binary.assert('a'::text COLLATE mysql.utf8mb4_bin = 'a '::text,'fixture uses PAD SPACE bin');

CREATE TABLE review_fix_binary.locate_input(
    ci text COLLATE mysql.utf8mb4_general_ci,
    bin text COLLATE mysql.utf8mb4_bin);
INSERT INTO review_fix_binary.locate_input VALUES ('a','a');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT LOCATE('a ',ci) FROM review_fix_binary.locate_input$q$)='0','LOCATE never pads needle');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT LOCATE('a ',bin) FROM review_fix_binary.locate_input$q$)='0','bin LOCATE never pads needle');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT INSTR(ci,'a ') FROM review_fix_binary.locate_input$q$)='0','INSTR never pads needle');

DO $$DECLARE coll text; actual int; r record; BEGIN
  FOREACH coll IN ARRAY ARRAY['utf8mb4_general_ci','utf8mb4_bin'] LOOP
    FOR r IN SELECT * FROM (VALUES
      ('a '::text,'a '::text,1,1),(' '::text,'a '::text,1,2),
      ('a '::text,'a'::text,1,0),('a  '::text,'a '::text,1,0),
      ('a a'::text,'a a a'::text,2,3),
      ('ab'::text,'aaaab'::text,1,4),('ababac'::text,'ababababac'::text,1,5),
      ('ab'::text,'aaab'::text,4,0),
      (''::text,'é界'::text,3,3),(''::text,'é界'::text,4,0),
      ('a'::text,'abc'::text,0,0),('a'::text,''::text,1,0)
    ) v(needle,haystack,start,expected) LOOP
      EXECUTE format('SELECT mysql.locate($1::text,$2::text COLLATE mysql.%I,$3)',coll)
        INTO actual USING r.needle,r.haystack,r.start;
      PERFORM review_fix_binary.assert(actual=r.expected,format('%s %s/%s start %s: %s',coll,r.needle,r.haystack,r.start,actual));
    END LOOP;
    EXECUTE format('SELECT mysql.locate($1::text,$2::text COLLATE mysql.%I,2)',coll)
      INTO actual USING E'\001a',E'é\001a\001a';
    PERFORM review_fix_binary.assert(actual=2,'control-byte needle and multibyte start');
    EXECUTE format('SELECT mysql.locate($1::text,$2::text COLLATE mysql.%I,3)',coll)
      INTO actual USING E'\001a',E'é\001a\001a';
    PERFORM review_fix_binary.assert(actual=4,'control-byte search after earlier match');
  END LOOP;
END $$;
SELECT review_fix_binary.assert(locate('é'::text,E'x\001É'::text COLLATE mysql.utf8mb4_general_ci)=3,'general_ci accent/case weights after control byte');
SELECT review_fix_binary.assert(locate('é'::text,E'x\001É'::text COLLATE mysql.utf8mb4_bin)=0,'bin preserves accent/case');
SELECT review_fix_binary.assert(locate('s'::text,'ß'::text COLLATE mysql.utf8mb4_general_ci)=0,'MySQL requires equal total byte lengths');
SELECT review_fix_binary.assert(locate('É'::text,'e'::text COLLATE mysql.utf8mb4_general_ci)=0,'same weight with different byte length is not a substring');
SELECT review_fix_binary.assert(locate('éA'::text,'xeÁ'::text COLLATE mysql.utf8mb4_general_ci)=2,'total byte length, not per-character width');
SELECT review_fix_binary.assert(locate('é'::text,'eeé'::text COLLATE mysql.utf8mb4_general_ci)=3,'skip wrong-width matches without losing later match');
SELECT review_fix_binary.assert(locate('ée'::text,'eeé'::text COLLATE mysql.utf8mb4_general_ci)=2,'wrong-width full match preserves overlapping candidate');
SELECT review_fix_binary.assert(locate('ss'::text,'ß'::text COLLATE mysql.utf8mb4_general_ci)=0,'general_ci has no expansion');
SELECT review_fix_binary.assert(locate('😀'::text,'x😁'::text COLLATE mysql.utf8mb4_general_ci)=2,'general_ci supplementary replacement weight');
SELECT review_fix_binary.assert(locate('😀'::text,'x😁'::text COLLATE mysql.utf8mb4_bin)=0,'bin distinguishes supplementary characters');
SELECT review_fix_binary.assert(locate('\x65'::hexlit,'É'::text COLLATE mysql.utf8mb4_general_ci)=0,'binary operand bypasses character weights');

-- Generic ICU still supports expansion/contraction and significant spaces.
CREATE COLLATION review_fix_binary.locate_icu (provider=icu,locale='und-u-ks-level1',deterministic=false);
SELECT review_fix_binary.assert(locate('ss'::text,'xßz'::text COLLATE review_fix_binary.locate_icu)=2,'generic ICU expansion survives');
SELECT review_fix_binary.assert(locate('a '::text,'a'::text COLLATE review_fix_binary.locate_icu)=0,'generic ICU keeps trailing space');

-- Both absent and highly repetitive needles must complete within a generous
-- bound. This catches the former cubic search without a timing-ratio flake.
SET statement_timeout='5s';
\timing on
DO $$DECLARE coll text; actual int; BEGIN
  FOREACH coll IN ARRAY ARRAY['utf8mb4_general_ci','utf8mb4_bin'] LOOP
    EXECUTE format('SELECT mysql.locate(''z'',repeat(''a'',65535)::text COLLATE mysql.%I)',coll) INTO actual;
    PERFORM review_fix_binary.assert(actual=0,'long absent needle');
    EXECUTE format('SELECT mysql.locate((repeat(''a'',32767)||''b'')::text,repeat(''a'',65535)::text COLLATE mysql.%I)',coll) INTO actual;
    PERFORM review_fix_binary.assert(actual=0,'long repeated absent needle');
    EXECUTE format('SELECT mysql.locate((repeat(''a'',32767)||''b'')::text,(repeat(''a'',65534)||''b'')::text COLLATE mysql.%I)',coll) INTO actual;
    PERFORM review_fix_binary.assert(actual=32768,'long repeated matched needle');
  END LOOP;
END $$;
\timing off
RESET statement_timeout;
DROP TABLE review_fix_binary.locate_input;
DROP COLLATION mysql.utf8mb4_general_ci;
DROP COLLATION mysql.utf8mb4_bin;
