"""Function discovery for a symbol-less DOL.

Finds function starts (entry point, bl targets, code pointers in data, code after a
terminator that nothing inside the previous function branches to), partitions the
text sections into contiguous functions and recovers bctr jump tables.

Usage: python -I analyze.py <main.dol> <out.json>
"""
import bisect
import json
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol
from ppc import BRANCH_OPS, decode


def is_uncond_bc(bo):
    return (bo & 0x14) == 0x14


VOLATILE = {0} | set(range(3, 13))  # r0, r3-r12: clobbered by calls
_WRITES_RD = {'addi', 'addis', 'addic', 'addic.', 'subfic', 'mulli', 'add', 'addc', 'adde', 'addme', 'addze',
              'subf', 'subfc', 'subfe', 'subfme', 'subfze', 'neg', 'mullw', 'mulhw', 'mulhwu', 'divw', 'divwu',
              'lwz', 'lbz', 'lhz', 'lha', 'lwzx', 'lbzx', 'lhzx', 'lhax', 'lwbrx', 'lhbrx', 'lwarx',
              'mfspr', 'mftb', 'mfcr', 'mfmsr', 'mfsr', 'mfsrin', 'eciwx'}
_WRITES_RD_RA = {'lwzu', 'lbzu', 'lhzu', 'lhau', 'lwzux', 'lbzux', 'lhzux', 'lhaux'}
_WRITES_RA = {'ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.', 'rlwinm', 'rlwimi', 'rlwnm', 'and', 'andc', 'or',
              'orc', 'xor', 'nand', 'nor', 'eqv', 'slw', 'srw', 'sraw', 'srawi', 'cntlzw', 'extsb', 'extsh',
              'stwu', 'stbu', 'sthu', 'stwux', 'stbux', 'sthux', 'lfsu', 'lfdu', 'lfsux', 'lfdux',
              'stfsu', 'stfdu', 'stfsux', 'stfdux', 'psq_lu', 'psq_lux', 'psq_stu', 'psq_stux'}


def gpr_writes(ins):
    """GPRs an instruction writes (not counting what a call clobbers)."""
    if ins.op in _WRITES_RD:
        return (ins.rd,)
    if ins.op in _WRITES_RA:
        return (ins.ra,)
    if ins.op in _WRITES_RD_RA:
        return (ins.rd, ins.ra)
    if ins.op == 'lmw':
        return tuple(range(ins.rd, 32))
    if ins.op == 'lswi':
        return tuple((ins.rd + k) & 31 for k in range((ins.imm + 3) // 4))
    return ()


class Analysis:
    def __init__(self, dol):
        self.dol = dol
        self.insns = {}
        for name, addr, data, text in dol.text_sections():
            for o in range(0, len(data) - 3, 4):
                w = int.from_bytes(data[o:o + 4], 'big')
                self.insns[addr + o] = decode(w, addr + o)
        self.text_ranges = [(a, a + len(d)) for _, a, d, _ in dol.text_sections()]

    def in_text(self, a):
        return any(lo <= a < hi for lo, hi in self.text_ranges)

    def is_terminator(self, ins):
        if ins.op == 'b' and not ins.lk:
            return True
        if ins.op in ('bclr', 'bcctr') and not ins.lk and is_uncond_bc(ins.rd):
            return True
        if ins.op == 'rfi':
            return True
        return False

    def find_starts(self):
        starts = {self.dol.entry: 'entry'}
        for lo, hi in self.text_ranges:
            starts.setdefault(lo, 'section')
        for a, ins in self.insns.items():
            if ins.op == 'b' and ins.lk and self.in_text(ins.target):
                starts.setdefault(ins.target, 'call')
        # Code pointers in data (vtables, callbacks, function tables).
        self.data_ptrs = set()
        for name, addr, data, text in self.dol.data_sections():
            for o in range(0, len(data) - 3, 4):
                v = int.from_bytes(data[o:o + 4], 'big')
                if v & 3 == 0 and self.in_text(v):
                    self.data_ptrs.add(v)
        # Code pointers built in code: lis rX,hi ; addi rX,rX,lo  (callbacks passed as args)
        self.code_ptrs = set()
        last_lis = {}
        for a in sorted(self.insns):
            ins = self.insns[a]
            if ins.op == 'addis' and ins.ra == 0:
                last_lis[ins.rd] = (a, ins.imm << 16)
            elif ins.op in ('addi', 'ori') and ins.ra in last_lis and a - last_lis[ins.ra][0] <= 32:
                v = (last_lis[ins.ra][1] + (ins.imm if ins.op == 'addi' else ins.imm)) & 0xFFFFFFFF
                if v & 3 == 0 and self.in_text(v):
                    self.code_ptrs.add(v)
        return starts

    def find_jump_tables(self, starts):
        """Map bctr address -> list of targets.

        A switch is lwzx rC,rA,rB ; mtctr rC ; bctr where rA or rB holds the table address, set by
        lis (+ addi) shortly before or, for a switch in a loop, hoisted into a callee-saved register by
        the prologue. The cmplwi guarding the switch bounds it; every case must lie between the call
        targets around the bctr (the function), which rejects tables of function pointers.
        """
        calls = sorted(starts)
        tables = {}
        for a, ins in self.insns.items():
            if not (ins.op == 'bcctr' and not ins.lk and is_uncond_bc(ins.rd)):
                continue
            ctr_reg = None
            load = None
            bound = None
            for back in range(1, 24):
                p = self.insns.get(a - 4 * back)
                if p is None or self.is_terminator(p):
                    break
                if ctr_reg is None:
                    if p.op == 'mtspr' and p.spr == 9:
                        ctr_reg = p.rd
                    continue
                if load is None:
                    if p.op == 'lwzx' and p.rd == ctr_reg:
                        load = p
                    elif p.op == 'lwz' and p.rd == ctr_reg:
                        break  # loaded function pointer, not a table
                    continue
                if p.op == 'cmpli' and bound is None:
                    bound = p.imm + 1
            if load is None:
                continue
            sec_lo, sec_hi = next((lo, hi) for lo, hi in self.text_ranges if lo <= a < hi)
            k = bisect.bisect_right(calls, a)
            lo = max(calls[k - 1], sec_lo) if k else sec_lo
            hi = min(calls[k], sec_hi) if k < len(calls) else sec_hi
            tbl = next((v for v in (self._reg_const(load.addr, r, lo) for r in (load.ra, load.rb))
                        if v is not None), None)
            if tbl is None:
                continue
            targets = []
            n = bound if bound is not None else 1024
            for k in range(n):
                v = self.dol.read32(tbl + 4 * k)
                if v is None or not lo <= v < hi or v & 3:
                    break
                targets.append(v)
            if targets:
                tables[a] = {'table': tbl, 'targets': targets, 'bounded': bound is not None}
        return tables

    def _reg_const(self, a, reg, lo):
        """Address in reg at a when lis (+ addi/ori) set it: searched back through straight-line code
        for a volatile register, back to the prologue (stwu r1) for a callee-saved one; None otherwise."""
        x = a - 4
        while x >= lo:
            p = self.insns[x]
            if reg in VOLATILE and (self.is_terminator(p) or (p.op in BRANCH_OPS and p.lk)):
                return None
            if reg in gpr_writes(p):
                if p.op == 'addis' and p.ra == 0:
                    return (p.imm << 16) & 0xFFFFFFFF
                if p.op == 'addi' and p.ra != 0:
                    v = self._reg_const(x, p.ra, lo)
                    return None if v is None else (v + p.imm) & 0xFFFFFFFF
                if p.op == 'ori':
                    v = self._reg_const(x, p.rd, lo)
                    return None if v is None else v | p.imm
                return None
            if p.op == 'stwu' and p.rd == 1 and p.ra == 1:
                return None  # the function's first instructions
            x -= 4
        return None

    def run(self):
        starts = self.find_starts()
        self.tables = self.find_jump_tables(starts)
        jt_targets = {t for v in self.tables.values() for t in v['targets']}
        jt_addrs = set()
        for v in self.tables.values():
            for k in range(len(v['targets'])):
                jt_addrs.add(v['table'] + 4 * k)

        def prev_is_end(t):
            p = self.insns.get(t - 4)
            return p is None or self.is_terminator(p) or p.word == 0

        # Data/code pointers become starts only if they follow a terminator and are not
        # jump table cases.
        for t in self.data_ptrs | self.code_ptrs:
            if t in jt_targets or t in starts:
                continue
            if prev_is_end(t) and self.insns[t].op != 'illegal' and self.insns[t].word != 0:
                starts[t] = 'ptr'

        # Iteratively split functions at unreachable code following a terminator.
        # A split only shortens the function it came from, so each pass can split every
        # function once; new pieces are examined on the next pass.
        todo = set(starts)
        while todo:
            order = sorted(starts)
            index = {s: i for i, s in enumerate(order)}
            new = set()
            for s in sorted(todo):
                end = self._range_end(s, order, index[s])
                reach = self._reachable(s, end)
                a = s
                while a < end:
                    ins = self.insns[a]
                    if self.is_terminator(ins) and a + 4 < end and (a + 4) not in reach:
                        n = a + 4
                        while n < end and self.insns[n].word == 0:
                            n += 4
                        if n < end and n not in starts:
                            starts[n] = 'split'
                            new.add(n)
                        break
                    a += 4
            todo = new

        order = sorted(starts)
        funcs = []
        for i, s in enumerate(order):
            end = self._range_end(s, order, i)
            # trim trailing zero padding
            e = end
            while e > s and self.insns[e - 4].word == 0:
                e -= 4
            if e == s:
                continue
            funcs.append({'addr': s, 'size': e - s, 'kind': starts[s]})
        self.funcs = funcs
        return funcs

    def _range_end(self, s, order, i):
        sec_end = next(hi for lo, hi in self.text_ranges if lo <= s < hi)
        nxt = order[i + 1] if i + 1 < len(order) else sec_end
        return min(nxt, sec_end)

    def _reachable(self, s, end):
        seen = set()
        work = [s]
        while work:
            a = work.pop()
            while s <= a < end and a not in seen:
                seen.add(a)
                ins = self.insns[a]
                if ins.op == 'b' and not ins.lk:
                    work.append(ins.target)
                    break
                if ins.op == 'bc' and not ins.lk:
                    work.append(ins.target)
                    if is_uncond_bc(ins.rd):
                        break
                if ins.op == 'bcctr' and not ins.lk:
                    for t in self.tables.get(a, {}).get('targets', []):
                        work.append(t)
                    if is_uncond_bc(ins.rd):
                        break
                if ins.op == 'bclr' and not ins.lk and is_uncond_bc(ins.rd):
                    break
                if ins.op == 'rfi':
                    break
                a += 4
        return seen


def main():
    dol = Dol(sys.argv[1])
    an = Analysis(dol)
    funcs = an.run()
    illegal = sum(1 for i in an.insns.values() if i.op == 'illegal' and i.word != 0)
    kinds = {}
    for f in funcs:
        kinds[f['kind']] = kinds.get(f['kind'], 0) + 1
    unrecovered = sum(1 for a, i in an.insns.items()
                      if i.op == 'bcctr' and not i.lk and is_uncond_bc(i.rd) and a not in an.tables)
    print(f'{len(funcs)} functions {kinds}, {len(an.tables)} jump tables ({unrecovered} bctr left to '
          f'callIndirect), {illegal} illegal words')
    out = {
        'entry': dol.entry,
        'functions': funcs,
        'jump_tables': {str(k): v for k, v in an.tables.items()},
    }
    with open(sys.argv[2], 'w', encoding='utf-8') as f:
        json.dump(out, f)


if __name__ == '__main__':
    main()
