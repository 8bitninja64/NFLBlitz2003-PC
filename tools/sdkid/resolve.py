"""SDK / library function identification pipeline for main.dol.

Usage (from project root):
    python -I tools/sdkid/resolve.py [--manual config/sdk_symbols_manual.txt] CORPUS.json [CORPUS.json ...]

Corpora are produced by parse_asm.py; earlier ones win naming ties.
Stages (iterated to a fixpoint):
  1. exact   : relocation-masked word-for-word match, unique in the DOL
  2. callgraph: names of branch / pointer targets taken from matched reference code
  3. gapfill : object-file order alignment between already-named anchors
  4. context : fuzzy match restricted to the neighbourhood of the same object file
  5. global  : (once, at the end) fuzzy match anywhere, only very similar + long functions
Outputs config/sdk_symbols.txt, config/sdk_data_symbols.txt, build/sdkid/symbols.json.
"""
import bisect, collections, difflib, json, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np  # noqa: E402
from common import (Dol, is_generic, btarget, sext16, norm_seq, info_words, obj_key, lib_of,  # noqa: E402
                    load_functions)

LIB_LO = 0x8013F000   # lowest address where library code may appear (besides .text0)


def objbase(o):
    b = o.split(':', 1)[1].lower()
    b = os.path.basename(b)
    return b[:-2] if b.endswith('.s') else b


class Ctx:
    pass


def load_text(dol):
    secs = []
    for name, addr, data, is_text in dol.sections:
        if is_text:
            secs.append((addr, np.frombuffer(data[:len(data) // 4 * 4], dtype='>u4').astype(np.uint32)))
    return secs


def find_all(secs, words, masks):
    out = []
    n = len(words)
    w = np.array(words, dtype=np.uint32)
    m = np.array(masks, dtype=np.uint32)
    for base, arr in secs:
        if len(arr) < n:
            continue
        lim = len(arr) - n + 1
        cand = np.nonzero((arr[:lim] & m[0]) == (w[0] & m[0]))[0]
        for k in range(1, n):
            if len(cand) == 0:
                break
            cand = cand[(arr[cand + k] & m[k]) == (w[k] & m[k])]
        out.extend(int(base + 4 * c) for c in cand)
    return out


def main():
    args = sys.argv[1:]
    manual = []
    if '--manual' in args:
        i = args.index('--manual')
        mpath = args[i + 1]
        del args[i:i + 2]
        if os.path.exists(mpath):
            for ln in open(mpath):
                p = ln.split('#')[0].split()
                if len(p) >= 2:
                    manual.append((int(p[0], 16), p[1], p[2] if len(p) > 2 else 'high'))
    corpora = args
    dol = Dol('orig/main.dol')
    secs = load_text(dol)
    funcs = load_functions()
    fstarts = [f['addr'] for f in funcs]
    fsize = {f['addr']: f['size'] for f in funcs}

    def dol_words(a, n):
        return [dol.read32(a + 4 * i) for i in range(n)]

    dseq_cache = {}

    def dol_seq(a):
        if a not in dseq_cache:
            n = fsize.get(a, 4) // 4
            ws = dol_words(a, n)
            if None in ws:
                ws = [w for w in ws if w is not None]
            dseq_cache[a] = norm_seq(ws, a)
        return dseq_cache[a]

    # ---------------- reference corpus
    refs = []
    for ci, path in enumerate(corpora):
        for f in json.load(open(path)):
            if not f['words']:
                continue
            f['ci'] = ci
            f['obj'] = obj_key(f)
            f['seq'], _ = norm_seq(f['words'], f['addr'])
            f['info'] = info_words(f['words'], f['masks'])
            f['lib'] = lib_of(f['corpus'] + ':' + f['file'], f['name'])
            f.setdefault('refs', [])
            refs.append(f)
    by_name = collections.defaultdict(list)
    objects = collections.defaultdict(list)
    for r in refs:
        by_name[r['name']].append(r)
        objects[r['obj']].append(r)
    for o in objects.values():
        o.sort(key=lambda r: r['addr'])
    name_objs = collections.defaultdict(set)
    for r in refs:
        name_objs[r['name']].add(objbase(r['obj']))
    BIG = ('dolphin_sdk_1', 'dolphin_sdk_2', 'code_80391580', 'code_803a949c', 'runtime', 'gx')
    for nm, objs in name_objs.items():
        if len(objs) > 1:
            objs.difference_update([o for o in list(objs) if o in BIG and len(objs) > 1][:len(objs) - 1])
    print('reference functions', len(refs), 'names', len(by_name))

    # ---------------- exact search over unique masked variants
    variants = {}
    for r in refs:
        key = (tuple(w & m for w, m in zip(r['words'], r['masks'])), tuple(r['masks']))
        variants.setdefault(key, []).append(r)
    vhits = []
    for key, rs in variants.items():
        hits = find_all(secs, rs[0]['words'], rs[0]['masks'])
        if hits:
            vhits.append((rs, hits))
    print('variants', len(variants), 'with hits', len(vhits))

    sym = {}
    name_at = {}
    trusted = []      # (addr, ref, alignment-dict-or-None)
    trusted_keys = set()
    log = collections.Counter()

    def assign(a, name, conf, method, ref):
        if is_generic(name):
            return False
        if name in name_at:
            return False
        if a in sym:
            if sym[a]['name'] != name:
                sym[a].setdefault('aliases', set()).add(name)
            return False
        sym[a] = {'name': name, 'conf': conf, 'method': method,
                  'lib': ref['lib'] if ref else 'cg', 'src': (ref['corpus'] + ':' + ref['file']) if ref else ''}
        name_at[name] = a
        log[method] += 1
        return True

    def trust(a, ref, align=None):
        k = (a, id(ref))
        if k not in trusted_keys:
            trusted_keys.add(k)
            trusted.append((a, ref, align))

    for a, n, c in manual:
        sym[a] = {'name': n, 'conf': c, 'method': 'manual', 'lib': 'manual', 'src': ''}
        name_at[n] = a

    # stage 1
    vhits.sort(key=lambda v: -v[0][0]['info'])
    weak = []
    for rs, hits in vhits:
        info = rs[0]['info']
        if len(hits) == 1 and info >= 5:
            for r in sorted(rs, key=lambda r: r['ci']):
                if assign(hits[0], r['name'], 'high', 'exact', r):
                    break
            for r in rs:
                trust(hits[0], r)
        else:
            weak.append((rs, hits))

    def nearest_named(a):
        ks = sorted(sym)
        i = bisect.bisect_left(ks, a)
        prev = sym[ks[i - 1]]['name'] if i > 0 else None
        j = i + 1 if i < len(ks) and ks[i] == a else i
        nxt = sym[ks[j]]['name'] if j < len(ks) else None
        return prev, nxt, (ks[i - 1] if i > 0 else None), (ks[j] if j < len(ks) else None)

    def context_ok(a, name, maxdist=0x1800):
        prev, nxt, pa, na = nearest_named(a)
        mine = name_objs.get(name, set())
        ok = False
        if prev and pa is not None and a - pa <= maxdist and mine & name_objs.get(prev, set()):
            ok = True
        if nxt and na is not None and na - a <= maxdist and mine & name_objs.get(nxt, set()):
            ok = True
        return ok

    def ratio(seq_a, seq_b):
        sm = difflib.SequenceMatcher(None, seq_a, seq_b, autojunk=False)
        if sm.real_quick_ratio() < 0.4 or sm.quick_ratio() < 0.4:
            return 0.0, None
        return sm.ratio(), sm

    def alignment(sm):
        al = {}
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal' or (tag == 'replace' and (i2 - i1) == (j2 - j1)):
                for k in range(i2 - i1):
                    al[i1 + k] = j1 + k
        return al

    # ---------------- propagation
    data_votes = collections.defaultdict(collections.Counter)
    sda = find_sda_bases(dol)
    print('SDA bases r13=%08X r2=%08X' % sda)

    def propagate():
        votes = collections.defaultdict(collections.Counter)
        data_votes.clear()
        for a, r, al in trusted:
            ha = {}
            for idx, kind, s in r['refs']:
                if al is not None:
                    if idx not in al:
                        continue
                    idx = al[idx]
                ia = a + 4 * idx
                w = dol.read32(ia)
                if w is None:
                    continue
                if kind == 'b':
                    if (w >> 26) == 18:
                        t = btarget(w, ia)
                        if dol.is_text(t):
                            votes[t][s] += 1
                elif kind == 'ha':
                    ha[s] = w & 0xFFFF
                elif kind == 'l' and s in ha:
                    t = ((ha[s] << 16) + sext16(w & 0xFFFF)) & 0xFFFFFFFF
                    if dol.is_text(t):
                        votes[t][s] += 1
                    else:
                        data_votes[t][s] += 1
                elif kind in ('sda21', 'sda2'):
                    ra = (w >> 16) & 31
                    base = sda[0] if ra == 13 else sda[1] if ra == 2 else None
                    if base is not None:
                        data_votes[(base + sext16(w & 0xFFFF)) & 0xFFFFFFFF][s] += 1
        ch = 0
        for t, c in votes.items():
            good = [(n, k) for n, k in c.most_common() if not is_generic(n)]
            if not good or t in sym:
                continue
            n, k = good[0]
            conf = 'high' if len(good) == 1 else 'med'
            if assign(t, n, conf, 'callgraph', None):
                ch += 1
        return ch

    def confirm_weak():
        ch = 0
        for rs, hits in weak:
            names = {r['name'] for r in rs}
            for h in hits:
                if h in sym and sym[h]['name'] in names:
                    for r in rs:
                        if (h, id(r)) not in trusted_keys:
                            trust(h, r)
                            ch += 1
                    if sym[h]['method'] == 'callgraph':
                        sym[h]['method'] = 'exact+callgraph'
                        sym[h]['conf'] = 'high'
                        rr = [r for r in rs if r['name'] == sym[h]['name']][0]
                        sym[h]['lib'] = rr['lib']
                        sym[h]['src'] = rr['corpus'] + ':' + rr['file']
            # unique weak hit with object context
            if len(hits) == 1 and hits[0] not in sym and rs[0]['info'] >= 1:
                h = hits[0]
                for r in sorted(rs, key=lambda r: r['ci']):
                    if r['name'] not in name_at and context_ok(h, r['name'], 0x800):
                        if assign(h, r['name'], 'high' if rs[0]['info'] >= 3 else 'med', 'exact+context', r):
                            trust(h, r)
                            ch += 1
                        break
                else:
                    # long, unique masked match in library territory: accept without object context
                    if len(rs[0]['words']) >= 8 and rs[0]['info'] >= 2 and (h >= LIB_LO or h < 0x80005600):
                        for r in sorted(rs, key=lambda r: r['ci']):
                            if r['name'] not in name_at and assign(h, r['name'], 'med', 'exact-weak', r):
                                trust(h, r)
                                ch += 1
                                break
        return ch

    def unnamed_between(lo, hi):
        """DOL function starts strictly between lo and hi; None if a named function intervenes."""
        i = bisect.bisect_right(fstarts, lo)
        out = []
        while i < len(fstarts) and fstarts[i] < hi:
            a = fstarts[i]
            if a in sym:
                return None
            out.append(a)
            i += 1
        return out

    def align_dp(R, D):
        """Order-preserving assignment of refs R to DOL addrs D maximizing similarity."""
        nR, nD = len(R), len(D)
        S = [[0.0] * nD for _ in range(nR)]
        SM = [[None] * nD for _ in range(nR)]
        for i, r in enumerate(R):
            for j, d in enumerate(D):
                ds = dol_seq(d)[0]
                L1, L2 = len(r['seq']), len(ds)
                if min(L1, L2) * 2.5 < max(L1, L2):
                    continue
                s, sm = ratio(r['seq'], ds)
                S[i][j], SM[i][j] = s, sm
        best = [[0.0] * (nD + 1) for _ in range(nR + 1)]
        for i in range(nR - 1, -1, -1):
            for j in range(nD - 1, -1, -1):
                best[i][j] = max(best[i + 1][j], best[i][j + 1], best[i + 1][j + 1] + (S[i][j] if S[i][j] >= 0.45 else 0))
        out = []
        i = j = 0
        while i < nR and j < nD:
            if S[i][j] >= 0.45 and abs(best[i][j] - (best[i + 1][j + 1] + S[i][j])) < 1e-9:
                out.append((R[i], D[j], S[i][j], SM[i][j]))
                i += 1
                j += 1
            elif best[i][j] == best[i + 1][j]:
                i += 1
            else:
                j += 1
        return out

    def gapfill():
        ch = 0
        for ok, lst in objects.items():
            anchors = [(k, name_at[r['name']]) for k, r in enumerate(lst) if r['name'] in name_at]
            # keep monotonic anchors only
            mono = []
            for k, a in anchors:
                if not mono or a > mono[-1][1]:
                    mono.append((k, a))
            if not mono:
                continue
            segs = []
            for (k1, a1), (k2, a2) in zip(mono, mono[1:]):
                if k2 - k1 > 1:
                    segs.append((lst[k1 + 1:k2], a1, a2))
            # head / tail segments
            k0, a0 = mono[0]
            if k0 > 0:
                i = bisect.bisect_left(fstarts, a0)
                lo = fstarts[max(0, i - k0 - 2)]
                segs.append((lst[:k0], lo - 1, a0))
            kl, al_ = mono[-1]
            if kl < len(lst) - 1:
                i = bisect.bisect_right(fstarts, al_)
                hi = fstarts[min(len(fstarts) - 1, i + (len(lst) - kl) + 2)]
                segs.append((lst[kl + 1:], al_, hi + 1))
            for R, lo, hi in segs:
                R = [r for r in R if r['name'] not in name_at and not is_generic(r['name'])]
                if not R or hi - lo > 0x6000:
                    continue
                i = bisect.bisect_right(fstarts, lo)
                D = []
                while i < len(fstarts) and fstarts[i] < hi:
                    if fstarts[i] not in sym and fstarts[i] >= LIB_LO or fstarts[i] < 0x80005600 and fstarts[i] not in sym:
                        D.append(fstarts[i])
                    elif fstarts[i] in sym and lo < fstarts[i] < hi and not (name_objs.get(sym[fstarts[i]]['name'], set()) & name_objs[R[0]['name']]):
                        # foreign symbol inside: stop at it for tail segs, restart for head segs
                        if lo == min(lo, fstarts[i]):
                            pass
                    i += 1
                if not D or len(D) > 3 * len(R) + 6:
                    continue
                for r, d, s, sm in align_dp(R, D):
                    if d in sym or r['name'] in name_at:
                        continue
                    eq = len(R) == len(D)
                    if len(r['seq']) <= 3:
                        # tiny functions (blr stubs, getters): only trust a 1:1 segment with identical code
                        if not (eq and s == 1.0):
                            continue
                        conf = 'med'
                    elif s >= 0.8:
                        conf = 'high'
                    elif s >= 0.6 or (eq and s >= 0.5):
                        conf = 'med'
                    else:
                        conf = 'low'
                    if assign(d, r['name'], conf, 'gapfill', r):
                        ch += 1
                        if conf == 'high':
                            trust(d, r, None if s == 1.0 else alignment(sm))
        return ch

    def context_fuzzy(final=False):
        ch = 0
        # candidate unnamed refs whose object has at least one named sibling
        pend = []
        for nm, rs in by_name.items():
            if nm in name_at or is_generic(nm):
                continue
            pend.append((nm, rs))
        unnamed = [a for a in fstarts if a not in sym and (a >= LIB_LO or a < 0x80005600)]
        results = []
        for nm, rs in pend:
            # neighbourhood: DOL addrs of siblings in same object
            sib = set()
            for r in rs:
                for s in objects[r['obj']]:
                    if s['name'] in name_at:
                        sib.add(name_at[s['name']])
            best = None
            if sib and not final:
                lo, hi = min(sib) - 0x2000, max(sib) + 0x2000
                cands = unnamed[bisect.bisect_left(unnamed, lo):bisect.bisect_right(unnamed, hi)]
            elif final:
                cands = unnamed
            else:
                continue
            for r in rs:
                L = len(r['seq'])
                if final and (L < 12 or r['info'] < 6):
                    continue
                for d in cands:
                    ds = dol_seq(d)[0]
                    if min(L, len(ds)) * 1.6 < max(L, len(ds)):
                        continue
                    s, sm = ratio(r['seq'], ds)
                    if best is None or s > best[0]:
                        best = (s, d, r, sm)
            if best and best[0] >= (0.6 if not final else 0.8):
                results.append((best[0], nm) + best[1:])
        results.sort(key=lambda x: -x[0])
        for s, nm, d, r, sm in results:
            if d in sym or nm in name_at:
                continue
            if final:
                conf = 'med' if s >= 0.92 else 'low'
                meth = 'fuzzy-global'
            else:
                if not context_ok(d, nm, 0x2000):
                    continue
                conf = 'high' if s >= 0.85 else 'med' if s >= 0.7 else 'low'
                meth = 'fuzzy-context'
            if assign(d, nm, conf, meth, r):
                ch += 1
                if conf == 'high':
                    trust(d, r, None if s == 1.0 else alignment(sm))
        return ch

    def lib_region_fuzzy():
        """Unnamed DOL functions sandwiched between named functions of one library are matched
        against that library's unplaced reference functions."""
        ks = sorted(sym)
        cand_by_lib = collections.defaultdict(list)
        for a in fstarts:
            if a in sym or not (a >= LIB_LO or a < 0x80005600):
                continue
            i = bisect.bisect_left(ks, a)
            if i == 0 or i >= len(ks):
                continue
            pl, nl = sym[ks[i - 1]]['lib'], sym[ks[i]]['lib']
            if a - ks[i - 1] > 0x3000 or ks[i] - a > 0x3000:
                continue
            for l in {pl, nl}:
                if l not in ('cg', 'manual'):
                    cand_by_lib[l].append(a)
        results = []
        for nm, rs in by_name.items():
            if nm in name_at or is_generic(nm):
                continue
            best = None
            for r in rs:
                L = len(r['seq'])
                if L < 4:
                    continue
                for d in cand_by_lib.get(r['lib'], ()):
                    ds = dol_seq(d)[0]
                    if min(L, len(ds)) * 1.5 < max(L, len(ds)):
                        continue
                    sc, sm = ratio(r['seq'], ds)
                    if best is None or sc > best[0]:
                        best = (sc, d, r, sm)
            if best and best[0] >= 0.6:
                results.append((best[0], nm) + best[1:])
        results.sort(key=lambda x: -x[0])
        ch = 0
        for sc, nm, d, r, sm in results:
            if d in sym or nm in name_at:
                continue
            conf = 'high' if sc >= 0.85 else 'med' if sc >= 0.7 else 'low'
            if assign(d, nm, conf, 'fuzzy-libregion', r):
                ch += 1
                if conf == 'high':
                    trust(d, r, None if sc == 1.0 else alignment(sm))
        return ch

    it = 0
    while True:
        it += 1
        c1 = propagate()
        c2 = confirm_weak()
        c3 = gapfill()
        c4 = context_fuzzy() if c1 + c2 + c3 == 0 else 0
        if c1 + c2 + c3 + c4 == 0:
            c4 = lib_region_fuzzy()
        print('iter', it, 'callgraph', c1, 'weak', c2, 'gapfill', c3, 'context/libregion', c4, 'total', len(sym))
        if c1 + c2 + c3 + c4 == 0:
            break
    c5 = context_fuzzy(final=True)
    propagate()
    print('global fuzzy', c5)

    # ---------------- output
    os.makedirs('config', exist_ok=True)
    with open('config/sdk_symbols.txt', 'w', newline='\n') as fo:
        fo.write('# addr name confidence method lib=<lib> [src=<reference>] [aliases=..] [note]\n')
        fo.write('# generated by tools/sdkid/resolve.py -- do not edit; put overrides in config/sdk_symbols_manual.txt\n')
        for a in sorted(sym):
            s = sym[a]
            note = '' if a in fsize else ' boundary=not_a_function_start'
            if a in fsize and s.get('reflen') and s['reflen'] != fsize[a]:
                note += ' size_mismatch'
            al = (' aliases=' + ','.join(sorted(s['aliases']))) if s.get('aliases') else ''
            fo.write('0x%08X %s %s %s lib=%s%s%s%s\n' % (a, s['name'], s['conf'], s['method'], s['lib'],
                                                       (' src=' + s['src']) if s['src'] else '', al, note))
    with open('config/sdk_data_symbols.txt', 'w', newline='\n') as fo:
        fo.write('# addr name votes -- data symbols referenced by identified library code\n')
        seen = set()
        for t in sorted(data_votes):
            c = data_votes[t]
            good = [(n, k) for n, k in c.most_common() if not is_generic(n)]
            if not good or good[0][0] in seen:
                continue
            seen.add(good[0][0])
            fo.write('0x%08X %s %d%s\n' % (t, good[0][0], good[0][1], ' conflict=' + ','.join(n for n, _ in good[1:]) if len(good) > 1 else ''))
    json.dump({'%08X' % a: {k: (sorted(v) if isinstance(v, set) else v) for k, v in s.items()} for a, s in sym.items()},
              open('build/sdkid/symbols.json', 'w'), indent=0)
    print(len(sym), 'symbols', dict(log))
    print(collections.Counter(s['lib'] for s in sym.values()).most_common())
    print(collections.Counter(s['conf'] for s in sym.values()))


def find_sda_bases(dol):
    """Locate __init_registers style 'lis r13,X; ori r13,r13,Y' / r2 pairs."""
    r13 = r2 = 0
    for name, base, data, is_text in dol.sections:
        if not is_text:
            continue
        for o in range(0, len(data) - 4, 4):
            w = int.from_bytes(data[o:o + 4], 'big')
            w2 = int.from_bytes(data[o + 4:o + 8], 'big')
            if (w >> 16) == 0x3DA0 and (w2 >> 16) == 0x61AD:
                r13 = ((w & 0xFFFF) << 16) | (w2 & 0xFFFF)
            elif (w >> 16) == 0x3C40 and (w2 >> 16) == 0x6042:
                r2 = ((w & 0xFFFF) << 16) | (w2 & 0xFFFF)
            if r13 and r2:
                return r13, r2
    return r13, r2


if __name__ == '__main__':
    main()
