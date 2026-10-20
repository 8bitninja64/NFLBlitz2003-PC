"""Report exact-matched functions whose reference size differs from functions.json size
(boundary errors in build/analysis/functions.json)."""
import json, glob, os
fs = {f['addr']: f['size'] for f in json.load(open('build/analysis/functions.json'))['functions']}
refs = {}
for p in glob.glob('build/sdkid/*.json'):
    if not os.path.basename(p).startswith(('melee', 'prime', 'tp')):
        continue
    for f in json.load(open(p)):
        refs.setdefault((f['corpus'], f['file'], f['name']), len(f['words']) * 4)
sym = json.load(open('build/sdkid/symbols.json'))
for k, v in sorted(sym.items()):
    if not v['method'].startswith('exact') or not v['src']:
        continue
    c, fl = v['src'].split(':', 1)
    L = refs.get((c, fl, v['name']))
    a = int(k, 16)
    if L and a in fs and L != fs[a]:
        print(k, v['name'], 'ref', L, 'dol', fs[a])
