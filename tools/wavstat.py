"""Audio diagnostics for a BLITZ_WAV recording: clicks (sample jumps far above the local
signal), dropouts (runs of exact silence inside sound), and the spectral balance per second.

Usage: python -I tools/wavstat.py <file.wav> [start_s] [end_s]
"""
import struct
import sys

import numpy as np


def load(path):
    d = open(path, 'rb').read()
    rate = struct.unpack('<I', d[24:28])[0]
    a = np.frombuffer(d[44:], dtype='<i2')
    a = a[:len(a) // 2 * 2].reshape(-1, 2).astype(np.float64)
    return rate, a


def main():
    rate, a = load(sys.argv[1])
    t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else len(a) / rate
    a = a[int(t0 * rate):int(t1 * rate)]
    m = a.mean(axis=1)
    print(f'{len(a) / rate:.1f} s at {rate} Hz, peak {np.abs(a).max():.0f}, rms {np.sqrt((m ** 2).mean()):.0f}')
    # Clicks: |second difference| much larger than the local typical second difference.
    d2 = np.abs(np.diff(m, 2))
    win = 256
    local = np.convolve(d2, np.ones(win) / win, mode='same') + 1
    clicks = np.where((d2 > 12 * local) & (d2 > 2000))[0]
    sec = (clicks / rate).astype(int)
    print(f'clicks: {len(clicks)}; per second (first 20 busy seconds):',
          sorted(((int(s), int(c)) for s, c in zip(*np.unique(sec, return_counts=True))), key=lambda x: -x[1])[:20])
    # Dropouts: >= 32 exact zeros surrounded by sound.
    z = (a[:, 0] == 0) & (a[:, 1] == 0)
    runs, start = [], None
    for i, v in enumerate(z):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if i - start >= 32 and start > 0 and np.abs(m[max(0, start - 64):start]).mean() > 200:
                runs.append((start / rate, i - start))
            start = None
    print(f'dropouts inside sound: {len(runs)}', runs[:10])
    # Spectrum per 2 s: share of energy below 250 Hz, 250-4k, above 4k.
    print('spectral balance (low <250 Hz / mid / high >4 kHz, % of energy):')
    n = 2 * rate
    for k in range(0, len(m) - n, n):
        seg = m[k:k + n] * np.hanning(n)
        p = np.abs(np.fft.rfft(seg)) ** 2
        f = np.fft.rfftfreq(n, 1 / rate)
        tot = p.sum() + 1e-9
        if tot < 1e12:
            continue
        lo, hi = p[f < 250].sum() / tot, p[f > 4000].sum() / tot
        print(f'  {t0 + k / rate:6.1f}s  {100 * lo:5.1f} / {100 * (1 - lo - hi):5.1f} / {100 * hi:5.1f}')


main()
