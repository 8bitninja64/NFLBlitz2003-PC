"""Contact sheet of Aurora texture dumps (RGBA8 DDS): python -I tools/ddssheet.py <dir> <out.png> [filter]
filter: 'holes' = mostly opaque textures that contain fully transparent pixels. Transparent pixels
are drawn magenta; each tile is labelled by its index (printed with the file name)."""
import os, struct, sys, zlib
import numpy as np

def load(p):
    d = open(p, 'rb').read()
    h, w = struct.unpack('<II', d[12:20])
    off = 128 + (20 if d[84:88] == b'DX10' else 0)
    return np.frombuffer(d[off:off + w * h * 4], np.uint8).reshape(h, w, 4)

def png(path, img):
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))
    ck = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ck(b'IDAT', zlib.compress(raw)) + ck(b'IEND', b''))

d, out = sys.argv[1], sys.argv[2]
flt = sys.argv[3] if len(sys.argv) > 3 else ''
tiles = []
for f in sorted(os.listdir(d)):
    if not f.endswith('.dds'): continue
    a = load(os.path.join(d, f))
    if a.shape[0] < 32 or a.shape[1] < 32: continue
    zero = (a[..., 3] == 0).mean()
    if flt == 'holes' and not (0.002 < zero < 0.25): continue
    rgb = a[..., :3].copy()
    rgb[a[..., 3] == 0] = (255, 0, 255)
    tiles.append((f, zero, rgb))
T = 128
cols = 10
rows = (len(tiles) + cols - 1) // cols
sheet = np.zeros((rows * T, cols * T, 3), np.uint8)
for i, (f, z, rgb) in enumerate(tiles):
    h, w, _ = rgb.shape
    ys = (np.arange(T) * h // T); xs = (np.arange(T) * w // T)
    sheet[(i // cols) * T:(i // cols + 1) * T, (i % cols) * T:(i % cols + 1) * T] = rgb[ys][:, xs]
    print(i, f, f'{100 * z:.1f}% transparent')
png(out, sheet)
