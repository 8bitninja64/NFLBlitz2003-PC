"""Runs the game with scripted input several times and keeps screenshots that are much
brighter than usual (looking for the white glare/bloom). Frame numbers are drawn on screen
(BLITZ_FRAMENO) so a kept shot can be matched to a BLITZ_GX_DUMP frame.

Usage: python -I tools/brightscan.py <runs> <start_s> <end_s> <threshold>
"""
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'windows-clang', 'NFLBlitz2003.exe')
OUT = os.path.join(ROOT, 'build', 'bright')


def png_mean(path):
    d = open(path, 'rb').read()
    i, idat, w, h, ct = 8, b'', 0, 0, 2
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    prev = bytearray(stride)
    p = 0
    total = count = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            cc = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - cc), abs(a - cc), abs(a + b - 2 * cc)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else cc)) & 255
        prev = line
        if h // 8 <= y < h * 7 // 8 and y % 4 == 0:
            for x in range(w // 6, w * 5 // 6, 4):
                total += sum(line[x * bpp:x * bpp + 3]) / 3
                count += 1
    return total / max(count, 1)


def main():
    runs, start, end, threshold = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
    os.makedirs(OUT, exist_ok=True)
    script = open(os.path.join(ROOT, 'build', 'soak_input.txt')).read().strip()
    env = dict(os.environ, BLITZ_FRAMENO='1', BLITZ_INPUT=script, BLITZ_TOPMOST='1')
    for run in range(runs):
        proc = subprocess.Popen([EXE], cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0 = time.time()
        time.sleep(start)
        n = 0
        while time.time() - t0 < end:
            shot = os.path.join(OUT, 'tmp.png')
            subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File',
                            os.path.join(ROOT, 'tools', 'capture_screen.ps1'), '-Out', 'build/bright/tmp.png',
                            '-Scale', '0.3'], cwd=ROOT, stdout=subprocess.DEVNULL)
            m = png_mean(shot)
            if m >= threshold:
                keep = os.path.join(OUT, f'run{run}_{n:03d}_{int(m)}.png')
                shutil.copy(shot, keep)
                print(f'run {run} t={time.time() - t0:.0f}s brightness {m:.0f} -> {keep}', flush=True)
            n += 1
        proc.kill()
        time.sleep(2)
    print('done', flush=True)


main()
