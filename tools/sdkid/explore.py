"""Exploration helper: per-function summary (callees, hw regs, strings, sda refs).

Usage: python -I tools/sdkid/explore.py START END      (hex, range of function starts)
       python -I tools/sdkid/explore.py dis ADDR [N]   (disassemble function / N words)
"""
import json, os, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'recomp'))
from dol import Dol  # noqa: E402
from ppc import decode  # noqa: E402

dol = Dol('orig/main.dol')
funcs = sorted(json.load(open('build/analysis/functions.json'))['functions'], key=lambda f: f['addr'])
fsize = {f['addr']: f['size'] for f in funcs}
try:
    sym = {int(k, 16): v['name'] for k, v in json.load(open('build/sdkid/symbols.json')).items()}
except Exception:
    sym = {}


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def cstr(addr, maxlen=60):
    s = dol.section_of(addr)
    if not s or s[3]:
        return None
    o = addr - s[1]
    b = s[2][o:o + maxlen]
    z = b.find(b'\0')
    if z <= 2:
        return None
    b = b[:z]
    if all(32 <= c < 127 or c in (9, 10) for c in b):
        return b.decode()
    return None


def info(a, size=None):
    size = size or fsize.get(a, 4)
    calls, hw, strs, sda = [], set(), [], []
    hi = {}
    for i in range(0, size, 4):
        ia = a + i
        w = dol.read32(ia)
        if w is None:
            break
        ins = decode(w, ia)
        if ins.op == 'b' and ins.lk:
            calls.append(sym.get(ins.imm, '%08X' % ins.imm))
        elif ins.op == 'b' and not (a <= ins.imm < a + size):
            calls.append('->' + sym.get(ins.imm, '%08X' % ins.imm))
        if ins.op == 'addis' and ins.ra == 0:
            hi[ins.rd] = (ins.imm & 0xFFFF) << 16
            if (ins.imm & 0xFFFF) in (0xCC00, 0xCC01):
                hw.add('%X' % ((ins.imm & 0xFFFF) << 16))
        elif ins.op in ('addi', 'ori', 'lwz', 'stw', 'lhz', 'sth', 'lbz', 'stb', 'lfs', 'lfd') and ins.ra in hi:
            v = (hi[ins.ra] + (ins.imm if ins.op != 'ori' else ins.imm & 0xFFFF)) & 0xFFFFFFFF
            if v >> 16 in (0xCC00, 0xCC01):
                hw.add('%08X' % v)
            st = cstr(v)
            if st:
                strs.append(st)
            elif ins.op == 'addi' and dol.is_text(v):
                calls.append('&' + sym.get(v, '%08X' % v))
        if ins.ra in (2, 13) and ins.op in ('lwz', 'stw', 'addi', 'lfs', 'lfd', 'lbz', 'stb', 'lhz', 'sth', 'lha'):
            sda.append('r%d%+d' % (ins.ra, ins.imm))
            if ins.op == 'addi':
                pass
    return calls, sorted(hw), strs, sda


def main():
    if sys.argv[1] == 'dis':
        a = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else fsize.get(a, 64) // 4
        for i in range(n):
            w = dol.read32(a + 4 * i)
            ins = decode(w, a + 4 * i)
            extra = ''
            if ins.op in ('b', 'bc'):
                extra = ' -> %08X %s' % (ins.imm, sym.get(ins.imm, ''))
            print('%08X %08X %-8s rd=%d ra=%d rb=%d imm=%s%s %s' % (a + 4 * i, w, ins.op, ins.rd, ins.ra, ins.rb,
                  hex(ins.imm) if isinstance(ins.imm, int) else ins.imm, ' lk' if ins.lk else '', extra))
        return
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    for f in funcs:
        if lo <= f['addr'] <= hi:
            c, h, s, d = info(f['addr'], f['size'])
            print('%08X %5d %-28s calls=%s hw=%s str=%s sda=%s' % (f['addr'], f['size'], sym.get(f['addr'], '-'),
                  ','.join(c[:8]), ','.join(h[:4]), '|'.join(x[:30] for x in s[:3]), ','.join(d[:4])))


if __name__ == '__main__':
    main()
