"""Compare device titles across triger-sensor backup dates."""
import os
import sys
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type

for n in ['triger-sensor_2026-08-27-11-28.epro2',
          'triger-sensor_2026-08-28-11-36.epro2',
          'triger-sensor_2026-08-28-16-24.epro2',
          'triger-sensor_2026-08-31-11-37.epro2']:
    p = os.path.join(os.environ['TEMP'], 'ts_bin', n)
    entries = load_epru(p)
    docs = docs_by_type(entries)
    objs = docs['SCH_PAGE']['ca1c10d680cb80bb']
    attrs = collections.defaultdict(dict)
    for c, pls in objs:
        if c.get('type') != 'ATTR':
            continue
        for pp in pls:
            if isinstance(pp, dict) and pp.get('parentId') and pp.get('key') is not None:
                attrs[pp['parentId']][pp['key']] = pp.get('value')
    devs = {}
    for uuid, dlist in docs.get('DEVICE', {}).items():
        for c, pls in dlist:
            if c.get('type') == 'META' and pls:
                m = pls[0]
                if isinstance(m, dict):
                    devs[uuid] = m.get('title')
    print('=' * 34, n[-13:])
    for c, pls in objs:
        if c.get('type') != 'COMPONENT':
            continue
        a = attrs.get(c.get('id'), {})
        des = a.get('Designator')
        if des:
            dev_uuid = a.get('Device')
            title = devs.get(dev_uuid) or str(dev_uuid)[:16]
            print(f'  {des:6} {str(title)[:64]}')
