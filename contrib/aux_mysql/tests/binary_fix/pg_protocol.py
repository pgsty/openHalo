"""Check actual PG result formats with libpq; no Python DB package required."""
import ctypes as c, ctypes.util, os, pathlib, subprocess
pgconfig=os.environ.get('PG_CONFIG','pg_config')
libdir=subprocess.check_output([pgconfig,'--libdir'],text=True).strip()
candidates=[pathlib.Path(libdir)/'libpq.dylib',pathlib.Path(libdir)/'libpq.so']
lib=next((str(p) for p in candidates if p.exists()),ctypes.util.find_library('pq'))
pq=c.CDLL(lib)
pq.PQconnectdb.argtypes=[c.c_char_p];pq.PQconnectdb.restype=c.c_void_p
pq.PQstatus.argtypes=[c.c_void_p];pq.PQstatus.restype=c.c_int
pq.PQexecParams.argtypes=[c.c_void_p,c.c_char_p,c.c_int,c.c_void_p,c.c_void_p,c.c_void_p,c.c_void_p,c.c_int];pq.PQexecParams.restype=c.c_void_p
pq.PQresultStatus.argtypes=[c.c_void_p];pq.PQresultStatus.restype=c.c_int
pq.PQgetvalue.argtypes=[c.c_void_p,c.c_int,c.c_int];pq.PQgetvalue.restype=c.c_void_p
pq.PQgetlength.argtypes=[c.c_void_p,c.c_int,c.c_int];pq.PQgetlength.restype=c.c_int
pq.PQclear.argtypes=[c.c_void_p];pq.PQfinish.argtypes=[c.c_void_p]
conn=pq.PQconnectdb(b'')
assert pq.PQstatus(conn)==0
try:
    for fmt, expected in [(0,b'\\x410042c3a9'),(1,b'A\x00B\xc3\xa9')]:
        result=pq.PQexecParams(conn,b'SELECT s FROM review_fix_binary.b WHERE id=1',0,None,None,None,None,fmt)
        try:
            assert pq.PQresultStatus(result)==2
            value=c.string_at(pq.PQgetvalue(result,0,0),pq.PQgetlength(result,0,0))
            assert value==expected,(fmt,value,expected)
        finally: pq.PQclear(result)
    # Bind raw binary bytes with inferred custom type, exercising its RECEIVE.
    payload=b'A\x00B\xc3\xa9'
    buffers=(c.c_char_p*1)(payload)
    lengths=(c.c_int*1)(len(payload));formats=(c.c_int*1)(1)
    result=pq.PQexecParams(conn,b'SELECT $1::mysql.text_string',1,None,buffers,lengths,formats,1)
    try:
        assert pq.PQresultStatus(result)==2
        value=c.string_at(pq.PQgetvalue(result,0,0),pq.PQgetlength(result,0,0))
        assert value==payload,(value,payload)
    finally: pq.PQclear(result)
finally: pq.PQfinish(conn)
print('PG text format is escaped and complete; PG binary format preserves every byte.')
