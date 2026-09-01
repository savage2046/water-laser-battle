"""Dump PCB component list from the triger-sensor backup."""
import os
import sys
import json
import collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_full_report import load_epru, docs_by_type

p = os.path.join(os.environ['TEMP'], 'ts_bin', 'triger-sensor_2026-08-31-11-37.epro2')
entries = load_epru(p)
docs = docs_by_type(entries)
pcb_objs = docs['PCB']['a63391f5039f097a']

attrs = collections.defaultdict(dict)
for c, pls in pcb_objs:
    if c.get('type') != 'ATTR':
        continue
    for pp in pls:
        if isinstance(pp, dict) and pp.get('parentId') and pp.get('key') is not None:
            attrs[pp['parentId']][pp['key']] = pp.get('value')

print('=== PCB components ===')
for c, pls in pcb_objs:
    if c.get('type') != 'COMPONENT':
        continue
    cid = c.get('id')
    pos = {}
    for pp in pls:
        if isinstance(pp, dict):
            pos = {'x': pp.get('x'), 'y': pp.get('y'), 'rot': pp.get('rotation')}
    a = attrs.get(cid, {})
    des = a.get('Designator')
    if des:
        nm = a.get('Name') or ''
        print(f"  {des:6} {str(nm)[:24]:24} pos={pos}")

print()
print('=== PAD_NET sample (first 10) ===')
n = 0
for c, pls in pcb_objs:
    if c.get('type') != 'PAD_NET':
        continue
    for pp in pls:
        if isinstance(pp, dict):
            print('  ', json.dumps(pp, ensure_ascii=False)[:160])
            n += 1
            if n >= 10:
                break
    if n >= 10:
        break

print()
print('=== VIA sample ===')
n = 0
for c, pls in pcb_objs:
    if c.get('type') != 'VIA':
        continue
    for pp in pls:
        if isinstance(pp, dict):
            print('  ', json.dumps(pp, ensure_ascii=False)[:160])
            n += 1
            if n >= 5:
                break
    if n >= 5:
        break
