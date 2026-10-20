"""Parse a translated GX frame (build/gxframe_N.bin, written with BLITZ_GX_DUMP=N).

Usage: python -I gxdump.py <frame.bin> [--all]   (--all prints every command)
"""
import struct
import sys
from collections import Counter

AURORA_SIZES = {0x30: 34, 0x31: 23, 0x35: 16, 0x36: 13, 0x37: 8, 0x42: 1}


def field(v, bits, shift):
    return (v >> shift) & ((1 << bits) - 1)


class State:
    def __init__(self):
        self.vcd_lo = self.vcd_hi = 0
        self.vat = [[0] * 8 for _ in range(3)]
        self.xf = {}
        self.bp = {}

    def comp(self, fmt):
        return 1 if fmt <= 1 else 2 if fmt <= 3 else 4

    def vsize(self, n):
        a, b, c = self.vat[0][n], self.vat[1][n], self.vat[2][n]
        lo, hi = self.vcd_lo, self.vcd_hi
        size = sum(field(lo, 1, i) for i in range(9))

        def attr(t, direct, idx=1):
            return {0: 0, 1: direct, 2: idx, 3: 2 * idx}[t]
        size += attr(field(lo, 2, 9), (3 if field(a, 1, 0) else 2) * self.comp(field(a, 3, 1)))
        nbt = field(a, 1, 9)
        size += attr(field(lo, 2, 11), (9 if nbt else 3) * self.comp(field(a, 3, 10)), 3 if nbt and field(a, 1, 31) else 1)
        csz = [2, 3, 4, 2, 3, 4, 4, 4]
        size += attr(field(lo, 2, 13), csz[field(a, 3, 14)])
        size += attr(field(lo, 2, 15), csz[field(a, 3, 18)])
        cnt = [field(a, 1, 21), field(b, 1, 0), field(b, 1, 9), field(b, 1, 18), field(b, 1, 27), field(c, 1, 5), field(c, 1, 14), field(c, 1, 23)]
        fmt = [field(a, 3, 22), field(b, 3, 1), field(b, 3, 10), field(b, 3, 19), field(b, 3, 28), field(c, 3, 6), field(c, 3, 15), field(c, 3, 24)]
        for t in range(8):
            size += attr(field(hi, 2, 2 * t), (2 if cnt[t] else 1) * self.comp(fmt[t]))
        return size


def main():
    d = open(sys.argv[1], 'rb').read()
    show_all = '--all' in sys.argv
    st = State()
    i = 0
    counts = Counter()
    draws = 0
    fog_types = Counter()
    while i < len(d):
        op = d[i]
        if op == 0x61:
            v = struct.unpack('>I', d[i + 1:i + 5])[0]
            st.bp[v >> 24] = v & 0xFFFFFF
            counts[f'BP{v >> 24:02X}'] += 1
            if show_all:
                print(f'{i:7d} BP {v >> 24:02X} {v & 0xFFFFFF:06X}')
            i += 5
        elif op == 0x08:
            reg, v = d[i + 1], struct.unpack('>I', d[i + 2:i + 6])[0]
            if reg == 0x50: st.vcd_lo = v
            elif reg == 0x60: st.vcd_hi = v
            elif 0x70 <= reg < 0xA0 and (reg & 0xF) < 8: st.vat[(reg >> 4) - 7][reg & 7] = v
            counts[f'CP{reg:02X}'] += 1
            if show_all:
                print(f'{i:7d} CP {reg:02X} {v:08X}')
            i += 6
        elif op == 0x10:
            h = struct.unpack('>I', d[i + 1:i + 5])[0]
            n, addr = (h >> 16) + 1, h & 0xFFFF
            vals = struct.unpack(f'>{n}I', d[i + 5:i + 5 + 4 * n])
            for k, v in enumerate(vals):
                st.xf[addr + k] = v
            counts['XF' if addr < 0x1000 else f'XF{addr:04X}'] += 1
            if show_all and addr >= 0x1000:
                print(f'{i:7d} XF {addr:04X} n={n} ' + ' '.join(f'{v:08X}' for v in vals[:8]))
            i += 5 + 4 * n
        elif op in (0x20, 0x28, 0x30, 0x38):
            counts['XFIDX'] += 1
            i += 5
        elif op in (0x00, 0x48):
            i += 1
        elif op == 0x50:
            sub = struct.unpack('>H', d[i + 1:i + 3])[0]
            counts[f'AUR{sub:02X}'] += 1
            size = 13 if 0x10 <= sub <= 0x1F else AURORA_SIZES.get(sub)
            if size is None:
                print('unknown aurora sub', hex(sub), 'at', i)
                return
            if show_all:
                print(f'{i:7d} AURORA {sub:02X}')
            i += 3 + size
        elif op >= 0x80:
            n = struct.unpack('>H', d[i + 1:i + 3])[0]
            vs = st.vsize(op & 7)
            draws += 1
            fsel = (st.bp.get(0xF1, 0) >> 21) & 7  # fog type: 0 = off
            if fsel:
                fog_types[fsel] += 1
            if show_all or draws <= 3:
                xf = st.xf
                print(f'{i:7d} DRAW {op:02X} n={n} vsize={vs} vcd={st.vcd_lo:08X}/{st.vcd_hi:08X} '
                      f'numTexGens(XF103F)={xf.get(0x103F)} numChans(XF1009)={xf.get(0x1009)} '
                      f'genmode={st.bp.get(0):06X} texgen0={xf.get(0x1040, 0):08X} tevC0={st.bp.get(0xC0, 0):06X} '
                      f'order28={st.bp.get(0x28, 0):06X}')
            i += 3 + n * vs
        else:
            print('bad opcode', hex(op), 'at', i)
            return
    fog_draws = sum(fog_types.values())
    print(f'{draws} draws, {len(d)} bytes; draws with fog: {fog_draws} by type {dict(fog_types)}')
    if fog_draws:
        print(f'  last fog regs: EE={st.bp.get(0xEE,0):06X} EF={st.bp.get(0xEF,0):06X} F0={st.bp.get(0xF0,0):06X} '
              f'F1={st.bp.get(0xF1,0):06X} F2(color)={st.bp.get(0xF2,0):06X}')
    print(sorted(counts.items()))


main()
