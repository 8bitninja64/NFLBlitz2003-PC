"""Frame-interpolation feasibility: match the draws of consecutive translated GX frames
(build/gxframe_N.bin + build/gxarrays_N.txt, written with BLITZ_GX_DUMP=<first>,<count>) and
classify what changes between them (docs/INTERPOLATION_RESEARCH.md).

Usage: python -I tools/gxpair.py <a.bin> <b.bin> [--list]      one pair, per-class detail
       python -I tools/gxpair.py --range <first> <last> [dir]   every consecutive pair, one line each

Classes per draw of frame B (the newer frame):
  same     state, geometry and matrices identical: draw as is
  xf       state and geometry identical, only position/normal/texture matrices or projection
           differ: interpolable by blending the XF matrices
  skin     same state, textures and index data, but the arrays' contents (and usually their
           addresses) differ: CPU-skinned/rebuilt vertices, need per-vertex interpolation
  geom     same state, textures and vertex count, but inline vertex data differs (direct
           vertices: billboards, particles, HUD)
  new      no counterpart in frame A (appears; or count/state changed)
"""
import math
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(__file__))
from gxdump import AURORA_SIZES, State, field  # noqa: E402

STATE_BP = [0x00, 0x40, 0x41, 0x42, 0x43, 0xF3] + list(range(0x28, 0x30)) + list(range(0xC0, 0xE0)) + \
    list(range(0xF6, 0xFE)) + list(range(0xEE, 0xF3))
STATE_XF = [0x1009, 0x100A, 0x100E, 0x100F, 0x1010, 0x1011, 0x103F] + list(range(0x1040, 0x1048)) + \
    list(range(0x1050, 0x1058))


def f32(w):
    return struct.unpack('>f', struct.pack('>I', w))[0]


class Draw:
    __slots__ = ('idx', 'op', 'n', 'state', 'tex', 'texver', 'vhash', 'ahash', 'bases', 'mtx', 'proj', 'skinned',
                 'mtxids', 'ortho', 'after_copy', 'pos')


POS_FMT = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}


def centroid(st, vat, vdata, n, vs, arr_cur, strides, mem, m):
    """View-space centroid of a non-skinned draw's positions (indexed: from guest MEM1 at the end
    of the frame, build/gxmem_N.bin; direct: inline), or None."""
    lo = st.vcd_lo
    ptype = field(lo, 2, 9)
    a = st.vat[0][vat]
    if ptype == 0 or m is None or (lo & 1):
        return None
    cnt = 3 if field(a, 1, 0) else 2
    code, size = POS_FMT.get(field(a, 3, 1), ('f', 4))
    scale = 1.0 if code == 'f' else 1.0 / (1 << field(a, 5, 4))
    off = sum(field(lo, 1, k) for k in range(9))
    pts = []
    if ptype == 1:
        for v in range(n):
            pts.append(struct.unpack(f'>{cnt}{code}', vdata[v * vs + off:v * vs + off + cnt * size]))
    else:
        if mem is None or 0 not in arr_cur:
            return None
        base = int(arr_cur[0][0], 16) & 0x01FFFFFF
        stride = strides.get(0, cnt * size)
        isz = 1 if ptype == 2 else 2
        idx = set()
        for v in range(n):
            o = v * vs + off
            idx.add(vdata[o] if isz == 1 else vdata[o] << 8 | vdata[o + 1])
        for k in idx:
            o = base + k * stride
            pts.append(struct.unpack(f'>{cnt}{code}', mem[o:o + cnt * size]))
    if not pts:
        return None
    c = [sum(p[k] for p in pts) * scale / len(pts) for k in range(cnt)] + ([0.0] if cnt == 2 else [])
    r = [f32(w) for w in m]
    return tuple(r[4 * k] * c[0] + r[4 * k + 1] * c[1] + r[4 * k + 2] * c[2] + r[4 * k + 3] for k in range(3))


def parse(path):
    d = open(path, 'rb').read()
    arrays = []
    alog = path.replace('gxframe_', 'gxarrays_').replace('.bin', '.txt')
    if os.path.exists(alog):
        for line in open(alog):
            a, base, size, h = line.split()
            arrays.append((int(a), base, h))
    ai = 0
    mpath = path.replace('gxframe_', 'gxmem_')
    mem = open(mpath, 'rb').read() if os.path.exists(mpath) else None
    strides = {}
    st = State()
    tex = {}
    texver = {}
    arr_cur = {}
    draws = []
    copies = 0
    i = 0
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
            elif 0xB0 <= reg < 0xC0: strides[reg - 0xB0] = v & 0xFF
            i += 6
        elif op == 0x10:
            h = struct.unpack('>I', d[i + 1:i + 5])[0]
            n, addr = (h >> 16) + 1, h & 0xFFFF
            for k, v in enumerate(struct.unpack(f'>{n}I', d[i + 5:i + 5 + 4 * n])):
                st.xf[addr + k] = v
            i += 5 + 4 * n
        elif op in (0x20, 0x28, 0x30, 0x38):
            raise SystemExit('indexed XF load: not handled (matrices come from guest memory)')
        elif op in (0x00, 0x48):
            i += 1
        elif op == 0x50:
            sub = struct.unpack('>H', d[i + 1:i + 3])[0]
            size = 13 if 0x10 <= sub <= 0x1F else AURORA_SIZES[sub]
            p = d[i + 3:i + 3 + size]
            if 0x10 <= sub <= 0x1F:
                if ai < len(arrays):
                    a, base, h = arrays[ai]
                    assert a == sub - 0x10, (path, ai, a, sub)
                    arr_cur[a] = (base, h)
                ai += 1
            elif sub == 0x30:  # LOAD_TEXOBJ: id, ptr, w, h, fmt, tlut, mips, hash, version
                tex[p[0]] = p[1:30]
                texver[p[0]] = p[30:34]
            elif sub in (0x34, 0x35, 0x36, 0x37):
                if sub == 0x37:
                    copies += 1
            i += 3 + size
        elif op >= 0x80:
            n = struct.unpack('>H', d[i + 1:i + 3])[0]
            vs = st.vsize(op & 7)
            vdata = d[i + 3:i + 3 + n * vs]
            dr = Draw()
            dr.idx = len(draws)
            dr.op, dr.n = op, n
            vat = op & 7
            ntex = st.xf.get(0x103F, 0)
            dr.state = (st.vcd_lo, st.vcd_hi, st.vat[0][vat], st.vat[1][vat], st.vat[2][vat]) + \
                tuple(st.bp.get(r, 0) for r in STATE_BP) + tuple(st.xf.get(r, 0) for r in STATE_XF)
            # texture identity only for maps the TEV orders can use (enough for matching)
            dr.tex = tuple(tex.get(t) for t in range(8) if ntex)
            dr.texver = tuple(texver.get(t) for t in range(8) if ntex)
            used = set()
            lo = st.vcd_lo
            for k, sh in enumerate((9, 11, 13, 15)):
                if field(lo, 2, sh) >= 2: used.add(k)
            for t in range(8):
                if field(st.vcd_hi, 2, 2 * t) >= 2: used.add(4 + t)
            dr.bases = tuple(sorted((a, arr_cur.get(a, ('?', '?'))[0]) for a in used))
            dr.vhash = hash(vdata)
            dr.ahash = tuple(sorted((a, arr_cur.get(a, ('?', '?'))[1]) for a in used))
            # matrices this draw reads
            dr.skinned = bool(lo & 1)
            ids = set()
            if dr.skinned:
                for v in range(n):
                    ids.add(vdata[v * vs])
            else:
                ids.add(st.xf.get(0x1018, 0) & 0x3F)
            texids = []
            m18, m19 = st.xf.get(0x1018, 0), st.xf.get(0x1019, 0)
            for t in range(min(ntex, 8)):
                texids.append(field(m18, 6, 6 + 6 * t) if t < 4 else field(m19, 6, 6 * (t - 4)))
            dr.mtxids = tuple(sorted(ids))
            mtx = []
            for mi in dr.mtxids:
                mtx.append(tuple(st.xf.get(mi * 4 + k, 0) for k in range(12)))
                mtx.append(tuple(st.xf.get(0x400 + (mi % 32) * 3 + k, 0) for k in range(9)))
            for ti in texids:
                mtx.append(tuple(st.xf.get(ti * 4 + k, 0) for k in range(12)))
            dr.mtx = tuple(mtx)
            dr.proj = tuple(st.xf.get(0x1020 + k, 0) for k in range(7))
            dr.ortho = st.xf.get(0x1026, 0) == 1
            dr.after_copy = copies
            dr.pos = centroid(st, vat, vdata, n, vs, arr_cur, strides, mem, dr.mtx[0] if dr.mtx else None)
            draws.append(dr)
            i += 3 + n * vs
        else:
            raise SystemExit(f'bad opcode {op:#x} at {i}')
    return draws, copies


def match(A, B):
    """Return (pairs, cls): pairs[b] = a index or None; cls[b] in same/xf/skin/geom/new."""
    tiers = (
        ('exact', lambda d: (d.state, d.tex, d.op, d.n, d.vhash, d.ahash)),
        ('skin', lambda d: (d.state, d.tex, d.op, d.n, d.vhash)),  # same indices, array data rebuilt
        ('geom', lambda d: (d.state, d.tex, d.op, d.n)),  # inline vertex data changed
    )
    used = set()
    pairs = [None] * len(B)
    cls = ['new'] * len(B)
    for name, key in tiers:
        q = defaultdict(list)
        for a in A:
            if a.idx not in used:
                q[key(a)].append(a.idx)
        for v in q.values():
            v.reverse()
        for b in B:
            if pairs[b.idx] is not None:
                continue
            c = q.get(key(b))
            if c:
                a = c.pop()
                used.add(a)
                pairs[b.idx] = a
                if name == 'exact':
                    cls[b.idx] = 'same' if (A[a].mtx == b.mtx and A[a].proj == b.proj) else 'xf'
                else:
                    cls[b.idx] = name
    return pairs, cls


def lis_len(seq):
    import bisect
    tails = []
    for x in seq:
        k = bisect.bisect_left(tails, x)
        if k == len(tails): tails.append(x)
        else: tails[k] = x
    return len(tails)


def translation(m):
    return (f32(m[3]), f32(m[7]), f32(m[11]))


def mtx_delta(ma, mb):
    """Largest change of the 3x3 part and the translation (view space units)."""
    rot = max(abs(f32(ma[k]) - f32(mb[k])) for k in (0, 1, 2, 4, 5, 6, 8, 9, 10))
    ta, tb = translation(ma), translation(mb)
    return rot, math.dist(ta, tb)


def summarize(A, B, ca, cb):
    pairs, cls = match(A, B)
    c = Counter(cls)
    v = Counter()
    for b in B:
        v[cls[b.idx]] += b.n
    order = [pairs[b.idx] for b in B if pairs[b.idx] is not None]
    inorder = lis_len(order)
    # camera-cut signal: projection change or the median rotation change of world (3D) draws
    rots = []
    for b in B:
        a = pairs[b.idx]
        if a is not None and not b.ortho and b.mtx and A[a].mtx and not b.skinned:
            rots.append(mtx_delta(A[a].mtx[0], b.mtx[0])[0])
    rots.sort()
    med = rots[len(rots) // 2] if rots else 0.0
    proj = sum(1 for b in B if pairs[b.idx] is not None and A[pairs[b.idx]].proj != b.proj)
    return pairs, cls, c, v, inorder, len(order), med, proj


def verify(A, B, pairs, cls):
    """Mispairing check (needs gxmem): a matched skin/geom draw whose partner is not the nearest
    candidate with the same state/texture/count/indices. Returns (checked, mispaired, max move)."""
    groups = defaultdict(list)
    for a in A:
        if a.pos is not None:
            groups[(a.state, a.tex, a.op, a.n, a.vhash)].append(a)
    checked = bad = 0
    move = 0.0
    for b in B:
        a = pairs[b.idx]
        if a is None or cls[b.idx] not in ('skin', 'xf') or b.pos is None or A[a].pos is None:
            continue
        cands = groups[(b.state, b.tex, b.op, b.n, b.vhash)]
        if len(cands) < 2:
            continue
        checked += 1
        d = math.dist(A[a].pos, b.pos)
        move = max(move, d)
        best = min(math.dist(x.pos, b.pos) for x in cands)
        if best < d * 0.5 and d - best > 0.01:
            bad += 1
    return checked, bad, move


def describe(b):
    kind = 'ortho' if b.ortho else '3d'
    sk = f' skin{len(b.mtxids)}' if b.skinned else ''
    blend = b.state[5 + 2]
    return f'{kind}{sk} op={b.op:02X} n={b.n} blend={blend:06X} arrays={len(b.bases)}'


def main():
    args = sys.argv[1:]
    if args and args[0] == '--range':
        first, last = int(args[1]), int(args[2])
        dr = args[3] if len(args) > 3 else 'build'
        prev = None
        tot = Counter()
        totv = Counter()
        print('frame  draws copies | same   xf skin geom  new | verts: same%  xf% skin% geom%  new% | in-order  rot(med) proj  mispaired/checked')
        for f in range(first, last + 1):
            p = os.path.join(dr, f'gxframe_{f}.bin')
            if not os.path.exists(p):
                prev = None
                continue
            cur = parse(p)
            if prev:
                A, ca = prev
                B, cb = cur
                pairs, cls, c, v, ino, nm, med, proj = summarize(A, B, ca, cb)
                ck, bad, _ = verify(A, B, pairs, cls)
                tot['checked'] += ck
                tot['mispaired'] += bad
                tv = sum(v.values()) or 1
                tot.update(c)
                totv.update(v)
                print(f'{f:5d} {len(B):6d} {cb:6d} | {c["same"]:4d} {c["xf"]:4d} {c["skin"]:4d} {c["geom"]:4d} {c["new"]:4d} | '
                      f'{100*v["same"]/tv:11.1f} {100*v["xf"]/tv:4.1f} {100*v["skin"]/tv:5.1f} {100*v["geom"]/tv:5.1f} {100*v["new"]/tv:5.1f} | '
                      f'{ino:4d}/{nm:<4d} {med:8.4f} {proj:4d}  {bad}/{ck}')
            prev = cur
        print(f'ambiguous matches checked by position: {tot["checked"]}, mispaired: {tot["mispaired"]}')
        del tot['checked'], tot['mispaired']
        td, tv = sum(tot.values()) or 1, sum(totv.values()) or 1
        print('total draws: ' + ' '.join(f'{k} {100*tot[k]/td:.1f}%' for k in ('same', 'xf', 'skin', 'geom', 'new')))
        print('total verts: ' + ' '.join(f'{k} {100*totv[k]/tv:.1f}%' for k in ('same', 'xf', 'skin', 'geom', 'new')))
        return
    A, ca = parse(args[0])
    B, cb = parse(args[1])
    pairs, cls, c, v, ino, nm, med, proj = summarize(A, B, ca, cb)
    print(f'A: {len(A)} draws {ca} copies; B: {len(B)} draws {cb} copies')
    print('draws:', dict(c), ' verts:', dict(v))
    print(f'matched draws in the same relative order: {ino}/{nm}; median world rotation change {med:.4f}; '
          f'projection changed on {proj} matched draws')
    ck, bad, mv = verify(A, B, pairs, cls)
    print(f'ambiguous matches checked by position: {ck}, mispaired: {bad}, largest centroid move {mv:.3f}')
    groups = defaultdict(Counter)
    for b in B:
        groups[cls[b.idx]][describe(b)] += 1
    for k in ('xf', 'skin', 'geom', 'new', 'same'):
        print(f'-- {k}')
        for desc, n in groups[k].most_common(25 if '--list' in args else 12):
            print(f'  {n:4d} x {desc}')
    unmatched_a = len(A) - sum(1 for p in pairs if p is not None)
    print(f'draws of A with no counterpart in B: {unmatched_a}')


main()
