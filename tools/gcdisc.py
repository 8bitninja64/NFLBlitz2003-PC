"""GameCube disc image reader: header, FST listing, file and DOL extraction.

Usage: python -I gcdisc.py <iso> [<outdir> [--dol-only]]   (no outdir: list files)"""
import struct, sys, os

class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.read(0, 0x440)
        self.game_id = h[0:6].decode('ascii')
        self.title = h[0x20:0x60].split(b'\0')[0].decode('ascii', 'replace')
        self.dol_off, self.fst_off, self.fst_size = struct.unpack('>III', self.read(0x420, 12))
        self.files = self._parse_fst()

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _parse_fst(self):
        fst = self.read(self.fst_off, self.fst_size)
        count = struct.unpack('>I', fst[8:12])[0]
        strtab = count * 12
        out = []
        def name(o):
            e = fst.index(b'\0', strtab + o)
            return fst[strtab + o:e].decode('shift_jis', 'replace')
        def walk(i, end, prefix):
            while i < end:
                w0, a, b = struct.unpack('>III', fst[i * 12:i * 12 + 12])
                nm = name(w0 & 0xFFFFFF)
                if w0 >> 24:
                    walk(i + 1, b, prefix + nm + '/')
                    i = b
                else:
                    out.append((prefix + nm, a, b))
                    i += 1
        walk(1, count, '')
        return out

    def dol(self):
        hdr = self.read(self.dol_off, 0x100)
        offs = struct.unpack('>18I', hdr[0:0x48])
        sizes = struct.unpack('>18I', hdr[0x90:0xD8])
        size = max(o + s for o, s in zip(offs, sizes))
        return self.read(self.dol_off, size)

if __name__ == '__main__':
    d = Disc(sys.argv[1])
    print(d.game_id, d.title, hex(d.dol_off))
    if len(sys.argv) > 3 and sys.argv[3] == '--dol-only':
        os.makedirs(sys.argv[2], exist_ok=True)
        open(os.path.join(sys.argv[2], 'main.dol'), 'wb').write(d.dol())
    elif len(sys.argv) > 2:
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, 'main.dol'), 'wb').write(d.dol())
        for p, o, s in d.files:
            fp = os.path.join(out, 'files', p)
            os.makedirs(os.path.dirname(fp), exist_ok=True)
            with open(fp, 'wb') as w:
                w.write(d.read(o, s))
    else:
        for p, o, s in d.files:
            print(f'{s:10d} {p}')
