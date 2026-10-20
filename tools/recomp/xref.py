"""Callers/callees of guest functions (direct bl) with symbol names.

Usage: python -I xref.py <addr|name> [...]
"""
import bisect
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol
from ppc import decode

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')


def load_names():
    names = {}
    for f in ('config/sdk_symbols.txt', 'config/symbols.txt'):
        p = os.path.join(ROOT, f)
        if os.path.exists(p):
            for line in open(p, encoding='utf-8'):
                line = line.split('#')[0].split()
                if len(line) >= 2:
                    names[int(line[0], 16)] = line[1]
    return names


def main():
    dol = Dol(os.path.join(ROOT, 'orig/main.dol'))
    funcs = json.load(open(os.path.join(ROOT, 'build/analysis/functions.json')))['functions']
    starts = [f['addr'] for f in funcs]
    names = load_names()
    by_name = {v: k for k, v in names.items()}

    def owner(a):
        k = bisect.bisect_right(starts, a) - 1
        return starts[k] if k >= 0 else None

    def label(a):
        return f'{a:08X} {names.get(a, "")}'.rstrip()

    calls = {}  # callee -> set(callers)
    callees = {}
    for f in funcs:
        for a in range(f['addr'], f['addr'] + f['size'], 4):
            i = decode(dol.read32(a), a)
            if i.op == 'b' and i.lk:
                calls.setdefault(i.target, set()).add(f['addr'])
                callees.setdefault(f['addr'], set()).add(i.target)
    for arg in sys.argv[1:]:
        t = int(arg, 16) if all(c in '0123456789abcdefABCDEFx' for c in arg) else by_name[arg]
        print(f'== {label(t)}')
        print('  called by:', ', '.join(label(c) for c in sorted(calls.get(t, ()))) or '-')
        print('  calls:    ', ', '.join(label(c) for c in sorted(callees.get(t, ()))) or '-')


main()
