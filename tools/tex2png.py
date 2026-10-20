"""Decode a dumped GameCube texture (RGBA8 / RGB565 / RGB5A3 / I8) to PNG.
Usage: python -I tex2png.py <file.bin> <width> <height> <format>"""
import struct, sys, zlib

def png(path, w, h, rows):
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def decode(d, w, h, fmt):
    out = [[0] * (w * 4) for _ in range(h)]
    def put(x, y, r, g, b, a):
        if x < w and y < h: out[y][4*x:4*x+4] = [r, g, b, a]
    o = 0
    if fmt == 6:  # RGBA8: 4x4 tiles, AR block then GB block
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                ar = d[o:o+32]; gb = d[o+32:o+64]; o += 64
                for k in range(16):
                    put(tx + k % 4, ty + k // 4, ar[2*k+1], gb[2*k], gb[2*k+1], ar[2*k])
    elif fmt in (4, 5):  # 4x4 tiles of 16-bit
        for ty in range(0, h, 4):
            for tx in range(0, w, 4):
                for k in range(16):
                    v = struct.unpack_from('>H', d, o); o += 2; v = v[0]
                    if fmt == 4:
                        r, g, b, a = (v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255
                    elif v & 0x8000:
                        r, g, b, a = ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255
                    else:
                        r, g, b, a = ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7
                    put(tx + k % 4, ty + k // 4, r, g, b, a)
    elif fmt == 1:  # I8: 8x4 tiles
        for ty in range(0, h, 4):
            for tx in range(0, w, 8):
                for k in range(32):
                    v = d[o]; o += 1
                    put(tx + k % 8, ty + k // 8, v, v, v, 255)
    return out

f, w, h, fmt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
png(f[:-4] + '.png', w, h, decode(open(f, 'rb').read(), w, h, fmt))
