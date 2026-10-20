"""Print library-region layout: named functions and runs of unnamed ones.
Usage: python -I tools/sdkid/layout.py [LO] [HI]"""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import load_functions
lo = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x8013F000
hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x80230000
sym = {int(k, 16): v for k, v in json.load(open('build/sdkid/symbols.json')).items()}
fs = [f for f in load_functions() if lo <= f['addr'] < hi]
run = []
def flush():
    if run:
        print('   [%d unnamed %08X-%08X]' % (len(run), run[0], run[-1]))
        run.clear()
for f in fs:
    s = sym.get(f['addr'])
    if not s:
        run.append(f['addr']); continue
    flush()
    print('%08X %5d %-34s %-4s %-15s %s' % (f['addr'], f['size'], s['name'], s['conf'], s['method'], s['lib']))
flush()
