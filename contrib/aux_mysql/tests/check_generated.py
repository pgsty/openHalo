#!/usr/bin/env python3
"""Regenerate from the repository's pg14 branch and compare every output byte."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
aux = root / 'contrib/aux_mysql'
with tempfile.TemporaryDirectory(prefix='aux-mysql-generate-') as tmp:
    tmp = Path(tmp)
    inputs, out = tmp / 'input', tmp / 'output'
    inputs.mkdir()
    out.mkdir()
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args])
    grammar = tmp / 'upstream.y'
    grammar.write_bytes(git('show', 'pg14:src/backend/parser/mysql/mys_gram.y'))
    for path in git('ls-tree', '--name-only', 'pg14', 'contrib/aux_mysql/').decode().splitlines():
        (inputs / Path(path).name).write_bytes(git('show', 'pg14:' + path))
    for tool, src, dst in [('port_grammar.py', grammar, out / 'mys_gram.y'),
                           ('port_aux_sql.py', inputs, out / 'aux_mysql.sql')]:
        subprocess.run([sys.executable, '-I', str(aux / 'tools' / tool), str(src), str(dst)], check=True)
    for generated in sorted(out.iterdir()):
        tracked = aux / ('src/parser' if generated.suffix == '.y' else 'sql') / generated.name
        assert generated.read_bytes() == tracked.read_bytes(), str(tracked)
        print('PASS byte identity:', tracked.relative_to(root))
