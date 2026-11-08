#!/usr/bin/env python3
"""
Raw COM_STMT_* protocol checks that drivers rarely exercise.  PyMySQL only
does the authentication; packets are built by hand.
"""
import os
import struct
import sys

import pymysql

COM_STMT_PREPARE, COM_STMT_EXECUTE, COM_STMT_SEND_LONG_DATA = 0x16, 0x17, 0x18
COM_STMT_CLOSE, COM_STMT_RESET = 0x19, 0x1a
T_LONGLONG, T_VAR_STRING, T_BLOB = 0x08, 0xfd, 0xfc

fails = 0


def check(name, ok, detail=''):
    global fails
    print(('  ok   ' if ok else '  FAIL ') + name + ('' if ok else '  -> %r' % (detail,)))
    if not ok:
        fails += 1


conn = pymysql.connect(host='127.0.0.1', port=int(os.environ.get('MYSQL_PORT', '3306')), user='dbuser_mysql',
                       password='Mysql.123', database='test')
DEPRECATE_EOF = bool(conn.server_capabilities & (1 << 24)) and bool(conn.client_flag & (1 << 24))


def send(cmd, payload=b''):
    conn._next_seq_id = 0
    conn._write_bytes(struct.pack('<i', len(payload) + 1)[:3] + b'\x00' + bytes([cmd]) + payload)
    conn._next_seq_id = 1


def read():
    try:
        return conn._read_packet().get_all_data()
    except pymysql.err.MySQLError as e:     # PyMySQL raises on ERR packets
        return b'\xff' + struct.pack('<H', e.args[0]) + b'#HY000' + str(e.args[1]).encode()


def lenenc(b, i):
    c = b[i]
    if c < 251:
        return c, i + 1
    n = {0xfc: 2, 0xfd: 3, 0xfe: 8}[c]
    return int.from_bytes(b[i + 1:i + 1 + n], 'little'), i + 1 + n


def coldef_name(b):
    i = 0
    for _ in range(4):
        n, i = lenenc(b, i)
        i += n
    n, i = lenenc(b, i)
    return b[i:i + n].decode()


def prepare(sql):
    send(COM_STMT_PREPARE, sql.encode())
    p = read()
    if p[0] == 0xff:
        return {'errno': struct.unpack('<H', p[1:3])[0], 'msg': p[9:].decode()}
    sid, ncols, nparams = struct.unpack('<IHH', p[1:9])
    for _ in range(nparams):
        read()
    if nparams and not DEPRECATE_EOF:
        read()
    names = [coldef_name(read()) for _ in range(ncols)]
    if ncols and not DEPRECATE_EOF:
        read()
    return {'id': sid, 'ncols': ncols, 'nparams': nparams, 'names': names}


def execute(sid, params, bind=True):
    """params: list of (type, bytes-or-None)"""
    n = len(params)
    nullmap = bytearray((n + 7) // 8)
    vals = b''
    types = b''
    for k, (t, v) in enumerate(params):
        types += struct.pack('<H', t)
        if v is None:
            nullmap[k // 8] |= 1 << (k % 8)
        elif v is not ...:
            vals += v
    payload = struct.pack('<IBI', sid, 0, 1)
    if n:
        payload += bytes(nullmap) + (b'\x01' + types if bind else b'\x00') + vals
    send(COM_STMT_EXECUTE, payload)
    first = read()
    if first[0] == 0xff:
        return {'errno': struct.unpack('<H', first[1:3])[0]}
    if first[0] == 0x00:
        return {'ok': True}
    ncols, _ = lenenc(first, 0)
    names = [coldef_name(read()) for _ in range(ncols)]
    if not DEPRECATE_EOF:
        read()
    rows = []
    while True:
        p = read()
        if p[0] == 0xfe and len(p) < 0xffffff:
            break
        rows.append(p)
    return {'names': names, 'rows': rows}


def s(v):
    b = v if isinstance(v, bytes) else v.encode()
    return bytes([len(b)]) + b


print('== raw COM_STMT_*')
r = prepare("select '?' as q, ? /* ? */ + 1, `?` -- ?\n from (select 1 as `?`) t where 1 = ?")
check('markers in strings/comments/identifiers are not parameters', r.get('nparams') == 2, r)
check('column names follow the prepared text', r.get('names') == ['q', '? /* ? */ + 1', '?'], r.get('names'))
e = execute(r['id'], [(T_LONGLONG, struct.pack('<q', 41)), (T_LONGLONG, struct.pack('<q', 1))])
check('execute: names in the result set', e.get('names') == ['q', '? /* ? */ + 1', '?'], e)
check('execute: one row', len(e.get('rows', [])) == 1, e)

r = prepare('select ? as a, length(?) as l')
send(COM_STMT_SEND_LONG_DATA, struct.pack('<IH', r['id'], 1) + b'x' * 70000)
send(COM_STMT_SEND_LONG_DATA, struct.pack('<IH', r['id'], 1) + b'y' * 70000)
e = execute(r['id'], [(T_VAR_STRING, s("it's")), (T_BLOB, ...)])


def last_int4(e):                          # binary row: length() is int4
    return struct.unpack('<i', e.get('rows', [b'\0\0\0\0'])[0][-4:])[0]


check('long data (2 chunks, 140000 bytes)', last_int4(e) == 140000, e)
e = execute(r['id'], [(T_VAR_STRING, s('a')), (T_BLOB, s('bb'))])
check('long data is consumed by execute', last_int4(e) == 2, e)
e = execute(r['id'], [(T_VAR_STRING, s('c')), (T_BLOB, s('ddd'))], bind=False)
check('re-execute without re-binding types', last_int4(e) == 3, e)

send(COM_STMT_RESET, struct.pack('<I', r['id']))
check('COM_STMT_RESET answers OK', read()[0] == 0x00)
send(COM_STMT_CLOSE, struct.pack('<I', r['id']))
e = execute(r['id'], [(T_VAR_STRING, s('a')), (T_BLOB, s('b'))])
check('execute after close -> 1243', e.get('errno') == 1243, e)

e = execute(4242, [])
check('unknown statement handler -> 1243', e.get('errno') == 1243, e)
r = prepare('select 1; select 2')
check('multiple statements rejected at prepare -> 1064', r.get('errno') == 1064, r)
r = prepare('select * from no_such_table where id = ?')
check('unknown table reported at prepare -> 1146', r.get('errno') == 1146, r)
r = prepare('select abs(?) as v')
check('type-dependent statement prepares', 'id' in r, r)
e = execute(r['id'], [(T_LONGLONG, struct.pack('<q', -5))])
check('... and executes with the bound value', last_int4(e) == 5, e)

r = prepare('select ? as v')
e = execute(r['id'], [(T_VAR_STRING, s("x') union select version() -- "))])
check('quote in value stays data', b"union select" in e.get('rows', [b''])[0] and len(e['rows']) == 1, e)
e = execute(r['id'], [(T_VAR_STRING, s('a\\') + b'')])
check('trailing backslash stays data', e.get('rows', [b''])[0].endswith(b'a\\'), e)

send(COM_STMT_PREPARE, b'select ? /*!50000')
r = read()
check('unterminated executable comment: prepare fails cleanly', r[:1] == b'\xff' and r[1:3] == struct.pack('<H', 1064), r)
r = prepare('select ? /*!50000 + 1 */ as v')
check('executable comment text is prepared', 'id' in r, r)

with conn.cursor() as cur:
    cur.execute('select 1')
    check('connection usable afterwards', cur.fetchone() == (1,))

print('== raw stmt: all ok' if fails == 0 else '== raw stmt: %d failure(s)' % fails)
sys.exit(1 if fails else 0)
