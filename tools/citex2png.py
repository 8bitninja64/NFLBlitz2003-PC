"""Decode a dumped GameCube C4/C8 texture with an RGB5A3/RGB565/IA8 palette (from BLITZ_GX_DUMP) to two
PNGs: colour over magenta where transparent, and the alpha channel.
Usage: python -I tools/citex2png.py <tex.bin> <w> <h> <fmt 8|9> <tlut.bin> <tlutfmt 0|1|2> <out_prefix>"""
import struct, sys, zlib
import numpy as np

def png(p, img):
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))
    ck = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ck(b'IDAT', zlib.compress(raw)) + ck(b'IEND', b''))

def pal(data, fmt):
    out = []
    for i in range(len(data) // 2):
        v = struct.unpack_from('>H', data, 2 * i)[0]
        if fmt == 0:  # IA8: high byte alpha, low intensity
            a, l = v >> 8, v & 255; out.append((l, l, l, a))
        elif fmt == 1:
            out.append(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
        elif v & 0x8000:
            out.append((((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255))
        else:
            out.append((((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7))
    return out

tex, w, h, fmt, tl, tf, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], int(sys.argv[6]), sys.argv[7]
d = open(tex, 'rb').read(); P = pal(open(tl, 'rb').read(), tf)
idx = np.zeros((h, w), int); o = 0
bw, bh = (8, 8) if fmt == 8 else (8, 4)
for ty in range(0, h, bh):
    for tx in range(0, w, bw):
        for y in range(bh):
            for x in range(bw):
                if fmt == 8:
                    b = d[o + (y * bw + x) // 2]; v = b >> 4 if x % 2 == 0 else b & 15
                else:
                    v = d[o + y * bw + x]
                idx[ty + y, tx + x] = v
        o += 32
rgb = np.zeros((h, w, 3), np.uint8); al = np.zeros((h, w, 3), np.uint8)
for y in range(h):
    for x in range(w):
        r, g, b, a = P[idx[y, x]] if idx[y, x] < len(P) else (255, 0, 255, 0)
        rgb[y, x] = (r, g, b) if a else (255, 0, 255); al[y, x] = (a, a, a)
png(out + '.png', rgb); png(out + '_a.png', al)
print('indices used', sorted(set(idx.flatten().tolist()))[:20], 'alpha values', sorted(set(int(P[i][3]) for i in set(idx.flatten().tolist()) if i < len(P))))
