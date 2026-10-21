"""PowerPC -> C++ static recompiler for the NFL Blitz 2003 DOL.

Usage: python -I recomp.py <main.dol> <functions.json> <symbols.txt[,more.txt]> <hooks.txt> <outdir>

Every guest function becomes `void f_XXXXXXXX(guest::Ctx& c)`. Branches inside a
function are gotos, bl is a direct call, bctr through a recovered jump table is a
switch, other indirect branches go through guest::callIndirect. Functions listed in
hooks.txt are not translated: their f_ symbol forwards to a host HLE_<name>. A '?' before the name makes
a pre-hook: the function is translated, but first calls HLE_<name> with r0 = 0 and returns if it set r0.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol import Dol
from ppc import decode

FILES = 32
LDST_RE = re.compile(r'(lwz|lbz|lhz|lha|stw|stb|sth|lfs|lfd|stfs|stfd)(u?)(x?)')


def mask32(mb, me):
    m = 0
    if mb <= me:
        for b in range(mb, me + 1):
            m |= 1 << (31 - b)
    else:
        for b in range(0, me + 1):
            m |= 1 << (31 - b)
        for b in range(mb, 32):
            m |= 1 << (31 - b)
    return m


def hx(v):
    return f'0x{v & 0xFFFFFFFF:08X}u'


def load_symbols(paths):
    """Comma-separated symbol files; later files override earlier ones."""
    syms = {}
    for path in paths.split(','):
        syms.update(_load_symbol_file(path))
    return syms


def _load_symbol_file(path):
    syms = {}
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if not line:
                continue
            parts = line.split()
            syms[int(parts[0], 16)] = parts[1]
    return syms


def load_hooks(path):
    """hooks.txt: one symbol name or 0xADDR per line, optional '= HLEName'."""
    hooks = []
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if line:
                hooks.append(line)
    return hooks


class FuncGen:
    def __init__(self, g, func):
        self.g = g
        self.start = func['addr']
        self.end = func['addr'] + func['size']
        self.lines = []
        self.labels = set()
        self.entries = sorted(g.extra_entries.get(self.start, ()))

    def R(self, n):
        return f'c.r[{n}]'

    def RA0(self, n):
        return '0u' if n == 0 else f'c.r[{n}]'

    def F(self, n):
        return f'c.f[{n}].d'

    def P1(self, n):
        return f'c.ps1[{n}]'

    def emit(self, s):
        self.lines.append('  ' + s)

    def in_func(self, t):
        return self.start <= t < self.end

    def cond(self, bo, bi):
        pre = ''
        parts = []
        if not bo & 4:
            pre = 'c.ctr--; '
            parts.append('c.ctr == 0' if bo & 2 else 'c.ctr != 0')
        if not bo & 0x10:
            bit = f'(c.cr[{bi >> 2}] & {8 >> (bi & 3)})'
            parts.append(bit if bo & 8 else f'!{bit}')
        return pre, (' && '.join(parts) if parts else 'true')

    def branch_to(self, a, t, call):
        """Code that transfers control to t (not a call)."""
        if self.in_func(t):
            self.labels.add(t)
            back = f'CHECK_IRQ(c, 0x{a:08X}u); ' if t <= a else ''
            return f'{back}goto L_{t:08X};'
        name = self.g.callee(t, a)
        return f'{name}(c); return;'

    def gen(self):
        g = self.g
        for t in self.entries:
            self.labels.add(t)
        body = []
        self.lines = body
        a = self.start
        while a < self.end:
            ins = g.insns[a]
            self.lines.append(f'@L{a:08X}')
            self.insn(ins)
            a += 4
        out = []
        name = g.fname(self.start)
        if self.entries:
            out.append(f'static void body_{self.start:08X}(guest::Ctx& c, int entry) {{')
            out.append(f'  GUEST_ENTER(0x{self.start:08X}u);')
            out.append('  switch (entry) {')
            for k, t in enumerate(self.entries, 1):
                out.append(f'  case {k}: goto L_{t:08X};')
            out.append('  default: break;')
            out.append('  }')
        else:
            out.append(f'void {name}(guest::Ctx& c) {{')
        out.append(f'  GUEST_ENTER(0x{self.start:08X}u);')
        if self.start in g.prehooks:
            out.append(f'  c.r[0] = 0; HLE_{g.prehooks[self.start]}(c); if (c.r[0]) return;  // pre-hook')
        for ln in body:
            if ln.startswith('@L'):
                t = int(ln[2:], 16)
                if t in self.labels:
                    out.append(f'L_{t:08X}:;')
                continue
            out.append(ln)
        # Falling off the end continues into the next function.
        nxt = self.end
        if nxt in g.starts:
            out.append(f'  {g.fname(nxt)}(c);')
        out.append('}')
        if self.entries:
            out.append(f'void {name}(guest::Ctx& c) {{ body_{self.start:08X}(c, 0); }}')
            for k, t in enumerate(self.entries, 1):
                out.append(f'void {g.fname(t)}(guest::Ctx& c) {{ body_{self.start:08X}(c, {k}); }}')
        return out

    # ------------------------------------------------------------------
    def insn(self, i):
        g = self.g
        op = i.op
        a = i.addr
        R, RA0, F, P1, E = self.R, self.RA0, self.F, self.P1, self.emit
        rd, ra, rb, rc = i.rd, i.ra, i.rb, i.rc
        nxt = a + 4

        def rec(dst):
            if i.rcbit:
                E(f'guest::setCr0(c, {dst});')

        # --- branches -------------------------------------------------
        if op == 'b':
            t = i.target
            if i.lk:
                if t == nxt:
                    E(f'c.lr = {hx(nxt)};')
                else:
                    E(f'c.lr = {hx(nxt)}; {g.callee(t, a)}(c);')
            else:
                E(self.branch_to(a, t, False))
            return
        if op == 'bc':
            pre, cnd = self.cond(rd, ra)
            t = i.target
            if i.lk:
                E(f'{pre}if ({cnd}) {{ c.lr = {hx(nxt)}; {g.callee(t, a)}(c); }}')
            else:
                E(f'{pre}if ({cnd}) {{ {self.branch_to(a, t, False)} }}')
            return
        if op == 'bclr':
            pre, cnd = self.cond(rd, ra)
            if i.lk:
                E(f'{pre}if ({cnd}) {{ uint32_t t_ = c.lr; c.lr = {hx(nxt)}; guest::callIndirect(c, t_); }}')
            else:
                E(f'{pre}if ({cnd}) return;')
            return
        if op == 'bcctr':
            pre, cnd = self.cond(rd, ra)
            if i.lk:
                E(f'{pre}if ({cnd}) {{ c.lr = {hx(nxt)}; guest::callIndirect(c, c.ctr); }}')
                return
            jt = g.tables.get(a)
            if jt:
                E(f'{pre}if ({cnd}) {{')
                E('  switch (c.ctr) {')
                for t in sorted(set(jt['targets'])):
                    E(f'  case {hx(t)}: {self.branch_to(a, t, False)}')
                E('  default: guest::callIndirect(c, c.ctr); return;')
                E('  }')
                E('}')
            else:
                E(f'{pre}if ({cnd}) {{ guest::callIndirect(c, c.ctr); return; }}')
            return

        # --- integer arithmetic ---------------------------------------
        if op == 'addi':
            E(f'{R(rd)} = {RA0(ra)} + {hx(i.imm)};')
            return
        if op == 'addis':
            E(f'{R(rd)} = {RA0(ra)} + {hx(i.imm << 16)};')
            return
        if op in ('addic', 'addic.'):
            E(f'{{ uint32_t a_ = {R(ra)}; {R(rd)} = a_ + {hx(i.imm)}; c.ca = {R(rd)} < a_; }}')
            if op == 'addic.':
                E(f'guest::setCr0(c, {R(rd)});')
            return
        if op == 'subfic':
            E(f'{{ uint32_t a_ = {R(ra)}; {R(rd)} = {hx(i.imm)} - a_; c.ca = (uint64_t)(uint32_t)~a_ + {hx(i.imm)} + 1 > 0xFFFFFFFFull; }}')
            return
        if op == 'mulli':
            # Low 32 bits of the product are the same signed or unsigned; unsigned wraps (int32 overflow is UB).
            E(f'{R(rd)} = {R(ra)} * {hx(i.imm)};')
            return
        if op == 'cmpi':
            E(f'c.cr[{i.crf}] = guest::cmpS(c, (int32_t){R(ra)}, {i.imm});')
            return
        if op == 'cmpli':
            E(f'c.cr[{i.crf}] = guest::cmpU(c, {R(ra)}, {hx(i.imm)});')
            return
        if op == 'cmp':
            E(f'c.cr[{i.crf}] = guest::cmpS(c, (int32_t){R(ra)}, (int32_t){R(rb)});')
            return
        if op == 'cmpl':
            E(f'c.cr[{i.crf}] = guest::cmpU(c, {R(ra)}, {R(rb)});')
            return
        if op in ('ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.'):
            imm = i.imm << 16 if op in ('oris', 'xoris', 'andis.') else i.imm
            sym = {'o': '|', 'x': '^', 'a': '&'}[op[0]]
            if op == 'ori' and rd == 0 and ra == 0 and imm == 0:
                return  # nop
            E(f'{R(ra)} = {R(rd)} {sym} {hx(imm)};')
            if op.endswith('.'):
                E(f'guest::setCr0(c, {R(ra)});')
            return
        if op == 'rlwinm':
            m = mask32(i.mb, i.me)
            if i.imm == 0:
                E(f'{R(ra)} = {R(rd)} & {hx(m)};')
            else:
                E(f'{R(ra)} = guest::rotl({R(rd)}, {i.imm}) & {hx(m)};')
            rec(R(ra))
            return
        if op == 'rlwimi':
            m = mask32(i.mb, i.me)
            E(f'{R(ra)} = (guest::rotl({R(rd)}, {i.imm}) & {hx(m)}) | ({R(ra)} & {hx(~m)});')
            rec(R(ra))
            return
        if op == 'rlwnm':
            m = mask32(i.mb, i.me)
            E(f'{R(ra)} = guest::rotl({R(rd)}, {R(rb)} & 31) & {hx(m)};')
            rec(R(ra))
            return
        arith = {
            'add': '{a} + {b}', 'subf': '{b} - {a}', 'neg': '0u - {a}',
            'mullw': '{a} * {b}',  # unsigned: wraps like the hardware (see mulli)
            'mulhw': '(uint32_t)(((int64_t)(int32_t){a} * (int64_t)(int32_t){b}) >> 32)',
            'mulhwu': '(uint32_t)(((uint64_t){a} * (uint64_t){b}) >> 32)',
            'divw': 'guest::divwS((int32_t){a}, (int32_t){b})',
            'divwu': 'guest::divwU({a}, {b})',
        }
        if op in arith:
            if i.oe:
                E(f'/* OE ignored at {a:08X} */')
            E(f'{R(rd)} = ' + arith[op].format(a=R(ra), b=R(rb)) + ';')
            rec(R(rd))
            return
        carry = {
            'addc': ('{a}', '{b}', '0'), 'adde': ('{a}', '{b}', 'c.ca'),
            'subfc': ('~{a}', '{b}', '1'), 'subfe': ('~{a}', '{b}', 'c.ca'),
            'addme': ('{a}', '0xFFFFFFFFu', 'c.ca'), 'addze': ('{a}', '0u', 'c.ca'),
            'subfme': ('~{a}', '0xFFFFFFFFu', 'c.ca'), 'subfze': ('~{a}', '0u', 'c.ca'),
        }
        if op in carry:
            x, y, z = carry[op]
            x = x.format(a=R(ra), b=R(rb))
            y = y.format(a=R(ra), b=R(rb))
            E(f'{{ uint64_t s_ = (uint64_t)(uint32_t)({x}) + (uint64_t)(uint32_t)({y}) + {z}; '
              f'{R(rd)} = (uint32_t)s_; c.ca = (uint32_t)(s_ >> 32); }}')
            rec(R(rd))
            return
        logic = {'and': '{s} & {b}', 'andc': '{s} & ~{b}', 'or': '{s} | {b}', 'orc': '{s} | ~{b}',
                 'xor': '{s} ^ {b}', 'nand': '~({s} & {b})', 'nor': '~({s} | {b})', 'eqv': '~({s} ^ {b})',
                 'slw': '({b} & 0x20) ? 0u : ({s} << ({b} & 31))',
                 'srw': '({b} & 0x20) ? 0u : ({s} >> ({b} & 31))',
                 'cntlzw': 'guest::cntlz({s})', 'extsb': '(uint32_t)(int32_t)(int8_t){s}',
                 'extsh': '(uint32_t)(int32_t)(int16_t){s}'}
        if op in logic:
            if op == 'or' and rd == rb:
                E(f'{R(ra)} = {R(rd)};')
            else:
                E(f'{R(ra)} = ' + logic[op].format(s=R(rd), b=R(rb)) + ';')
            rec(R(ra))
            return
        if op == 'sraw':
            E(f'{R(ra)} = guest::sraw(c, {R(rd)}, {R(rb)});')
            rec(R(ra))
            return
        if op == 'srawi':
            E(f'{R(ra)} = guest::sraw(c, {R(rd)}, {i.imm});')
            rec(R(ra))
            return

        # --- loads/stores ----------------------------------------------
        ld = {'lwz': ('rd32', '', 32), 'lbz': ('rd8', '', 8), 'lhz': ('rd16', '', 16),
              'lha': ('rd16', '(uint32_t)(int32_t)(int16_t)', 16)}
        st = {'stw': 'wr32', 'stb': 'wr8', 'sth': 'wr16'}
        m = LDST_RE.fullmatch(op)
        if m:
            base, upd, indexed = m.group(1), bool(m.group(2)), bool(m.group(3))
            if indexed:
                ea = f'{R(ra)} + {R(rb)}' if upd else f'{RA0(ra)} + {R(rb)}'
            else:
                ea = f'{R(ra)} + {hx(i.imm)}' if upd else f'{RA0(ra)} + {hx(i.imm)}'
            s = f'{{ uint32_t ea_ = {ea}; '
            if base in ld:
                fn, cast, _ = ld[base]
                s += f'{R(rd)} = {cast}guest::{fn}(ea_); '
            elif base in st:
                s += f'guest::{st[base]}(ea_, {R(rd)}); '
            elif base == 'lfs':
                s += f'{F(rd)} = guest::asF32(guest::rd32(ea_)); {P1(rd)} = {F(rd)}; '
            elif base == 'lfd':
                s += f'c.f[{rd}].u = guest::rd64(ea_); '
            elif base == 'stfs':
                s += f'guest::wr32(ea_, guest::f32Bits((float){F(rd)})); '
            elif base == 'stfd':
                s += f'guest::wr64(ea_, c.f[{rd}].u); '
            if upd:
                s += f'{R(ra)} = ea_; '
            E(s + '}')
            return
        if op == 'stfiwx':
            E(f'guest::wr32({RA0(ra)} + {R(rb)}, (uint32_t)c.f[{rd}].u);')
            return
        if op in ('lwbrx', 'lhbrx'):
            fn = 'rd32' if op == 'lwbrx' else 'rd16'
            sw = 'bl_bswap32' if op == 'lwbrx' else 'bl_bswap16'
            E(f'{R(rd)} = {sw}(guest::{fn}({RA0(ra)} + {R(rb)}));')
            return
        if op in ('stwbrx', 'sthbrx'):
            if op == 'stwbrx':
                E(f'guest::wr32({RA0(ra)} + {R(rb)}, bl_bswap32({R(rd)}));')
            else:
                E(f'guest::wr16({RA0(ra)} + {R(rb)}, bl_bswap16((uint16_t){R(rd)}));')
            return
        if op == 'lwarx':
            E(f'{R(rd)} = guest::rd32({RA0(ra)} + {R(rb)});')
            return
        if op == 'stwcx.':
            E(f'guest::wr32({RA0(ra)} + {R(rb)}, {R(rd)}); c.cr[0] = 2 | (c.so & 1);')
            return
        if op == 'lmw':
            E(f'{{ uint32_t ea_ = {RA0(ra)} + {hx(i.imm)}; for (int k_ = {rd}; k_ < 32; k_++, ea_ += 4) c.r[k_] = guest::rd32(ea_); }}')
            return
        if op == 'stmw':
            E(f'{{ uint32_t ea_ = {RA0(ra)} + {hx(i.imm)}; for (int k_ = {rd}; k_ < 32; k_++, ea_ += 4) guest::wr32(ea_, c.r[k_]); }}')
            return
        if op in ('lswi', 'stswi'):
            n = i.imm
            if op == 'lswi':
                E(f'{{ uint32_t ea_ = {RA0(ra)}; int r_ = {rd}; for (int k_ = 0; k_ < {n}; k_++) {{ '
                  f'if ((k_ & 3) == 0) {{ if (k_) r_ = (r_ + 1) & 31; c.r[r_] = 0; }} '
                  f'c.r[r_] |= (uint32_t)guest::rd8(ea_ + k_) << (24 - 8 * (k_ & 3)); }} }}')
            else:
                E(f'{{ uint32_t ea_ = {RA0(ra)}; int r_ = {rd}; for (int k_ = 0; k_ < {n}; k_++) {{ '
                  f'if (k_ && (k_ & 3) == 0) r_ = (r_ + 1) & 31; '
                  f'guest::wr8(ea_ + k_, (uint8_t)(c.r[r_] >> (24 - 8 * (k_ & 3)))); }} }}')
            return

        # --- paired-single loads/stores ----------------------------------
        if op in ('psq_l', 'psq_lu', 'psq_st', 'psq_stu'):
            ea = f'{RA0(ra)} + {hx(i.imm)}' if not op.endswith('u') else f'{R(ra)} + {hx(i.imm)}'
            fn = 'psqLoad' if op.startswith('psq_l') else 'psqStore'
            s = f'{{ uint32_t ea_ = {ea}; guest::{fn}(c, {rd}, ea_, {i.w}, {i.i}); '
            if op.endswith('u'):
                s += f'{R(ra)} = ea_; '
            E(s + '}')
            return
        if op in ('psq_lx', 'psq_lux', 'psq_stx', 'psq_stux'):
            upd = 'ux' in op
            ea = f'{R(ra)} + {R(rb)}' if upd else f'{RA0(ra)} + {R(rb)}'
            fn = 'psqLoad' if op.startswith('psq_l') else 'psqStore'
            s = f'{{ uint32_t ea_ = {ea}; guest::{fn}(c, {rd}, ea_, {i.w}, {i.i}); '
            if upd:
                s += f'{R(ra)} = ea_; '
            E(s + '}')
            return

        # --- condition register ------------------------------------------
        crops = {'crand': '{a} & {b}', 'crandc': '{a} & !{b}', 'creqv': '!({a} ^ {b})',
                 'crnand': '!({a} & {b})', 'crnor': '!({a} | {b})', 'cror': '{a} | {b}',
                 'crorc': '{a} | !{b}', 'crxor': '{a} ^ {b}'}
        if op in crops:
            def bit(n):
                return f'((c.cr[{n >> 2}] >> {3 - (n & 3)}) & 1)'
            expr = crops[op].format(a=bit(ra), b=bit(rb))
            m = 8 >> (rd & 3)
            E(f'{{ uint32_t v_ = ({expr}) & 1; c.cr[{rd >> 2}] = (uint8_t)((c.cr[{rd >> 2}] & ~{m}) | (v_ ? {m} : 0)); }}')
            return
        if op == 'mcrf':
            E(f'c.cr[{i.crf}] = c.cr[{i.crfs}];')
            return
        if op == 'mfcr':
            E(f'{R(rd)} = ' + ' | '.join(f'((uint32_t)c.cr[{k}] << {28 - 4 * k})' for k in range(8)) + ';')
            return
        if op == 'mtcrf':
            for k in range(8):
                if i.imm & (0x80 >> k):
                    E(f'c.cr[{k}] = ({R(rd)} >> {28 - 4 * k}) & 15;')
            return
        if op == 'mcrxr':
            E(f'c.cr[{i.crf}] = (uint8_t)((c.so << 3) | (c.ov << 2) | (c.ca << 1)); c.so = c.ov = c.ca = 0;')
            return

        # --- special registers ---------------------------------------
        if op == 'mfspr':
            if i.spr == 8:
                E(f'{R(rd)} = c.lr;')
            elif i.spr == 9:
                E(f'{R(rd)} = c.ctr;')
            elif 912 <= i.spr <= 919:
                E(f'{R(rd)} = c.gqr[{i.spr - 912}];')
            else:
                E(f'{R(rd)} = guest::readSpr(c, {i.spr}, {hx(a)});')
            return
        if op == 'mtspr':
            if i.spr == 8:
                E(f'c.lr = {R(rd)};')
            elif i.spr == 9:
                E(f'c.ctr = {R(rd)};')
            elif 912 <= i.spr <= 919:
                E(f'c.gqr[{i.spr - 912}] = {R(rd)};')
            else:
                E(f'guest::writeSpr(c, {i.spr}, {R(rd)}, {hx(a)});')
            return
        if op == 'mftb':
            E(f'{R(rd)} = (uint32_t)(guest::timeBase() >> {32 if i.spr == 269 else 0});')
            return
        if op == 'mfmsr':
            E(f'{R(rd)} = c.msr;')
            return
        if op == 'mtmsr':
            E(f'guest::writeMsr(c, {R(rd)});')
            return
        if op in ('mfsr', 'mfsrin'):
            E(f'{R(rd)} = 0;')
            return
        if op in ('mtsr', 'mtsrin', 'dcbf', 'dcbst', 'dcbt', 'dcbtst', 'dcbi', 'icbi', 'sync',
                  'isync', 'eieio', 'tlbsync', 'tlbie'):
            return
        if op == 'dcbz':
            E(f'{{ uint32_t ea_ = ({RA0(ra)} + {R(rb)}) & ~31u; memset(guest::hostp(ea_), 0, 32); }}')
            return
        if op == 'dcbz_l':
            E(f'guest::dcbzL(c, {RA0(ra)} + {R(rb)});')
            return
        if op == 'sc':
            return  # the SDK's only system call is a sync/isync helper
        if op == 'rfi':
            E('guest::rfi(c); return;')
            return
        if op in ( 'tw', 'twi', 'illegal', 'eciwx', 'ecowx', 'lswx', 'stswx'):
            E(f'guest::trap(c, {hx(a)}, "{op}");')
            return

        # --- floating point ----------------------------------------------
        fbin = {'fadd': '{a} + {b}', 'fsub': '{a} - {b}', 'fmul': '{a} * {c}', 'fdiv': '{a} / {b}',
                'fmadd': '{a} * {c} + {b}', 'fmsub': '{a} * {c} - {b}',
                'fnmadd': '-({a} * {c} + {b})', 'fnmsub': '-({a} * {c} - {b})'}
        if op in fbin:
            E(f'{F(rd)} = ' + fbin[op].format(a=F(ra), b=F(rb), c=F(rc)) + ';')
            return
        if op.endswith('s') and op[:-1] in fbin:
            E(f'{F(rd)} = guest::roundSingle(' + fbin[op[:-1]].format(a=F(ra), b=F(rb), c=F(rc)) + f'); {P1(rd)} = {F(rd)};')
            return
        if op == 'fres':
            E(f'{F(rd)} = guest::fres({F(rb)}); {P1(rd)} = {F(rd)};')
            return
        if op == 'frsqrte':
            E(f'{F(rd)} = guest::frsqrte({F(rb)});')
            return
        if op == 'fsel':
            E(f'{F(rd)} = guest::fsel({F(ra)}, {F(rc)}, {F(rb)});')
            return
        if op == 'frsp':
            E(f'{F(rd)} = guest::roundSingle({F(rb)}); {P1(rd)} = {F(rd)};')
            return
        if op in ('fctiw', 'fctiwz'):
            E(f'c.f[{rd}].u = guest::fctiw(c, {F(rb)}, {"true" if op == "fctiwz" else "false"});')
            return
        if op == 'fmr':
            E(f'c.f[{rd}].u = c.f[{rb}].u;')
            return
        if op == 'fneg':
            E(f'c.f[{rd}].u = c.f[{rb}].u ^ 0x8000000000000000ull;')
            return
        if op == 'fabs':
            E(f'c.f[{rd}].u = c.f[{rb}].u & 0x7FFFFFFFFFFFFFFFull;')
            return
        if op == 'fnabs':
            E(f'c.f[{rd}].u = c.f[{rb}].u | 0x8000000000000000ull;')
            return
        if op in ('fcmpu', 'fcmpo'):
            E(f'c.cr[{i.crf}] = guest::cmpF({F(ra)}, {F(rb)});')
            return
        if op == 'mffs':
            E(f'c.f[{rd}].u = c.fpscr;')
            return
        if op == 'mtfsf':
            m = 0
            for k in range(8):
                if i.imm & (0x80 >> k):
                    m |= 0xF << (28 - 4 * k)
            E(f'c.fpscr = (c.fpscr & ~{hx(m)}) | ((uint32_t)c.f[{rb}].u & {hx(m)});')
            return
        if op == 'mtfsb0':
            E(f'c.fpscr &= ~{hx(0x80000000 >> rd)};')
            return
        if op == 'mtfsb1':
            E(f'c.fpscr |= {hx(0x80000000 >> rd)};')
            return
        if op == 'mtfsfi':
            sh = 28 - 4 * i.crf
            E(f'c.fpscr = (c.fpscr & ~{hx(0xF << sh)}) | {hx(i.imm << sh)};')
            return
        if op == 'mcrfs':
            sh = 28 - 4 * i.crfs
            E(f'c.cr[{i.crf}] = (c.fpscr >> {sh}) & 15;')
            return

        # --- paired singles -----------------------------------------------
        if op.startswith('ps_'):
            self.paired(i)
            return
        E(f'guest::trap(c, {hx(a)}, "unhandled {op}");')

    def paired(self, i):
        E = self.emit
        op = i.op[3:]
        d, a, b, cc = i.rd, i.ra, i.rb, i.rc
        A0, A1 = f'c.f[{a}].d', f'c.ps1[{a}]'
        B0, B1 = f'c.f[{b}].d', f'c.ps1[{b}]'
        C0, C1 = f'c.f[{cc}].d', f'c.ps1[{cc}]'
        rs = 'guest::roundSingle'

        def set2(x0, x1, rnd=True):
            if rnd:
                E(f'{{ double t0_ = {rs}({x0}), t1_ = {rs}({x1}); c.f[{d}].d = t0_; c.ps1[{d}] = t1_; }}')
            else:
                E(f'{{ double t0_ = {x0}, t1_ = {x1}; c.f[{d}].d = t0_; c.ps1[{d}] = t1_; }}')

        two = {'add': '{x} + {y}', 'sub': '{x} - {y}', 'div': '{x} / {y}'}
        if op in two:
            set2(two[op].format(x=A0, y=B0), two[op].format(x=A1, y=B1))
        elif op == 'mul':
            set2(f'{A0} * {C0}', f'{A1} * {C1}')
        elif op == 'madd':
            set2(f'{A0} * {C0} + {B0}', f'{A1} * {C1} + {B1}')
        elif op == 'msub':
            set2(f'{A0} * {C0} - {B0}', f'{A1} * {C1} - {B1}')
        elif op == 'nmadd':
            set2(f'-({A0} * {C0} + {B0})', f'-({A1} * {C1} + {B1})')
        elif op == 'nmsub':
            set2(f'-({A0} * {C0} - {B0})', f'-({A1} * {C1} - {B1})')
        elif op == 'muls0':
            set2(f'{A0} * {C0}', f'{A1} * {C0}')
        elif op == 'muls1':
            set2(f'{A0} * {C1}', f'{A1} * {C1}')
        elif op == 'madds0':
            set2(f'{A0} * {C0} + {B0}', f'{A1} * {C0} + {B1}')
        elif op == 'madds1':
            set2(f'{A0} * {C1} + {B0}', f'{A1} * {C1} + {B1}')
        elif op == 'sum0':
            set2(f'{A0} + {B1}', C1)
        elif op == 'sum1':
            set2(C0, f'{A0} + {B1}')
        elif op == 'sel':
            set2(f'guest::fsel({A0}, {C0}, {B0})', f'guest::fsel({A1}, {C1}, {B1})', False)
        elif op == 'res':
            set2(f'guest::fres({B0})', f'guest::fres({B1})')
        elif op == 'rsqrte':
            set2(f'guest::frsqrte({B0})', f'guest::frsqrte({B1})')
        elif op == 'neg':
            set2(f'-{B0}', f'-{B1}', False)
        elif op == 'abs':
            set2(f'std::fabs({B0})', f'std::fabs({B1})', False)
        elif op == 'nabs':
            set2(f'-std::fabs({B0})', f'-std::fabs({B1})', False)
        elif op == 'mr':
            set2(B0, B1, False)
        elif op.startswith('merge'):
            x = A0 if op[5] == '0' else A1
            y = B0 if op[6] == '0' else B1
            set2(x, y, False)
        elif op in ('cmpu0', 'cmpo0'):
            E(f'c.cr[{i.crf}] = guest::cmpF({A0}, {B0});')
        elif op in ('cmpu1', 'cmpo1'):
            E(f'c.cr[{i.crf}] = guest::cmpF({A1}, {B1});')
        else:
            E(f'guest::trap(c, 0x{i.addr:08X}u, "unhandled ps_{op}");')


def dol_hash(path):
    """FNV-1a 64 of the DOL file: ties a game module to the executable it was built from."""
    h = 0xCBF29CE484222325
    for b in open(path, 'rb').read():
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f'{h:016x}'


class Gen:
    def __init__(self, dol, analysis, symbols, hooks):
        self.dol = dol
        self.dol_hash = ''
        self.funcs = analysis['functions']
        self.tables = {int(k): v for k, v in analysis['jump_tables'].items()}
        self.symbols = symbols
        self.starts = {f['addr']: f for f in self.funcs}
        self.insns = {}
        for _, addr, data, _ in dol.text_sections():
            for o in range(0, len(data) - 3, 4):
                w = int.from_bytes(data[o:o + 4], 'big')
                self.insns[addr + o] = decode(w, addr + o)
        by_name = {v: k for k, v in symbols.items()}
        self.hooks = {}
        self.prehooks = {}  # '?name = HLE': host check first, original body unless it sets r0 = 1
        for h in hooks:
            name, _, hle = (x.strip() for x in h.partition('='))
            pre = name.startswith('?')
            name = name.lstrip('?').strip()
            addr = int(name, 16) if name.startswith('0x') else by_name.get(name)
            if addr is None:
                print(f'warning: hook {name} has no address', file=sys.stderr)
                continue
            (self.prehooks if pre else self.hooks)[addr] = hle or symbols.get(addr) or f'fn_{addr:08X}'
        self.sorted_starts = sorted(self.starts)
        self.extra_entries = {}
        self.unresolved = set()
        self._find_entries()

    def owner(self, t):
        import bisect
        k = bisect.bisect_right(self.sorted_starts, t) - 1
        if k < 0:
            return None
        f = self.funcs_by_start(self.sorted_starts[k])
        if f['addr'] <= t < f['addr'] + f['size']:
            return f['addr']
        return None

    def funcs_by_start(self, s):
        return self.starts[s]

    def _find_entries(self):
        """Branches (not calls) into the middle of another function need entry points."""
        for f in self.funcs:
            s, e = f['addr'], f['addr'] + f['size']
            for a in range(s, e, 4):
                i = self.insns[a]
                ts = []
                if i.op in ('b', 'bc'):
                    ts = [i.target]
                elif i.op == 'bcctr' and a in self.tables:
                    ts = self.tables[a]['targets']
                for t in ts:
                    if s <= t < e or t in self.starts:
                        continue
                    o = self.owner(t)
                    if o is None:
                        self.unresolved.add(t)
                    else:
                        self.extra_entries.setdefault(o, set()).add(t)

    def fname(self, a):
        return f'f_{a:08X}'

    def callee(self, t, frm):
        if t in self.starts:
            return self.fname(t)
        for o, ents in self.extra_entries.items():
            if t in ents:
                return self.fname(t)
        return f'/*unresolved {t:08X} from {frm:08X}*/guest_unresolved'

    def all_entry_addrs(self):
        out = set(self.starts)
        for ents in self.extra_entries.values():
            out |= ents
        return sorted(out)

    def write(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        entries = self.all_entry_addrs()
        with open(os.path.join(outdir, 'recomp_funcs.h'), 'w', encoding='utf-8', newline='\n') as h:
            h.write('// Generated by tools/recomp/recomp.py. Do not edit.\n#pragma once\n#include "ctx.h"\n\n')
            for a in entries:
                h.write(f'void {self.fname(a)}(guest::Ctx& c);\n')
            h.write('void guest_unresolved(guest::Ctx& c);\n')
            hle_names = sorted(set(self.hooks.values()) | set(self.prehooks.values()))
            for n in hle_names:
                h.write(f'void HLE_{n}(guest::Ctx& c);\n')
        chunks = [[] for _ in range(FILES)]
        for k, f in enumerate(self.funcs):
            chunks[k * FILES // len(self.funcs)].append(f)
        for n, chunk in enumerate(chunks):
            with open(os.path.join(outdir, f'recomp_{n:02d}.cpp'), 'w', encoding='utf-8', newline='\n') as out:
                out.write('// Generated by tools/recomp/recomp.py. Do not edit.\n#include "recomp_funcs.h"\n\n')
                for f in chunk:
                    a = f['addr']
                    sym = self.symbols.get(a)
                    if sym:
                        out.write(f'// {sym}\n')
                    if a in self.hooks:
                        out.write(f'void {self.fname(a)}(guest::Ctx& c) {{ HLE_{self.hooks[a]}(c); }}\n')
                        for t in sorted(self.extra_entries.get(a, ())):
                            out.write(f'void {self.fname(t)}(guest::Ctx& c) {{ guest::trap(c, 0x{t:08X}u, "entry into HLE function"); }}\n')
                        continue
                    out.write('\n'.join(FuncGen(self, f).gen()) + '\n\n')
        hle_names = sorted(set(self.hooks.values()) | set(self.prehooks.values()))
        with open(os.path.join(outdir, 'recomp_table.cpp'), 'w', encoding='utf-8', newline='\n') as t:
            t.write('// Generated by tools/recomp/recomp.py. Do not edit.\n#include "recomp_funcs.h"\n#include "module_abi.h"\n\n')
            t.write('namespace {\nconst guest::FuncEntry kTable[] = {\n')
            for a in entries:
                t.write(f'  {{0x{a:08X}u, {self.fname(a)}}},\n')
            t.write('};\n')
            t.write('const char* const kHleNames[] = {\n')
            for n in hle_names:
                t.write(f'  "{n}",\n')
            t.write('};\n')
            t.write('guest::TraceRing* traceRing() { return &guest::t_trace; }\n')
            t.write('const guest::GameModule kModule = {guest::kModuleAbiVersion, kTable, '
                    f'{len(entries)}u, 0x{self.dol.entry:08X}u, kHleNames, {len(hle_names)}u, traceRing, "{self.dol_hash}"}};\n')
            t.write('}  // namespace\n\nconst guest::GameModule* guest::builtinModule() { return &kModule; }\n')
        # Module build: HLE calls go through the table the runtime hands over at load.
        with open(os.path.join(outdir, 'recomp_hle.cpp'), 'w', encoding='utf-8', newline='\n') as h:
            h.write('// Generated by tools/recomp/recomp.py. Do not edit. Module builds only.\n#include "recomp_funcs.h"\n\n')
            h.write('namespace guest {\nextern const Fn* g_moduleHle;\n}\n\n')
            for k, n in enumerate(hle_names):
                h.write(f'void HLE_{n}(guest::Ctx& c) {{ guest::g_moduleHle[{k}](c); }}\n')
        print(f'{len(self.funcs)} functions, {sum(len(v) for v in self.extra_entries.values())} extra entries, '
              f'{len(self.hooks)} hooks, {len(self.unresolved)} unresolved targets')
        for u in sorted(self.unresolved)[:20]:
            print(f'  unresolved {u:08X}')


def main():
    dol = Dol(sys.argv[1])
    analysis = json.load(open(sys.argv[2], encoding='utf-8'))
    symbols = load_symbols(sys.argv[3])
    hooks = load_hooks(sys.argv[4])
    gen = Gen(dol, analysis, symbols, hooks)
    gen.dol_hash = dol_hash(sys.argv[1])
    gen.write(sys.argv[5])


if __name__ == '__main__':
    main()
