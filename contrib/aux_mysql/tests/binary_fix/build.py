# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Build binary, lock and collation code into an isolated PG18 test module."""
import os, pathlib, shlex, subprocess, sys
src = pathlib.Path(__file__).resolve().parents[2]
prefix_src = pathlib.Path(os.environ.get('BINARY_FIX_INDEX_SOURCE', str(src/'src'))).resolve()
assert (prefix_src/'mys_prefix.c').is_file(), 'BINARY_FIX_INDEX_SOURCE must contain the integrated mys_prefix.c/.h and weight table'
build = pathlib.Path(os.environ['BINARY_FIX_BUILD_DIR']).resolve()
pgconfig = os.environ.get('PG_CONFIG', 'pg_config')
run = lambda args, **kw: subprocess.run(args, check=True, **kw)
output = lambda args, **kw: subprocess.check_output(args, **kw)
upstream = os.environ.get('BINARY_FIX_PG14_GRAMMAR')
if upstream:
    raw = pathlib.Path(upstream).read_bytes()
else:
    repo = os.environ.get('BINARY_FIX_READONLY_REPO', str(src.parent.parent))
    raw = output(['git', '-C', repo, 'show', 'pg14:src/backend/parser/mysql/mys_gram.y'])
(build/'upstream.y').write_bytes(raw)
run([sys.executable, str(src/'tools/port_grammar.py'), str(build/'upstream.y'), str(build/'mys_gram.y')])
# Stock PG18 lacks OpenHalo's executor-only ON CONFLICT fields. This private
# expression harness never executes INSERT/REPLACE; remove just those field
# assignments from its scratch parser. check_generated.py verifies the actual
# production grammar byte for byte separately.
headers = pathlib.Path(output([pgconfig, '--includedir-server'], text=True).strip())
if 'allIndexes' not in (headers/'nodes/parsenodes.h').read_text():
    grammar = (build/'mys_gram.y').read_text()
    for field in ('allIndexes', 'ignoreFailure'):
        assignment = 'onconflict->' + field + ' = true;'
        assert grammar.count(assignment) == 4, assignment
        grammar = grammar.replace(assignment, '/* OpenHalo executor field omitted in expression harness */')
    (build/'mys_gram.y').write_text(grammar)
run([os.environ.get('BISON', 'bison'), '-d', '-o', str(build/'mys_gram.c'), str(build/'mys_gram.y')])
run([os.environ.get('FLEX', 'flex'), '-CF', '-o', str(build/'mys_scan.c'), str(src/'src/parser/mys_scan.l')])
run(['perl', '-I', str(src/'tools'), str(src/'tools/gen_keywordlist.pl'), '--extern', '--varname=MysScanKeywords', '--output', str(build), str(src/'src/parser/mys_kwlist.h')])
cc = shlex.split(os.environ.get('CC', 'cc'))
flags = cc + ['-fPIC', '-Wall', '-Wextra', '-Wno-unused-parameter', '-DHALO_BABELFISH_API']
flags += shlex.split(output([pgconfig, '--cppflags'], text=True))
flags += ['-I'+output([pgconfig,'--includedir-server'], text=True).strip()]
flags += ['-I'+str(p) for p in [build,src/'src',prefix_src,src/'src/parser',src/'src/mysm/include']]
files = [src/'src/mys_binary.c', src/'src/mys_lock.c', src/'tests/binary_fix/shim.c', src/'tests/binary_fix/prefix_fixture.c', build/'mys_gram.c', build/'mys_scan.c', src/'src/parser/mys_parser.c', src/'src/parser/mys_keywords.c']
objects = []
for file in files:
    obj = build/(file.stem+'.o')
    run(flags+['-c',str(file),'-o',str(obj)])
    objects.append(str(obj))
# Bind backend symbols explicitly on macOS: libc also exports hash_create /
# hash_search with incompatible APIs. Flat dynamic lookup silently chose them.
link = ['-bundle','-bundle_loader',str(pathlib.Path(output([pgconfig,'--bindir'],text=True).strip())/'postgres'),'-undefined','dynamic_lookup'] if sys.platform=='darwin' else ['-shared']
run(cc+link+objects+['-o',str(build/'review_fix_binary.so')])
