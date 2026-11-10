#!/usr/bin/env python3
# Copyright (c) 2026, PGSTY PTE. LTD.
# Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
# Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
"""Read-only preflight for an OpenHalo14 postgresql.conf. Exit 2 means review needed."""
import argparse
import json
import pathlib
import re

REMOVED = {
    'database_compat_mode': 'Remove. Dialect is chosen by the wire protocol; mysql.parser controls MySQL connections.',
    'standard_parserengine_auxiliary': 'Remove. There is no auxiliary core parser in OpenHalo18.',
    'mysql.ci_collation': 'Removed upstream in PG14 already. Review column/table collations; use mysql.default_collation for new tables.',
    'halo_mysql_ci_collation': 'Remove. Use explicit column/table collations.',
}
RENAMED = {'halo_mysql.port': 'mysql.port', 'halo_mysql.database': 'mysql.database',
           'halo_mysql.default_collation': 'mysql.default_collation',
           'halo_mysql.auto_create_extension': 'mysql.auto_create_extension'}

def inspect(path, seen=None):
    path = pathlib.Path(path).resolve()
    seen = set() if seen is None else seen
    if path in seen:
        return [{'file':str(path), 'level':'ERROR', 'message':'recursive include'}]
    seen.add(path)
    rows=[]
    for no,line in enumerate(path.read_text().splitlines(),1):
        match=re.match(r"^\s*([a-zA-Z_][\w.]*)\s*(?:=\s*)?(.*?)\s*(?:#.*)?$",line)
        if not match or line.lstrip().startswith('#'): continue
        key,value=match.groups(); key=key.lower()
        base={'file':str(path),'line':no,'parameter':key}
        if key in REMOVED: rows.append(dict(base,level='ERROR',message=REMOVED[key]))
        elif key in RENAMED: rows.append(dict(base,level='ERROR',message='Rename to '+RENAMED[key]))
        elif key.startswith('halo_mysql.'):
            rows.append(dict(base,level='ERROR',message='Research prefix is unsupported; use the documented mysql.* parameter.'))
        elif key == 'shared_preload_libraries' and 'aux_mysql' not in value:
            rows.append(dict(base,level='ERROR',message='Add aux_mysql to the preload list; restart.'))
        elif key == 'mysql.max_allowed_packet':
            rows.append(dict(base,level='INFO',message='Supported in bytes, 1024..1073741823; reload. @@max_allowed_packet reports this limit.'))
        elif key in ('include','include_if_exists','include_dir'):
            rel=value.strip().strip("'").replace("''", "'")
            target=path.parent/rel
            if key=='include_dir':
                if target.is_dir():
                    for child in sorted(target.glob('*.conf')): rows+=inspect(child,seen.copy())
                else: rows.append(dict(base,level='ERROR',message='include directory not found: '+str(target)))
            elif target.exists(): rows+=inspect(target,seen.copy())
            elif key!='include_if_exists': rows.append(dict(base,level='ERROR',message='include not found: '+str(target)))
    return rows

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('config',type=pathlib.Path)
    args=parser.parse_args()
    rows=inspect(args.config)
    print(json.dumps({'config':str(args.config),'findings':rows,
                     'scope':'known migration settings only; this does not certify complete configuration compatibility'},indent=2))
    raise SystemExit(2 if any(r['level']=='ERROR' for r in rows) else 0)
