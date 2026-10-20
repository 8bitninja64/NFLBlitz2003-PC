"""Parse doldisasm-style .s files (/* ADDR OFF  B0 B1 B2 B3 */ insn) from reference
decomp corpora into a function signature corpus (JSON).

Usage: python -I parse_asm.py [--perfile] [--syms symbols.txt] OUT.json TAG ROOT [ROOT...]
--syms: a decomp-toolkit symbols.txt for the same game build; used to split functions at
        their true boundaries and to replace generic names (func_XXXXXXXX etc.).
Each function: {name, file, addr, words[], masks[]} where mask zeroes
relocation-dependent bits (@ha/@h/@l/@sda21 immediates, external branch targets).
"""
import json, os, re, sys

LINE = re.compile(r'^/\*\s*([0-9A-Fa-f]{8})\s+(?:[0-9A-Fa-f]{8}\s+)?([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2})\s*\*/\s*(.*)$')
LABEL = re.compile(r'^([A-Za-z_.$@?][\w.$@?<>,:~\-]*):\s*$')
RELOC = re.compile(r'@(ha|h|l|sda21|sda2)\b')


SYMS = {}   # addr -> name (functions)
ADDRNAMES = {}  # addr -> name (any symbol)
GEN = re.compile(r'^(?:func|sub|lbl|fn|loc|data|unk|jtbl|jumptable|nullsub|zz)_([0-9A-Fa-f]{8})$')


def load_syms(path):
    rx = re.compile(r'^(\S+)\s*=\s*\.?([\w.]+):0x([0-9A-Fa-f]+);.*?type:(\w+)')
    for ln in open(path, encoding='utf-8', errors='replace'):
        m = rx.match(ln)
        if not m:
            continue
        name, sec, addr, typ = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        if typ == 'function':
            SYMS[addr] = name
        if addr not in ADDRNAMES or typ == 'function':
            ADDRNAMES[addr] = name


def canon(sym):
    """Replace a generic reference name with its real name from the symbols map."""
    if re.match(r'^x?[0-9A-Fa-f]{6,8}$', sym) or sym.startswith('0x'):
        return None
    m = GEN.match(sym)
    if m and ADDRNAMES:
        a = int(m.group(1), 16)
        n = ADDRNAMES.get(a)
        if n and not GEN.match(n):
            return n
    return sym


def parse_file(path, perfile=False):
    funcs = []
    stem = os.path.splitext(os.path.basename(path))[0]
    cur = None
    in_text = False
    pending_label = None
    for raw in open(path, encoding='utf-8', errors='replace'):
        line = raw.strip()
        if perfile and not funcs and cur is None:
            in_text = True
        if line.startswith('.section'):
            in_text = '.text' in line or '.init' in line
            cur = None
            continue
        if not in_text:
            continue
        m = LABEL.match(line)
        if m:
            name = m.group(1)
            if not (name.startswith('lbl_') or name.startswith('.L') or name.startswith('L_')):
                pending_label = name
            elif perfile and cur is None and pending_label is None:
                pending_label = stem
            continue
        m = LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        w = int(''.join(m.group(i) for i in range(2, 6)), 16)
        text = m.group(6)
        if addr in SYMS:
            pending_label = SYMS[addr]
        elif SYMS and cur is not None:
            pending_label = None  # the map is authoritative: labels that are not function starts don't split
        if pending_label is not None or cur is None:
            nm = pending_label or ('anon_%08X' % addr)
            nm = canon(nm) or nm
            cur = {'name': nm, 'file': path, 'addr': addr,
                   'words': [], 'masks': [], 'text': []}
            funcs.append(cur)
            pending_label = None
        mask = 0xFFFFFFFF
        if RELOC.search(text):
            mask = 0xFFFF0000
        cur['words'].append(w)
        cur['masks'].append(mask)
        cur['text'].append(text)
        mm = re.search(r'([A-Za-z_.$?][\w.$?@<>,:~\-]*?)@(ha|h|l|sda21|sda2)(?![a-z0-9])', text)
        if mm:
            cs = canon(mm.group(1))
            if cs:
                cur.setdefault('refs', []).append([len(cur['words']) - 1, mm.group(2), cs])
    # external branch targets
    for f in funcs:
        lo, hi = f['addr'], f['addr'] + 4 * len(f['words'])
        for i, w in enumerate(f['words']):
            op = w >> 26
            if op == 18:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                tgt = li if (w & 2) else f['addr'] + 4 * i + li
                if not (lo <= tgt < hi):
                    f['masks'][i] = 0xFC000003
                    parts = f['text'][i].split()
                    if len(parts) >= 2:
                        cs = canon(parts[-1])
                        if ADDRNAMES and tgt in ADDRNAMES:
                            cs = ADDRNAMES[tgt]
                        if cs:
                            f.setdefault('refs', []).append([i, 'b', cs])
        del f['text']
    return funcs


def main():
    args = sys.argv[1:]
    perfile = '--perfile' in args
    args = [a for a in args if a != '--perfile']
    if '--syms' in args:
        i = args.index('--syms')
        load_syms(args[i + 1])
        del args[i:i + 2]
    out, tag, roots = args[0], args[1], args[2:]
    allf = []
    for root in roots:
        for dp, dn, fn in os.walk(root):
            for n in sorted(fn):
                if n.endswith('.s'):
                    p = os.path.join(dp, n)
                    for f in parse_file(p, perfile):
                        f['file'] = os.path.relpath(p, root).replace(os.sep, '/')
                        f['corpus'] = tag
                        allf.append(f)
    json.dump(allf, open(out, 'w'))
    print(len(allf), 'functions')


if __name__ == '__main__':
    main()
