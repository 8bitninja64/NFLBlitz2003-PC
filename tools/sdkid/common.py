"""Shared helpers for the SDK identification pipeline."""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'recomp'))
from dol import Dol  # noqa: E402,F401

GENERIC = ('lbl_', 'func_', 'anon_', 'fn_', '@', 'jumptable_', 'L_', 'sub_', 'nullsub', 'loc_', 'unk_',
           'lit_', 'data_', 'zz_')


def is_generic(n):
    return n.startswith(GENERIC) or n.startswith('.') or n.startswith('$')


def btarget(w, a):
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if w & 2 else a + li) & 0xFFFFFFFF


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def norm_seq(words, base):
    """Relocation-normalized tokens + [(index, target)] of branches leaving the function."""
    n = len(words)
    hi = set()
    out, ext = [], []
    for i, w in enumerate(words):
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        t = w
        if op == 18:
            tg = btarget(w, base + 4 * i)
            if not (base <= tg < base + 4 * n):
                t = w & 0xFC000003
                ext.append((i, tg))
        elif op == 15 and ra == 0:
            t = w & 0xFFFF0000
            hi.add(rd)
        elif op == 24 and rd in hi:  # ori rA, rS, lo
            t = w & 0xFFFF0000
        elif (op == 14 or 32 <= op <= 55) and (ra in hi or ra in (2, 13)):
            t = w & 0xFFFF0000
        elif op == 14 and ra == 1 and rd == 1:
            t = w & 0xFFFF0000  # frame size drift
        elif op == 37 and ra == 1:
            t = w & 0xFFFF0000
        out.append(t)
    return out, ext


COMMON = {0x7C0802A6, 0x90010004, 0x7C0803A6, 0x4E800020, 0x60000000}


def info_words(words, masks):
    """Number of fully-specified, non-boilerplate instructions."""
    n = 0
    for w, m in zip(words, masks):
        if m != 0xFFFFFFFF or w in COMMON:
            continue
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op in (32, 36, 37, 47, 46) and ra == 1:
            continue
        if op == 14 and rd == 1 and ra == 1:
            continue
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):
            continue
        n += 1
    return n


def obj_key(f):
    """Object-file key for a reference function (per-function corpora use the directory)."""
    p = f['file']
    if f['corpus'] == 'tp':
        return 'tp:' + os.path.dirname(p)
    return f['corpus'] + ':' + p


LIBMAP = (('musyx', 'musyx'), ('metrotrk', 'trk'), ('trk_minnow', 'trk'), ('trk_', 'trk'), ('odemuexi', 'odemuexi'),
          ('debuggerdriver', 'odemuexi'), ('dolphin/gx', 'gx'), ('dolphin/os', 'os'), ('dolphin/card', 'card'),
          ('dolphin/dvd', 'dvd'), ('dolphin/vi', 'vi'), ('dolphin/pad', 'pad'), ('dolphin/si', 'si'),
          ('dolphin/exi', 'exi'), ('dolphin/ai', 'ai'), ('dolphin/ar', 'ar'), ('dolphin/dsp', 'dsp'),
          ('dolphin/mtx', 'mtx'), ('dolphin/db', 'db'), ('dolphin/thp', 'thp'), ('dolphin/dtk', 'dtk'),
          ('dolphin/gba', 'gba'), ('dolphin/hio', 'hio'), ('dolphin/ax', 'ax'), ('dolphin/gd', 'gd'),
          ('dolphin/base', 'base'), ('dolphin/ppcarch', 'base'), ('dolphin/mcc', 'mcc'), ('dolphin/jpeg', 'jpeg'),
          ('dolphin/', 'dolphin'), ('runtime', 'runtime'), ('msl', 'msl'), ('init', 'init'), ('os/__ppc', 'init'))


NAMEPFX = (('__GX', 'gx'), ('GX', 'gx'), ('__OS', 'os'), ('OS', 'os'), ('__VI', 'vi'), ('VI', 'vi'),
           ('__PAD', 'pad'), ('PAD', 'pad'), ('__SI', 'si'), ('SI', 'si'), ('__DVD', 'dvd'), ('DVD', 'dvd'),
           ('__CARD', 'card'), ('CARD', 'card'), ('__ARQ', 'ar'), ('ARQ', 'ar'), ('__AR', 'ar'), ('AR', 'ar'),
           ('__AI', 'ai'), ('AI', 'ai'), ('__DSP', 'dsp'), ('DSP', 'dsp'), ('__EXI', 'exi'), ('EXI', 'exi'),
           ('__DB', 'db'), ('DB', 'db'), ('PSMTX', 'mtx'), ('C_MTX', 'mtx'), ('PSVEC', 'mtx'), ('C_VEC', 'mtx'),
           ('PSQUAT', 'mtx'), ('C_QUAT', 'mtx'), ('MTX', 'mtx'), ('TRK', 'trk'), ('__TRK', 'trk'), ('PPC', 'base'),
           ('HIO', 'hio'), ('DTK', 'dtk'), ('__DTK', 'dtk'), ('THP', 'thp'), ('PERF', 'perf'), ('GBA', 'gba'),
           ('DC', 'os'), ('IC', 'os'), ('LC', 'os'))


def lib_of(src, name=None):
    p = src.split(':', 1)[-1].lower()
    if name and 'musyx' not in p:
        for k, lib in NAMEPFX:
            if name.startswith(k) and len(name) > len(k) and (name[len(k)].isupper() or name[len(k)] == '_'):
                return lib
    for key, lib in LIBMAP:
        if key in p:
            return lib
    return p.split('/')[0].replace('.s', '')


def load_functions(root='.'):
    fs = json.load(open(os.path.join(root, 'build/analysis/functions.json')))['functions']
    return sorted(fs, key=lambda f: f['addr'])
