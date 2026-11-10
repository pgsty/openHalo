# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""OID-independent extension catalog comparisons shared by upgrade tests."""
import hashlib
import json


def query(connection, statement, values=None):
    with connection.cursor() as cursor:
        cursor.execute(statement, values)
        return cursor.fetchall() if cursor.description else None


def catalog(c):
    query(c, 'SET search_path=pg_catalog')
    owned = """SELECT objid FROM pg_depend WHERE classid=%s::regclass
        AND refclassid='pg_extension'::regclass AND refobjid=
        (SELECT oid FROM pg_extension WHERE extname='aux_mysql') AND deptype='e'"""
    result = {'members': query(c, """SELECT x.type,x.schema,x.name,x.identity
        FROM pg_depend d CROSS JOIN LATERAL pg_identify_object(d.classid,d.objid,d.objsubid) x
        WHERE d.refclassid='pg_extension'::regclass AND d.refobjid=
        (SELECT oid FROM pg_extension WHERE extname='aux_mysql') AND d.deptype='e'
        ORDER BY x.type,x.schema,x.name,x.identity""")}
    for key, table, statement in (
        ('functions','pg_proc',"""SELECT p.oid::regprocedure::text,pg_get_functiondef(p.oid)
            FROM pg_proc p WHERE p.prokind IN ('f','p') AND p.oid IN ({owned}) ORDER BY 1"""),
        ('views','pg_class',"""SELECT n.nspname,p.relname,pg_get_viewdef(p.oid,false)
            FROM pg_class p JOIN pg_namespace n ON n.oid=p.relnamespace
            WHERE p.relkind='v' AND p.oid IN ({owned}) ORDER BY 1,2"""),
        ('view_columns','pg_class',"""SELECT n.nspname,p.relname,a.attname,
            format_type(a.atttypid,a.atttypmod),cn.nspname,co.collname
            FROM pg_class p JOIN pg_namespace n ON n.oid=p.relnamespace
            JOIN pg_attribute a ON a.attrelid=p.oid AND a.attnum>0 AND NOT a.attisdropped
            LEFT JOIN pg_collation co ON co.oid=a.attcollation
            LEFT JOIN pg_namespace cn ON cn.oid=co.collnamespace
            WHERE p.relkind='v' AND p.oid IN ({owned}) ORDER BY 1,2,a.attnum"""),
        ('casts','pg_cast',"""SELECT p.castsource::regtype::text,p.casttarget::regtype::text,
            p.castfunc::regprocedure::text,p.castcontext,p.castmethod
            FROM pg_cast p WHERE p.oid IN ({owned}) ORDER BY 1,2"""),
        ('operators','pg_operator',"""SELECT p.oid::regoperator::text,p.oprresult::regtype::text,
            p.oprcode::regprocedure::text,p.oprcom::regoperator::text,p.oprnegate::regoperator::text,
            p.oprrest::regprocedure::text,p.oprjoin::regprocedure::text,p.oprcanmerge,p.oprcanhash
            FROM pg_operator p WHERE p.oid IN ({owned}) ORDER BY 1"""),
        ('opclasses','pg_opclass',"""SELECT n.nspname,p.opcname,a.amname,p.opcdefault,
            p.opcintype::regtype::text,p.opckeytype::regtype::text,fn.nspname,f.opfname,
            ARRAY(SELECT ROW(x.amopstrategy,x.amoplefttype::regtype::text,
                x.amoprighttype::regtype::text,x.amopopr::regoperator::text)::text
                FROM pg_amop x WHERE x.amopfamily=f.oid ORDER BY 1),
            ARRAY(SELECT ROW(x.amprocnum,x.amproclefttype::regtype::text,
                x.amprocrighttype::regtype::text,x.amproc::regprocedure::text)::text
                FROM pg_amproc x WHERE x.amprocfamily=f.oid ORDER BY 1)
            FROM pg_opclass p JOIN pg_namespace n ON n.oid=p.opcnamespace
            JOIN pg_am a ON a.oid=p.opcmethod JOIN pg_opfamily f ON f.oid=p.opcfamily
            JOIN pg_namespace fn ON fn.oid=f.opfnamespace
            WHERE p.oid IN ({owned}) ORDER BY 1,2,3"""),
    ):
        result[key] = query(c, statement.format(owned=owned), (table,))
    return result


def compare(old, fresh, artifacts=None, label='upgrade', legacy_extra=False):
    a,b = catalog(old),catalog(fresh)
    original_a = json.loads(json.dumps(a))
    permitted = []
    if legacy_extra:
        # Preserve unreleased 1.6's view descriptor rather than drop its
        # dependent user views. This is the only allowed catalog difference.
        for i,row in enumerate(a['views']):
            if row[:2] == ('mys_informa_schema','columns'):
                newrow = next(r for r in b['views'] if r[:2] == row[:2])
                normalized = row[2].replace('COLLATE "default"','COLLATE "C"')
                assert normalized == newrow[2], 'unexpected columns view definition difference'
                if row != newrow:
                    permitted.append('mys_informa_schema.columns.EXTRA: historical pg_catalog.default retained; fresh pg_catalog.C')
                    a['views'][i] = newrow
        for i,row in enumerate(a['view_columns']):
            if row[:3] == ('mys_informa_schema','columns','extra'):
                assert row[-2:] in (('pg_catalog','default'),('pg_catalog','C')), row
                a['view_columns'][i] = row[:-1]+('C',)
    differences = {}
    for key in a:
        if a[key] != b[key]:
            encode = lambda rows: {json.dumps(r,ensure_ascii=True) for r in rows}
            aa,bb = encode(a[key]),encode(b[key])
            differences[key] = {'upgrade_only': sorted(aa-bb),'fresh_only': sorted(bb-aa)}
    if artifacts:
        artifacts.mkdir(parents=True,exist_ok=True)
        for name,value in [('upgraded',original_a),('fresh',b),('differences',differences)]:
            (artifacts/(label+'-'+name+'.json')).write_text(json.dumps(value,ensure_ascii=True,indent=2)+'\n')
        (artifacts/(label+'-permitted.json')).write_text(json.dumps(permitted,indent=2)+'\n')
    assert not differences, {k:{side:[r[:180] for r in rows[:3]] for side,rows in v.items()} for k,v in differences.items()}
    digest = hashlib.sha256(json.dumps(a,ensure_ascii=True,sort_keys=True).encode()).hexdigest()
    print('PASS',label,'catalog comparison' if permitted else 'catalog equality',
          {k:len(v) for k,v in a.items()},'sha256='+digest,flush=True)
    for difference in permitted: print('PRESERVED',difference,flush=True)
    return digest
