"""List reference names not yet placed, per library (diagnostic)."""
import json, glob, collections, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import lib_of, is_generic
sym = json.load(open('build/sdkid/symbols.json'))
named = set(v['name'] for v in sym.values())
for v in sym.values():
    named.update(v.get('aliases', []))
miss = collections.defaultdict(set)
skip = set(sys.argv[1:])
for p in glob.glob('build/sdkid/*.json'):
    b = os.path.basename(p)
    if not b.startswith(('melee', 'prime', 'tp')):
        continue
    for f in json.load(open(p)):
        l = lib_of(f['corpus'] + ':' + f['file'], f['name'])
        if f['name'] not in named and not is_generic(f['name']):
            miss[l].add(f['name'])
for l in sorted(miss):
    if l in skip:
        print(l, len(miss[l])); continue
    print(l, len(miss[l]), ' '.join(sorted(miss[l])))
