"""Contact sheet of build/shots/frame_N.png (BLITZ_SHOT output): python -I tools/shotsheet.py <out.png> [scale_div] [alpha]"""
import os, re, struct, sys, zlib
import numpy as np

def read_png(p):
    d = open(p, 'rb').read(); i = 8; idat = b''
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]; t = d[i + 4:i + 8]; c = d[i + 8:i + 8 + n]
        if t == b'IHDR': w, h = struct.unpack('>II', c[:8])
        elif t == b'IDAT': idat += c
        i += 12 + n
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, w * 3 + 1)
    return raw[:, 1:].reshape(h, w, 3)  # filter 0 only (frame_shot writes unfiltered rows)

def write_png(p, img):
    h, w, _ = img.shape
    raw = b''.join(b'\0' + img[y].tobytes() for y in range(h))
    ck = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ck(b'IDAT', zlib.compress(raw)) + ck(b'IEND', b''))

out = sys.argv[1]; div = int(sys.argv[2]) if len(sys.argv) > 2 else 4
want_alpha = len(sys.argv) > 3 and sys.argv[3] == 'alpha'
files = sorted((int(m.group(1)), f) for f in os.listdir('build/shots')
               for m in [re.match(r'frame_(\d+)(_alpha)?\.png$', f)] if m and bool(m.group(2)) == want_alpha)
imgs = [(n, read_png(os.path.join('build/shots', f))[::div, ::div]) for n, f in files]
h, w, _ = imgs[0][1].shape; cols = 8; rows = (len(imgs) + cols - 1) // cols
sheet = np.zeros((rows * h, cols * w, 3), np.uint8)
for k, (n, im) in enumerate(imgs):
    sheet[(k // cols) * h:(k // cols + 1) * h, (k % cols) * w:(k % cols + 1) * w] = im
print(' '.join(str(n) for n, _ in imgs))
write_png(out, sheet)
