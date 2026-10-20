"""Gekko (GameCube PowerPC 750CL) instruction decoder.

decode(word, addr) returns an Insn with a mnemonic and the raw operand fields the
recompiler needs. Unknown encodings decode as mnemonic 'illegal'.
"""
from dataclasses import dataclass, field


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


@dataclass
class Insn:
    addr: int
    word: int
    op: str
    rd: int = 0      # rD / rS / frD / frS / crbD / BO
    ra: int = 0      # rA / frA / crbA / BI
    rb: int = 0      # rB / frB / crbB
    rc: int = 0      # frC
    imm: int = 0     # SIMM/UIMM/d/branch target/SH etc.
    mb: int = 0
    me: int = 0
    rcbit: bool = False   # record bit (Rc), also 'l' for loads
    oe: bool = False
    lk: bool = False
    aa: bool = False
    crf: int = 0
    crfs: int = 0
    w: int = 0       # paired single W
    i: int = 0       # paired single GQR index
    spr: int = 0
    extra: dict = field(default_factory=dict)

    @property
    def target(self):
        return self.imm


def _d(w):
    return sext(w & 0xFFFF, 16)


def decode(w, addr):
    op = w >> 26
    rd = (w >> 21) & 31
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    rc = (w >> 6) & 31
    rcb = bool(w & 1)
    I = lambda name, **kw: Insn(addr, w, name, **kw)

    if op == 18:
        li = sext(w & 0x03FFFFFC, 26)
        aa = bool(w & 2)
        return I('b', imm=(li if aa else (addr + li)) & 0xFFFFFFFF, aa=aa, lk=bool(w & 1))
    if op == 16:
        bd = sext(w & 0xFFFC, 16)
        aa = bool(w & 2)
        return I('bc', rd=rd, ra=ra, imm=(bd if aa else (addr + bd)) & 0xFFFFFFFF, aa=aa, lk=bool(w & 1))
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return I('bclr', rd=rd, ra=ra, lk=bool(w & 1))
        if xo == 528:
            return I('bcctr', rd=rd, ra=ra, lk=bool(w & 1))
        if xo == 0:
            return I('mcrf', crf=rd >> 2, crfs=ra >> 2)
        names = {257: 'crand', 129: 'crandc', 289: 'creqv', 225: 'crnand', 33: 'crnor',
                 449: 'cror', 417: 'crorc', 193: 'crxor'}
        if xo in names:
            return I(names[xo], rd=rd, ra=ra, rb=rb)
        if xo == 150:
            return I('isync')
        if xo == 50:
            return I('rfi')
        return I('illegal')
    if op == 17:
        return I('sc')
    simple = {14: 'addi', 15: 'addis', 12: 'addic', 13: 'addic.', 8: 'subfic', 7: 'mulli'}
    if op in simple:
        return I(simple[op], rd=rd, ra=ra, imm=_d(w))
    if op == 11:
        return I('cmpi', crf=rd >> 2, ra=ra, imm=_d(w))
    if op == 10:
        return I('cmpli', crf=rd >> 2, ra=ra, imm=w & 0xFFFF)
    logi = {24: 'ori', 25: 'oris', 26: 'xori', 27: 'xoris', 28: 'andi.', 29: 'andis.'}
    if op in logi:
        return I(logi[op], rd=rd, ra=ra, imm=w & 0xFFFF)
    if op == 20:
        return I('rlwimi', rd=rd, ra=ra, imm=rb, mb=rc, me=(w >> 1) & 31, rcbit=rcb)
    if op == 21:
        return I('rlwinm', rd=rd, ra=ra, imm=rb, mb=rc, me=(w >> 1) & 31, rcbit=rcb)
    if op == 23:
        return I('rlwnm', rd=rd, ra=ra, rb=rb, mb=rc, me=(w >> 1) & 31, rcbit=rcb)
    mem = {32: 'lwz', 33: 'lwzu', 34: 'lbz', 35: 'lbzu', 40: 'lhz', 41: 'lhzu', 42: 'lha', 43: 'lhau',
           36: 'stw', 37: 'stwu', 38: 'stb', 39: 'stbu', 44: 'sth', 45: 'sthu', 46: 'lmw', 47: 'stmw',
           48: 'lfs', 49: 'lfsu', 50: 'lfd', 51: 'lfdu', 52: 'stfs', 53: 'stfsu', 54: 'stfd', 55: 'stfdu'}
    if op in mem:
        return I(mem[op], rd=rd, ra=ra, imm=_d(w))
    if op == 56 or op == 57 or op == 60 or op == 61:
        names = {56: 'psq_l', 57: 'psq_lu', 60: 'psq_st', 61: 'psq_stu'}
        return I(names[op], rd=rd, ra=ra, imm=sext(w & 0xFFF, 12), w=(w >> 15) & 1, i=(w >> 12) & 7)
    if op == 3:
        return I('twi', rd=rd, ra=ra, imm=_d(w))
    if op == 31:
        return _decode31(w, addr, rd, ra, rb, rcb)
    if op == 59:
        xo = (w >> 1) & 31
        names = {21: 'fadds', 20: 'fsubs', 25: 'fmuls', 18: 'fdivs', 24: 'fres', 29: 'fmadds',
                 28: 'fmsubs', 31: 'fnmadds', 30: 'fnmsubs'}
        if xo in names:
            return I(names[xo], rd=rd, ra=ra, rb=rb, rc=rc, rcbit=rcb)
        return I('illegal')
    if op == 63:
        xo5 = (w >> 1) & 31
        names5 = {21: 'fadd', 20: 'fsub', 25: 'fmul', 18: 'fdiv', 26: 'frsqrte', 29: 'fmadd',
                  28: 'fmsub', 31: 'fnmadd', 30: 'fnmsub', 23: 'fsel'}
        if xo5 in names5:
            return I(names5[xo5], rd=rd, ra=ra, rb=rb, rc=rc, rcbit=rcb)
        xo = (w >> 1) & 0x3FF
        names = {0: 'fcmpu', 32: 'fcmpo'}
        if xo in names:
            return I(names[xo], crf=rd >> 2, ra=ra, rb=rb)
        n2 = {12: 'frsp', 14: 'fctiw', 15: 'fctiwz', 40: 'fneg', 72: 'fmr', 136: 'fnabs', 264: 'fabs'}
        if xo in n2:
            return I(n2[xo], rd=rd, rb=rb, rcbit=rcb)
        if xo == 583:
            return I('mffs', rd=rd, rcbit=rcb)
        if xo == 711:
            return I('mtfsf', imm=(w >> 17) & 0xFF, rb=rb, rcbit=rcb)
        if xo == 70:
            return I('mtfsb0', rd=rd, rcbit=rcb)
        if xo == 38:
            return I('mtfsb1', rd=rd, rcbit=rcb)
        if xo == 134:
            return I('mtfsfi', crf=rd >> 2, imm=(w >> 12) & 15, rcbit=rcb)
        if xo == 64:
            return I('mcrfs', crf=rd >> 2, crfs=ra >> 2)
        return I('illegal')
    if op == 4:
        return _decode4(w, addr, rd, ra, rb, rc, rcb)
    return I('illegal')


def _decode4(w, addr, rd, ra, rb, rc, rcb):
    I = lambda name, **kw: Insn(addr, w, name, **kw)
    xo6 = (w >> 1) & 0x3F
    if xo6 == 6 or xo6 == 7 or xo6 == 38 or xo6 == 39:
        names = {6: 'psq_lx', 7: 'psq_stx', 38: 'psq_lux', 39: 'psq_stux'}
        return I(names[xo6], rd=rd, ra=ra, rb=rb, w=(w >> 10) & 1, i=(w >> 7) & 7)
    xo5 = (w >> 1) & 31
    n5 = {18: 'ps_div', 20: 'ps_sub', 21: 'ps_add', 23: 'ps_sel', 24: 'ps_res', 25: 'ps_mul',
          26: 'ps_rsqrte', 28: 'ps_msub', 29: 'ps_madd', 30: 'ps_nmsub', 31: 'ps_nmadd',
          10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1', 14: 'ps_madds0', 15: 'ps_madds1'}
    if xo5 in n5:
        return I(n5[xo5], rd=rd, ra=ra, rb=rb, rc=rc, rcbit=rcb)
    xo = (w >> 1) & 0x3FF
    n10 = {40: 'ps_neg', 72: 'ps_mr', 136: 'ps_nabs', 264: 'ps_abs',
           528: 'ps_merge00', 560: 'ps_merge01', 592: 'ps_merge10', 624: 'ps_merge11'}
    if xo in n10:
        return I(n10[xo], rd=rd, ra=ra, rb=rb, rcbit=rcb)
    nc = {0: 'ps_cmpu0', 32: 'ps_cmpo0', 64: 'ps_cmpu1', 96: 'ps_cmpo1'}
    if xo in nc:
        return I(nc[xo], crf=rd >> 2, ra=ra, rb=rb)
    if xo == 1014:
        return I('dcbz_l', ra=ra, rb=rb)
    return I('illegal')


_X31 = {
    # xo: (name, form)   forms: 'arith' rd,ra,rb (+OE,Rc) ; 'logic' rs->ra,rb ; 'mem' rd,ra,rb
    266: ('add', 'arith'), 10: ('addc', 'arith'), 138: ('adde', 'arith'), 234: ('addme', 'arith'),
    202: ('addze', 'arith'), 40: ('subf', 'arith'), 8: ('subfc', 'arith'), 136: ('subfe', 'arith'),
    232: ('subfme', 'arith'), 200: ('subfze', 'arith'), 104: ('neg', 'arith'),
    235: ('mullw', 'arith'), 75: ('mulhw', 'arith'), 11: ('mulhwu', 'arith'),
    491: ('divw', 'arith'), 459: ('divwu', 'arith'),
}
_X31_9 = {75, 11}  # mulhw/mulhwu have no OE

_LOGIC = {28: 'and', 60: 'andc', 444: 'or', 412: 'orc', 316: 'xor', 476: 'nand', 124: 'nor',
          284: 'eqv', 24: 'slw', 536: 'srw', 792: 'sraw', 26: 'cntlzw', 954: 'extsb', 922: 'extsh'}

_MEMX = {23: 'lwzx', 55: 'lwzux', 87: 'lbzx', 119: 'lbzux', 279: 'lhzx', 311: 'lhzux', 343: 'lhax',
         375: 'lhaux', 151: 'stwx', 183: 'stwux', 215: 'stbx', 247: 'stbux', 407: 'sthx', 439: 'sthux',
         535: 'lfsx', 567: 'lfsux', 599: 'lfdx', 631: 'lfdux', 663: 'stfsx', 695: 'stfsux',
         727: 'stfdx', 759: 'stfdux', 983: 'stfiwx', 534: 'lwbrx', 662: 'stwbrx', 790: 'lhbrx',
         918: 'sthbrx', 20: 'lwarx', 150: 'stwcx.', 533: 'lswx', 661: 'stswx'}


def _decode31(w, addr, rd, ra, rb, rcb):
    I = lambda name, **kw: Insn(addr, w, name, **kw)
    xo10 = (w >> 1) & 0x3FF
    xo9 = (w >> 1) & 0x1FF
    if xo9 in _X31 and (xo9 not in _X31_9 or not (w & 0x400)):
        name, _ = _X31[xo9]
        return I(name, rd=rd, ra=ra, rb=rb, oe=bool(w & 0x400), rcbit=rcb)
    if xo10 in _LOGIC:
        return I(_LOGIC[xo10], rd=rd, ra=ra, rb=rb, rcbit=rcb)
    if xo10 == 824:
        return I('srawi', rd=rd, ra=ra, imm=rb, rcbit=rcb)
    if xo10 in _MEMX:
        return I(_MEMX[xo10], rd=rd, ra=ra, rb=rb, rcbit=rcb)
    if xo10 == 597:
        return I('lswi', rd=rd, ra=ra, imm=rb or 32)
    if xo10 == 725:
        return I('stswi', rd=rd, ra=ra, imm=rb or 32)
    if xo10 == 0:
        return I('cmp', crf=rd >> 2, ra=ra, rb=rb)
    if xo10 == 32:
        return I('cmpl', crf=rd >> 2, ra=ra, rb=rb)
    if xo10 == 339 or xo10 == 467:
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        return I('mfspr' if xo10 == 339 else 'mtspr', rd=rd, spr=spr)
    if xo10 == 371:
        spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
        return I('mftb', rd=rd, spr=spr)
    if xo10 == 19:
        return I('mfcr', rd=rd)
    if xo10 == 144:
        return I('mtcrf', rd=rd, imm=(w >> 12) & 0xFF)
    if xo10 == 83:
        return I('mfmsr', rd=rd)
    if xo10 == 146:
        return I('mtmsr', rd=rd)
    if xo10 == 595:
        return I('mfsr', rd=rd, imm=ra & 15)
    if xo10 == 210:
        return I('mtsr', rd=rd, imm=ra & 15)
    if xo10 == 659:
        return I('mfsrin', rd=rd, rb=rb)
    if xo10 == 242:
        return I('mtsrin', rd=rd, rb=rb)
    if xo10 == 512:
        return I('mcrxr', crf=rd >> 2)
    if xo10 == 4:
        return I('tw', rd=rd, ra=ra, rb=rb)
    cache = {86: 'dcbf', 54: 'dcbst', 278: 'dcbt', 246: 'dcbtst', 1014: 'dcbz', 470: 'dcbi',
             982: 'icbi', 598: 'sync', 854: 'eieio', 566: 'tlbsync', 306: 'tlbie'}
    if xo10 in cache:
        return I(cache[xo10], ra=ra, rb=rb)
    if xo10 == 310:
        return I('eciwx', rd=rd, ra=ra, rb=rb)
    if xo10 == 438:
        return I('ecowx', rd=rd, ra=ra, rb=rb)
    return I('illegal')


BRANCH_OPS = {'b', 'bc', 'bclr', 'bcctr'}
