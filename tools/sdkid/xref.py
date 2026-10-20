"""Find callers (bl/b) of given addresses or symbol names. Usage: xref.py NAME|ADDR ..."""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import Dol, btarget, load_functions
import bisect
dol = Dol('orig/main.dol')
sym = {int(k, 16): v['name'] for k, v in json.load(open('build/sdkid/symbols.json')).items()}
rev = {v: k for k, v in sym.items()}
fs = load_functions(); starts = [f['addr'] for f in fs]
tg = set()
for a in sys.argv[1:]:
    tg.add(rev[a] if a in rev else int(a, 16))
for name, base, data, t in dol.sections:
    if not t: continue
    for o in range(0, len(data), 4):
        w = int.from_bytes(data[o:o+4], 'big')
        if w >> 26 == 18:
            ia = base + o
            x = btarget(w, ia)
            if x in tg:
                i = bisect.bisect_right(starts, ia) - 1
                f = starts[i]
                print('%08X in %08X %-30s -> %s%s' % (ia, f, sym.get(f, '-'), sym.get(x, '%08X' % x), ' (bl)' if w & 1 else ' (b)'))
