"""Audio level of a BLITZ_WAV recording (standard library only, unlike wavstat.py): length, peak,
RMS and how many seconds are audible. Exit status 1 if fewer than half the seconds are audible.

Usage: python -I tools/wavlevel.py <file.wav>
"""
import array
import math
import struct
import sys


def main():
    d = open(sys.argv[1], 'rb').read()
    rate = struct.unpack('<I', d[24:28])[0]
    a = array.array('h')
    a.frombytes(d[44:len(d) - (len(d) - 44) % 4])
    if sys.byteorder == 'big':
        a.byteswap()
    sec = rate * 2  # stereo samples per second
    peak, total, audible, seconds = 0, 0.0, 0, len(a) // sec
    for s in range(seconds):
        chunk = a[s * sec:(s + 1) * sec]
        sq = sum(v * v for v in chunk)
        total += sq
        peak = max(peak, max(chunk), -min(chunk))
        if math.sqrt(sq / sec) > 100:
            audible += 1
    rms = math.sqrt(total / max(1, seconds * sec))
    print(f'{seconds} s at {rate} Hz, peak {peak}, rms {rms:.0f}, audible {audible}/{seconds} s')
    sys.exit(0 if seconds and audible * 2 >= seconds else 1)


main()
