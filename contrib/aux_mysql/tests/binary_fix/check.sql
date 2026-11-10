-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
SET search_path=mysql,pg_catalog;
CREATE SCHEMA review_fix_binary;
CREATE FUNCTION review_fix_binary.assert(ok bool, msg text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN IF ok IS DISTINCT FROM true THEN RAISE EXCEPTION '%',msg; END IF; END $$;
SELECT review_fix_binary.assert(hex('ab'::text::binary_n(4))='61620000', 'BINARY padding');
SELECT review_fix_binary.assert(hex('ab'::text::varbinary_n(4))='6162', 'VARBINARY no padding');
SELECT review_fix_binary.assert(hex('abcdef'::text::binary_n(3))='616263', 'explicit BINARY truncation');
SELECT review_fix_binary.assert(hex('a'::text::binary_n(0))='', 'BINARY zero');
CREATE TABLE review_fix_binary.b (id int PRIMARY KEY, b binary_n(4) UNIQUE, v varbinary_n(3), s text);
INSERT INTO review_fix_binary.b VALUES (1,'ab','xy', E'A\001Bé');
DO $$BEGIN
  BEGIN INSERT INTO review_fix_binary.b(id,b) VALUES(2,'abcde'); RAISE EXCEPTION 'strict overflow accepted';
  EXCEPTION WHEN string_data_right_truncation THEN NULL; END;
  BEGIN INSERT INTO review_fix_binary.b(id,b) VALUES(2,'ab'); RAISE EXCEPTION 'padded unique bypass';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END$$;
SET review_fix.mysql_session=on;
SET review_fix.strict=off;
INSERT INTO review_fix_binary.b VALUES (2,'abcdefgh','abcdef','z');
SELECT review_fix_binary.assert(hex(b)='61626364' AND hex(v)='616263', 'lenient write') FROM review_fix_binary.b WHERE id=2;
SET review_fix.strict=on;
INSERT INTO review_fix_binary.b VALUES (3,'\x41','x','\x41');
SELECT review_fix_binary.assert(hex(b)='5C783431' AND hex(s)='5C783431', 'literal backslash') FROM review_fix_binary.b WHERE id=3;
SET review_fix.mysql_session=off;
SELECT review_fix_binary.assert(hex(s)='410142C3A9' AND octet_length(s)=5 AND char_length(s)=4, 'TEXT control byte and UTF8') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert(hex(left(s,3))='410142' AND hex(right(s,2))='42C3A9', 'character character slicing') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert(hex(substring(s,2,3))='0142C3A9', 'character substring') FROM review_fix_binary.b WHERE id=1;
DO $$DECLARE v text; BEGIN
  BEGIN SELECT '\x410042'::hexlit::text INTO v;
  RAISE EXCEPTION 'PG text accepted NUL';
  EXCEPTION WHEN character_not_in_repertoire THEN NULL; END;
END$$;
SELECT review_fix_binary.assert(hex(concat_pair(concat_one('\x00ff'::hexlit),concat_one('a'::text)))='00FF61', 'binary CONCAT');
SELECT review_fix_binary.assert(pg_typeof(concat_pair(concat_one('\x00ff'::hexlit),concat_one('a'::text)))='bytea'::regtype, 'CONCAT return type');
SELECT review_fix_binary.assert(concat_pair(concat_one('a'::text),concat_one(NULL::bytea)) IS NULL, 'CONCAT NULL');
SELECT review_fix_binary.assert(hex(left('\x00ff'::hexlit,1))='00' AND hex(right('\x00ff'::hexlit,1))='FF', 'binary LEFT RIGHT');
SELECT review_fix_binary.assert(ascii('\xff00'::hexlit)=255 AND char_length('\x00ff'::hexlit)=2, 'binary scalar functions');
SELECT review_fix_binary.assert('\x41'::hexlit + '1'::text = 66, 'hexlit string arithmetic');
CREATE TABLE review_fix_binary.i (v int PRIMARY KEY);
INSERT INTO review_fix_binary.i SELECT generate_series(1,1000);
ANALYZE review_fix_binary.i;
EXPLAIN (COSTS OFF) SELECT * FROM review_fix_binary.i WHERE v='\x41'::hexlit;
DO $$DECLARE p json; BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) SELECT * FROM review_fix_binary.i WHERE v=''\x41''::mysql.hexlit' INTO p;
  PERFORM review_fix_binary.assert(p->0->'Plan'->>'Node Type' IN ('Index Scan','Index Only Scan','Bitmap Heap Scan'), 'integer index not used: '||p::text);
END$$;
SELECT review_fix_binary.assert(9223372036854775807::int8 < '\xffffffffffffffff'::hexlit, 'unsigned fallback');
SELECT review_fix_binary.assert(E'A\001B'::text <> E'A\001C'::text, 'comparison after control byte');
SELECT review_fix_binary.assert('a '::char(2) = 'a'::char(2), 'CHAR padding');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT 0x41+'1'$q$)='66', 'grammar mixed arithmetic');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT '1'+0x41$q$)='66', 'grammar reverse arithmetic');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT 0x41+0x31$q$)='114', 'literal numeric context');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT(0x00ff,'a')$q$,true)='bytea|\x00ff61','grammar CONCAT result type/bytes');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT('a',0x00ff,3,'z'))$q$)='6100FF337A','mixed CONCAT');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT('a',42)$q$,true)='text|a42','text CONCAT unchanged');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(LEFT(0x00ff,1))$q$)='00','grammar LEFT');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IF(1,0x00ff,0x42))$q$)='00FF','grammar IF');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IF(0,0x41,3))$q$)='33','grammar IF numeric branch');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CAST('a' AS BINARY(3)))$q$)='610000','grammar BINARY cast');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IF(0.5,0x41,0x42))$q$)='41','IF floating condition');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IFNULL(NULL,0x00ff))$q$)='00FF','IFNULL NUL');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(TRIM(0x20004120))$q$)='0041','TRIM NUL');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(REPLACE(0x410042,'A','Z'))$q$)='5A0042','REPLACE NUL');
CREATE FUNCTION review_fix_binary.fail() RETURNS text LANGUAGE plpgsql VOLATILE AS $$ BEGIN RAISE EXCEPTION 'dead branch evaluated'; END $$;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IF(1,0x41,review_fix_binary.fail()))$q$)='41','IF lazy branch');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(IFNULL(0x41,review_fix_binary.fail()))$q$)='41','IFNULL lazy branch');
SELECT review_fix_binary.assert(hex(upper(E'a\001b'::text))='410142','UPPER after control byte');
DO $$BEGIN
  BEGIN PERFORM '\x61ff'::hexlit::text; RAISE EXCEPTION 'invalid UTF8 accepted';
  EXCEPTION WHEN character_not_in_repertoire THEN NULL; END;
END$$;
CREATE COLLATION review_fix_binary.ci (provider=icu,locale='und-u-ks-level2',deterministic=false);
CREATE TABLE review_fix_binary.c (s text COLLATE review_fix_binary.ci UNIQUE);
INSERT INTO review_fix_binary.c VALUES (E'a\001B');
DO $$BEGIN
  BEGIN INSERT INTO review_fix_binary.c VALUES (E'A\001b'); RAISE EXCEPTION 'collation unique bypass';
  EXCEPTION WHEN unique_violation THEN NULL; END;
END$$;
SELECT review_fix_binary.assert(pg_catalog.hashtext(E'a\001B'::text COLLATE review_fix_binary.ci)=pg_catalog.hashtext(E'A\001b'::text COLLATE review_fix_binary.ci),'collation hash invariant');
CREATE INDEX text_hash ON review_fix_binary.c USING hash(s);
CREATE INDEX binary_hash ON review_fix_binary.b USING hash(b);
SET enable_seqscan=off;
EXPLAIN (COSTS OFF) SELECT * FROM review_fix_binary.b WHERE b='\x61620000'::hexlit;
DO $$DECLARE p json; BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) SELECT * FROM review_fix_binary.b WHERE b=''\x61620000''::mysql.hexlit' INTO p;
  PERFORM review_fix_binary.assert(p->0->'Plan'->>'Node Type' IN ('Index Scan','Bitmap Heap Scan'), 'binary index not used: '||p::text);
END$$;
RESET enable_seqscan;
SELECT review_fix_binary.assert(count(*)=1,'binary index match') FROM review_fix_binary.b WHERE b='\x61620000'::hexlit;
SELECT review_fix_binary.assert(encode(pg_catalog.textsend(s),'hex')='410142c3a9','PG binary send') FROM review_fix_binary.b WHERE id=1;

SET review_fix.mysql_session=on;
SET review_fix.strict=on;
SET review_fix.ignore=on;
INSERT INTO review_fix_binary.b(id,v) VALUES(9,'abcdef');
SELECT review_fix_binary.assert(hex(v)='616263','IGNORE length coercion') FROM review_fix_binary.b WHERE id=9;
SET review_fix.ignore=off;
SET review_fix.mysql_session=off;

SELECT review_fix_binary.assert(hex(mysql.if(1,s,'x'::text))='410142C3A9','IF on TEXT') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert(hex(mysql.ifnull(NULL::text,s))='410142C3A9','IFNULL on TEXT') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert(hex(mysql.replace(s,'A'::text,'Z'::text))='5A0142C3A9','REPLACE on TEXT') FROM review_fix_binary.b WHERE id=1;
SELECT review_fix_binary.assert('a'::text='a'::text,'new/old character comparison');
