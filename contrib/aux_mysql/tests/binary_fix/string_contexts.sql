-- Minimum incremental coverage: actual regenerated MySQL grammar, typed
-- return values, NUL/invalid-UTF8 bytes, lazy ELT, and real advisory locks.
SET search_path=mysql,pg_catalog;
-- B03 must execute a btree lookup: a competing hash index hid the missing
-- cross-type comparator in the earlier test. Force index paths, then assert
-- their result as well as the plan. These tables have only primary-key btrees.
CREATE TABLE review_fix_binary.hexkeys(id varbinary_n(16) PRIMARY KEY, n int);
CREATE TABLE review_fix_binary.fixedkeys(id binary_n(4) PRIMARY KEY, n int);
INSERT INTO review_fix_binary.hexkeys VALUES ('\x41000000',1),('\x410000ff',2),('\x42000000',3);
INSERT INTO review_fix_binary.fixedkeys SELECT id,n FROM review_fix_binary.hexkeys;
SET enable_seqscan=off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT n FROM review_fix_binary.hexkeys WHERE id='\x410000ff'::hexlit;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT n FROM review_fix_binary.fixedkeys WHERE id='\x410000ff'::hexlit;
DO $$DECLARE p json; t text; BEGIN
  FOREACH t IN ARRAY ARRAY['hexkeys','fixedkeys'] LOOP
    EXECUTE format('EXPLAIN (FORMAT JSON,COSTS OFF) SELECT n FROM review_fix_binary.%I WHERE id=''\x410000ff''::mysql.hexlit',t) INTO p;
    PERFORM review_fix_binary.assert(p->0->'Plan'->>'Node Type'='Index Scan' AND p->0->'Plan'->>'Index Cond' IS NOT NULL,'btree lookup missing: '||p::text);
  END LOOP;
END$$;
SELECT review_fix_binary.assert(review_fix_value($q$SELECT n FROM review_fix_binary.hexkeys WHERE id=0x410000ff$q$)='2','VARBINARY PK lookup executes');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT n FROM review_fix_binary.fixedkeys WHERE 0x410000ff=id$q$)='2','BINARY PK reverse lookup executes');
SELECT review_fix_binary.assert(count(*)=2,'btree lower/upper bounds execute') FROM review_fix_binary.hexkeys WHERE id>='\x410000ff'::hexlit AND id<'\x43000000'::bytea;
SELECT review_fix_binary.assert(count(*)=3,'cross binary type index join') FROM review_fix_binary.hexkeys a JOIN review_fix_binary.fixedkeys b ON a.id=b.id;
RESET enable_seqscan;

CREATE TABLE review_fix_binary.searches(t text_string COLLATE review_fix_binary.ci);
INSERT INTO review_fix_binary.searches VALUES('Hello World'),('\xc3a900e7958c576f726c64');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT count(*) FROM review_fix_binary.searches WHERE t RLIKE 'world$'$q$)='2','TEXT RLIKE overload and NUL suffix');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT count(*) FROM review_fix_binary.searches WHERE t NOT REGEXP 'world$'$q$)='0','negative REGEXP');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT count(*) FROM review_fix_binary.searches WHERE t REGEXP BINARY 'world$'$q$)='0','REGEXP BINARY case sensitivity');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT LOCATE('WORLD',t) FROM review_fix_binary.searches WHERE t='Hello World'$q$)='7','p4 LOCATE baseline');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT INSTR(t,'world') FROM review_fix_binary.searches WHERE t='Hello World'$q$)='7','p4 INSTR baseline');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT count(*) FROM review_fix_binary.searches WHERE t LIKE '%WORLD'$q$)='2','p4 TEXT LIKE and NUL suffix');
SELECT review_fix_binary.assert(locate('WORLD'::text,'\xc3a900e7958c576f726c64'::text_string COLLATE review_fix_binary.ci)=4,'LOCATE character position after NUL');
SELECT review_fix_binary.assert(locate('界'::text,'\xc3a900e7958c576f726c64'::text_string,3)=3,'LOCATE start counts NUL');
SELECT review_fix_binary.assert(locate('\x00'::text_string,'\xc3a900e7958c'::text_string COLLATE review_fix_binary.ci)=2,'search for NUL');
SELECT review_fix_binary.assert(instr('\xc3a900e7958c576f726c64'::text_string,'\x576f726c64'::hexlit)=7,'binary argument chooses byte positions');
SELECT review_fix_binary.assert(locate(''::text,'abc'::text_string,4)=4 AND locate(''::text,'abc'::text_string,5)=0,'empty needle bounds');
SELECT review_fix_binary.assert(locate('b'::text,'abc'::text_string,NULL)=0 AND locate(NULL::text,'abc'::text_string,1) IS NULL,'LOCATE NULL semantics');
SELECT review_fix_binary.assert('\x410042'::text_string ~* 'b$' AND NOT ('\x410042'::text_string ~* '^a$'),'REGEXP does not truncate at NUL');
SELECT review_fix_binary.assert('\x410042'::text_string ~ '\x0042'::text_string,'NUL in regexp pattern');
SELECT review_fix_binary.assert('\x410042'::text_string LIKE '\x41005f'::text_string,'NUL in LIKE pattern');
SELECT review_fix_binary.assert('\x410042'::text_string ~ '\x0042'::hexlit,'binary regexp pattern');

SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT_WS(0x00,'a',NULL,'',0xff)$q$,true)='bytea|\x610000ff','binary separator and NULL skipping');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT_WS(',',0x00ff,NULL,'b')$q$,true)='bytea|\x00ff2c62','binary CONCAT_WS values');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT_WS(NULL,0xff) IS NULL$q$)='t','NULL separator');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT_WS(0xff,NULL,NULL))$q$)='','all NULL values');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT CONCAT_WS(',',NULL,'','b')$q$,true)='text|,b','empty value is not skipped');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(1,0x00ff,'x')$q$,true)='bytea|\x00ff','ELT binary payload/type');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(2,0x00ff,'x')$q$,true)='bytea|\x78','unselected binary determines ELT type');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(2,'x',0x00ff)$q$,true)='bytea|\x00ff','binary alternative last');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(2,'a') IS NULL$q$)='t','ELT index immediately beyond arguments');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(NULL,'a') IS NULL$q$)='t','ELT NULL index');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(1,NULL,0xff) IS NULL$q$)='t','ELT selected NULL');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(1.9,'a','b')$q$)='b','numeric ELT index rounds');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT('1.9','a','b')$q$)='a','string ELT index uses integer prefix');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(0x02,'a','b')$q$)='b','hexadecimal ELT index');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(18446744073709551617,'a') IS NULL$q$)='t','large ELT index does not overflow');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(ELT(1,0x00ff,review_fix_binary.fail()))$q$)='00FF','ELT leaves other values unevaluated');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(3,review_fix_binary.fail(),0x00ff) IS NULL$q$)='t','out-of-range ELT evaluates no value');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT_WS(s,s,'z')) FROM review_fix_binary.b WHERE id=1$q$)='410042C3A9410042C3A97A','NUL TEXT separator/values');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(1,s,'z') FROM review_fix_binary.b WHERE id=1$q$,true)='text_string|\x410042c3a9','ELT keeps character type');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT_WS(',',s,0xff)) FROM review_fix_binary.b WHERE id=1$q$)='410042C3A92CFF','mixed TEXT and non-UTF8 binary');

-- Casts to text_string use the PG escaped external format in this private PG
-- session; every name is private to this test cluster, and is released below.
SELECT review_fix_binary.assert(mysql.get_lock('\x7265766965775f6669785f4100c384'::text_string,'0'::text_string)=1,'get NUL/Unicode lock');
SELECT review_fix_binary.assert(mysql.is_free_lock('\x7265766965775f6669785f6100c3a4'::text_string)=0,'case-fold suffix after NUL');
SELECT review_fix_binary.assert(mysql.is_free_lock('\x7265766965775f6669785f61'::text_string)=1,'NUL suffix is part of name');
SELECT review_fix_binary.assert(mysql.release_lock('\x7265766965775f6669785f6100c3a4'::text_string)=1,'release exact normalized name');
SELECT review_fix_binary.assert(mysql.get_lock(('review_fix_'||repeat('界',53))::text_string,'0'::text_string)=1,'64 Unicode characters, not 64 bytes');
SELECT review_fix_binary.assert(mysql.release_lock(('review_fix_'||repeat('界',53))::text_string)=1,'release Unicode name');
DO $$BEGIN
  BEGIN PERFORM mysql.get_lock((mysql.raw_bytes('review_fix_'||repeat('x',53)) OPERATOR(pg_catalog.||) decode('0061','hex'))::text_string,0);
    RAISE EXCEPTION 'name suffix escaped length check';
  EXCEPTION WHEN SQLSTATE '42M57' THEN NULL; END;
END$$;
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_timeout','\x300078'::text_string)=1,'length-aware timeout numeric prefix');
SELECT review_fix_binary.assert(mysql.release_lock('review_fix_timeout')=1,'release timeout test lock');
SELECT review_fix_binary.assert(mysql.release_all_locks()=0,'no test locks leaked');
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_recursive'::text_string,0)=1,'recursive lock first acquisition');
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_recursive'::text_string,0)=1,'recursive lock second acquisition');
SELECT review_fix_binary.assert(mysql.release_all_locks()=2,'release all counts acquisitions');
SELECT review_fix_binary.assert(mysql.is_free_lock('review_fix_recursive'::text_string)=1,'release all leaves no lock');
