"""Per-draw summary of translated GX frames (build/gxframe_N.bin): blend mode, the texture
bound to map 0, z mode and vertex count, grouped. Compare frames to find what changes when an
effect appears.

Usage: python -I tools/gxdraws.py <frame.bin>... [--blend 043D] [--list]
"""
import struct
import sys
from collections import Counter

sys.path.insert(0, __import__('os').path.dirname(__file__))
from gxdump import AURORA_SIZES, State  # noqa: E402

FMT = {0: 'I4', 1: 'I8', 2: 'IA4', 3: 'IA8', 4: '565', 5: '5A3', 6: 'RGBA8', 8: 'C4', 9: 'C8', 10: 'C14', 14: 'CMPR'}


def scan(path, want_blend=None, show=False):
    d = open(path, 'rb').read()
    st = State()
    tex = {}
    i = 0
    groups = Counter()
    while i < len(d):
        op = d[i]
        if op == 0x61:
            v = struct.unpack('>I', d[i + 1:i + 5])[0]
            st.bp[v >> 24] = v & 0xFFFFFF
            i += 5
        elif op == 0x08:
            reg, v = d[i + 1], struct.unpack('>I', d[i + 2:i + 6])[0]
            if reg == 0x50: st.vcd_lo = v
            elif reg == 0x60: st.vcd_hi = v
            elif 0x70 <= reg < 0xA0 and (reg & 0xF) < 8: st.vat[(reg >> 4) - 7][reg & 7] = v
            i += 6
        elif op == 0x10:
            h = struct.unpack('>I', d[i + 1:i + 5])[0]
            n, addr = (h >> 16) + 1, h & 0xFFFF
            for k, v in enumerate(struct.unpack(f'>{n}I', d[i + 5:i + 5 + 4 * n])):
                st.xf[addr + k] = v
            i += 5 + 4 * n
        elif op in (0x20, 0x28, 0x30, 0x38):
            i += 5
        elif op in (0x00, 0x48):
            i += 1
        elif op == 0x50:
            sub = struct.unpack('>H', d[i + 1:i + 3])[0]
            size = 13 if 0x10 <= sub <= 0x1F else AURORA_SIZES[sub]
            p = d[i + 3:i + 3 + size]
            if sub == 0x30:  # LOAD_TEXOBJ: id, ptr, w, h, fmt, tlut, mips, hash, version
                tid = p[0]
                w, hgt, fmt, tl = struct.unpack('>IIII', p[9:25])
                tex[tid] = f'{FMT.get(fmt, fmt)} {w}x{hgt} @{struct.unpack(">I", p[26:30])[0]:08X}'
            i += 3 + size
        elif op >= 0x80:
            n = struct.unpack('>H', d[i + 1:i + 3])[0]
            vs = st.vsize(op & 7)
            blend = st.bp.get(0x41, 0)
            key = (f'{blend:06X}', f'z={st.bp.get(0x40, 0):06X}', tex.get(0, '-') if st.xf.get(0x103F, 0) else 'notex',
                   f'kc={st.bp.get(0x42, 0):06X}')
            if want_blend is None or f'{blend & 0xFFFF:04X}' == want_blend:
                groups[key] += n
                if show:
                    print(f'  draw {op:02X} n={n} ' + ' '.join(key) + f' tev0c={st.bp.get(0xC0, 0):06X} tev0a={st.bp.get(0xC1, 0):06X}')
            i += 3 + n * vs
        else:
            print('bad opcode', hex(op), 'at', i)
            break
    return groups


def main():
    files = [a for a in sys.argv[1:] if a.endswith('.bin')]
    blend = sys.argv[sys.argv.index('--blend') + 1] if '--blend' in sys.argv else None
    show = '--list' in sys.argv
    for f in files:
        print(f)
        for key, verts in sorted(scan(f, blend, show).items(), key=lambda kv: -kv[1]):
            print(f'  {verts:6d} verts  ' + '  '.join(key))


main()
