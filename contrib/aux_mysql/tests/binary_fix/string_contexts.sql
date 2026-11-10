-- Copyright (c) 2026, PGSTY PTE. LTD.
-- Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
-- Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
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

-- Character function/collation scans run against the full extension in route_a.py.

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
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT_WS(s,s,'z')) FROM review_fix_binary.b WHERE id=1$q$)='410142C3A9410142C3A97A','TEXT separator/values');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT ELT(1,s,'z') FROM review_fix_binary.b WHERE id=1$q$,true)=E'text|A\001Bé','ELT keeps character type');
SELECT review_fix_binary.assert(review_fix_value($q$SELECT HEX(CONCAT_WS(',',s,0xff)) FROM review_fix_binary.b WHERE id=1$q$)='410142C3A92CFF','mixed TEXT and non-UTF8 binary');

-- Binary names preserve NUL; text names retain Unicode case folding.
SELECT review_fix_binary.assert(mysql.get_lock('\x7265766965775f6669785f4100c384'::bytea,0)=1,'get binary NUL lock');
SELECT review_fix_binary.assert(mysql.is_free_lock('\x7265766965775f6669785f4100c384'::bytea)=0,'NUL suffix is part of name');
SELECT review_fix_binary.assert(mysql.release_lock('\x7265766965775f6669785f4100c384'::bytea)=1,'release binary NUL lock');
SELECT review_fix_binary.assert(mysql.get_lock(('review_fix_'||repeat('界',53))::text,'0'::text)=1,'64 Unicode characters, not 64 bytes');
SELECT review_fix_binary.assert(mysql.release_lock(('review_fix_'||repeat('界',53))::text)=1,'release Unicode name');
DO $$BEGIN
  BEGIN PERFORM mysql.get_lock((mysql.raw_bytes('review_fix_'||repeat('x',53)) OPERATOR(pg_catalog.||) decode('0061','hex'))::bytea,0);
    RAISE EXCEPTION 'name suffix escaped length check';
  EXCEPTION WHEN SQLSTATE '42M57' THEN NULL; END;
END$$;
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_timeout','\x300078'::bytea)=1,'length-aware timeout numeric prefix');
SELECT review_fix_binary.assert(mysql.release_lock('review_fix_timeout')=1,'release timeout test lock');
SELECT review_fix_binary.assert(mysql.release_all_locks()=0,'no test locks leaked');
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_recursive'::text,0)=1,'recursive lock first acquisition');
SELECT review_fix_binary.assert(mysql.get_lock('review_fix_recursive'::text,0)=1,'recursive lock second acquisition');
SELECT review_fix_binary.assert(mysql.release_all_locks()=2,'release all counts acquisitions');
SELECT review_fix_binary.assert(mysql.is_free_lock('review_fix_recursive'::text)=1,'release all leaves no lock');
