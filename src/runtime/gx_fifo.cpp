#include "gx_fifo.h"

#include "hw.h"
#include "log.h"
#include "memory.h"
#include "sched.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace gxfifo {

namespace {

constexpr int kInterruptCp = 17;
constexpr int kInterruptPeToken = 18;
constexpr int kInterruptPeFinish = 19;

Sink* g_sink = nullptr;
Stats g_stats{};

// --- registers ---------------------------------------------------------------------
uint16_t g_cp[0x40];   // CP (0xCC000000), 16-bit regs by offset / 2
uint16_t g_pe[0x40];   // PE (0xCC001000)
uint32_t g_piFifoBase = 0, g_piFifoEnd = 0, g_piFifoWptr = 0;

uint16_t g_peToken = 0;
bool g_cpBreakpointHit = false;

bool gpReading() { return (g_cp[0x02 / 2] & 0x11) == 0x11; }  // GP read enable + CPU/GP link

// --- command parser ---------------------------------------------------------------
// CP state needed to size draw commands (taken from the command stream).
uint32_t g_vcdLo = 0, g_vcdHi = 0;
uint32_t g_vat[3][8];

std::vector<uint8_t> g_stream;  // bytes not yet forming a complete command
std::mutex g_parseMutex;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

uint32_t field(uint32_t v, int bits, int shift) { return (v >> shift) & ((1u << bits) - 1); }

uint32_t compSize(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }

uint32_t colorSize(uint32_t fmt) {
  static const uint8_t sizes[8] = {2, 3, 4, 2, 3, 4, 4, 4};
  return sizes[fmt & 7];
}

void raise(int interrupt) {
  sched::postIrq([interrupt](guest::Ctx& c) {
    uint32_t handler = sched::callGuest(c, hw::kGetInterruptHandler, {(uint32_t)interrupt});
    if (handler) sched::callGuest(c, handler, {(uint32_t)interrupt, 0});
  });
}

void bpWrite(uint32_t v) {
  uint32_t reg = v >> 24;
  if (reg == 0x52 && (v & (1u << 14))) g_stats.displayCopies++;
  if (reg == 0x45 || reg == 0x47 || reg == 0x48)
    LOG_DEBUG("GX PE event BP %02X value %06X intsr %04X", reg, v & 0xFFFFFF, g_pe[0x0A / 2]);
  if (reg == 0x45) {  // PE_DONE: draw done
    if (g_pe[0x0A / 2] & 2) {
      g_pe[0x0A / 2] |= 8;
      raise(kInterruptPeFinish);
    }
  } else if (reg == 0x47 || reg == 0x48) {  // PE_TOKEN_INT / PE_TOKEN
    g_peToken = (uint16_t)v;
    if (reg == 0x47 && (g_pe[0x0A / 2] & 1)) {
      g_pe[0x0A / 2] |= 4;
      raise(kInterruptPeToken);
    }
  }
}

void cpWrite(uint8_t reg, uint32_t v) {
  if (reg == 0x50) g_vcdLo = v;
  else if (reg == 0x60) g_vcdHi = v;
  else if (reg >= 0x70 && reg < 0xA0 && (reg & 0xF) < 8) g_vat[(reg >> 4) - 7][reg & 7] = v;
}

void parseBuffer(const uint8_t* p, uint32_t n, uint32_t* consumed, int depth);

void callDisplayList(uint32_t addr, uint32_t size, int depth) {
  g_stats.displayLists++;
  if (depth > 4) {
    LOG_WARN("GX: nested display list depth exceeded");
    return;
  }
  // The CP fetches display lists in 32-byte units: the low address/size bits are ignored.
  addr &= 0x01FFFFE0;
  size &= ~31u;
  if (addr + size > guest::kMem1Size || size > 0x400000) {
    LOG_ERROR("GX: display list %08X size %X outside MEM1, skipped", addr, size);
    return;
  }
  const uint8_t* p = guest::ptr(0x80000000 | addr);
  uint32_t used = 0;
  parseBuffer(p, size, &used, depth + 1);
}

// Returns the size of the command at p, or 0 if more bytes are needed.
uint32_t commandSize(const uint8_t* p, uint32_t n) {
  uint8_t op = p[0];
  switch (op) {
  case 0x00:
  case 0x44:
  case 0x48: return 1;
  case 0x08: return n >= 6 ? 6 : 0;
  case 0x10: {
    if (n < 5) return 0;
    uint32_t count = field(be32(p + 1), 16, 16) + 1;
    uint32_t sz = 5 + count * 4;
    return n >= sz ? sz : 0;
  }
  case 0x20:
  case 0x28:
  case 0x30:
  case 0x38:
  case 0x61: return n >= 5 ? 5 : 0;
  case 0x40: return n >= 9 ? 9 : 0;
  default:
    if (op >= 0x80 && op < 0xC0) {
      if (n < 3) return 0;
      uint32_t count = (uint32_t)p[1] << 8 | p[2];
      uint32_t sz = 3 + count * vertexSize(op & 7);
      return n >= sz ? sz : 0;
    }
    return 0xFFFFFFFF;  // unknown opcode
  }
}

// Recent commands, dumped when the parser meets an unknown opcode (desync diagnosis).
struct RecentCmd {
  uint8_t bytes[12];
  uint32_t size;
  uint32_t vcdLo, vcdHi, vat[3];
};
RecentCmd g_recent[16];
uint32_t g_recentPos = 0;

void dumpRecent() {
  for (uint32_t k = g_recentPos > 16 ? g_recentPos - 16 : 0; k < g_recentPos; k++) {
    const RecentCmd& r = g_recent[k & 15];
    LOG_ERROR("  cmd size %u: %02X %02X %02X %02X %02X %02X %02X %02X %02X  vcd %08X/%08X vat %08X %08X %08X", r.size,
              r.bytes[0], r.bytes[1], r.bytes[2], r.bytes[3], r.bytes[4], r.bytes[5], r.bytes[6], r.bytes[7],
              r.bytes[8], r.vcdLo, r.vcdHi, r.vat[0], r.vat[1], r.vat[2]);
  }
}

void execute(const uint8_t* p, uint32_t size, int depth) {
  uint8_t op = p[0];
  {
    RecentCmd& r = g_recent[g_recentPos++ & 15];
    memcpy(r.bytes, p, size < 12 ? size : 12);
    r.size = size;
    r.vcdLo = g_vcdLo;
    r.vcdHi = g_vcdHi;
    uint32_t vat = (op >= 0x80) ? (op & 7) : 0;
    r.vat[0] = g_vat[0][vat];
    r.vat[1] = g_vat[1][vat];
    r.vat[2] = g_vat[2][vat];
  }
  g_stats.commands++;
  switch (op) {
  case 0x08:
    g_stats.cpWrites++;
    cpWrite(p[1], be32(p + 2));
    break;
  case 0x10: g_stats.xfWrites++; break;
  case 0x61:
    g_stats.bpWrites++;
    bpWrite(be32(p + 1));
    break;
  case 0x40: callDisplayList(be32(p + 1), be32(p + 5), depth); return;
  default:
    if (op >= 0x80) g_stats.draws++;
    break;
  }
  if (g_sink) g_sink->command(p, size);
}

void parseBuffer(const uint8_t* p, uint32_t n, uint32_t* consumed, int depth) {
  uint32_t off = 0;
  while (off < n) {
    uint32_t sz = commandSize(p + off, n - off);
    if (sz == 0) break;
    if (sz == 0xFFFFFFFF) {
      static int reported = 0;
      if (reported++ < 3) {
        LOG_ERROR("GX: unknown FIFO opcode %02X at offset %u of %u (depth %d); recent commands:", p[off], off, n, depth);
        dumpRecent();
        char hex[64 * 3 + 1];
        uint32_t from = off >= 40 ? off - 40 : 0;
        int h = 0;
        for (uint32_t k = from; k < n && k < from + 64; k++) h += snprintf(hex + h, sizeof(hex) - h, "%02X ", p[k]);
        LOG_ERROR("  bytes from offset %u: %s", from, hex);
      }
      off++;
      continue;
    }
    execute(p + off, sz, depth);
    off += sz;
  }
  *consumed = off;
}

void gpReceive(const uint8_t* bytes, uint32_t n) {
  std::lock_guard lk(g_parseMutex);
  g_stats.bytes += n;
  g_stream.insert(g_stream.end(), bytes, bytes + n);
  uint32_t used = 0;
  parseBuffer(g_stream.data(), (uint32_t)g_stream.size(), &used, 0);
  g_stream.erase(g_stream.begin(), g_stream.begin() + used);
  if (g_stream.size() > 0x01000000) {
    LOG_ERROR("GX: %zu bytes waiting for a complete command (opcode %02X); dropping them", g_stream.size(), g_stream[0]);
    g_stream.clear();
  }
}

// --- write-gather pipe ------------------------------------------------------------
void wgpWrite(uint32_t v, int size) {
  uint8_t bytes[4];
  for (int i = 0; i < size; i++) bytes[i] = (uint8_t)(v >> (8 * (size - 1 - i)));
  // Into guest RAM at the CPU FIFO write pointer (display lists are built this way).
  if (g_piFifoEnd > g_piFifoBase) {
    for (int i = 0; i < size; i++) {
      guest::g_mem[g_piFifoWptr & guest::kMemMask] = bytes[i];
      g_piFifoWptr++;
      if ((g_piFifoWptr & 0x03FFFFFF) >= (g_piFifoEnd & 0x03FFFFFF))
        g_piFifoWptr = (g_piFifoBase & 0x03FFFFFF) | 0x04000000;  // wrapped
    }
  }
  if (gpReading()) gpReceive(bytes, (uint32_t)size);
}

// --- MMIO ---------------------------------------------------------------------------
uint32_t reg16Read(const uint16_t* regs, uint32_t off, int size) {
  off &= 0x7F;
  if (size == 4) return (uint32_t)regs[off / 2] << 16 | regs[off / 2 + 1];
  return regs[off / 2];
}

void reg16Write(uint16_t* regs, uint32_t off, uint32_t v, int size) {
  off &= 0x7F;
  if (size == 4) {
    regs[off / 2] = (uint16_t)(v >> 16);
    regs[off / 2 + 1] = (uint16_t)v;
  } else {
    regs[off / 2] = (uint16_t)v;
  }
}

uint32_t cpRead(uint32_t off, int size) {
  off &= 0x7F;
  // The GP is always caught up: idle, read pointer == write pointer, distance 0.
  uint32_t wptr = g_piFifoWptr & 0x03FFFFFF;
  switch (off) {
  case 0x00: return 0x000C | (g_cpBreakpointHit ? 0x10 : 0);  // SR: read idle, command idle, breakpoint
  case 0x30: case 0x32: return 0;
  case 0x34: case 0x38: return size == 4 ? wptr : (wptr & 0xFFFF);
  case 0x36: case 0x3A: return wptr >> 16;
  }
  return reg16Read(g_cp, off, size);
}

void cpMmioWrite(uint32_t off, uint32_t v, int size) {
  reg16Write(g_cp, off, v, size);
  if ((off & 0x7F) != 0x02) return;
  // CR: the GP has always consumed everything written so far, so an enabled breakpoint
  // (bit 5) at or behind the write pointer is reached immediately; raise the CP
  // interrupt (bit 1 enables it). Disabling the breakpoint clears the status.
  uint16_t cr = g_cp[0x02 / 2];
  if (!(cr & 0x20)) {
    g_cpBreakpointHit = false;
  } else if (!g_cpBreakpointHit) {
    g_cpBreakpointHit = true;
    if (cr & 0x02) raise(kInterruptCp);
  }
}

uint32_t peRead(uint32_t off, int size) {
  off &= 0x7F;
  if (off == 0x0E) return g_peToken;
  return reg16Read(g_pe, off, size);
}

void peWrite(uint32_t off, uint32_t v, int size) {
  off &= 0x7F;
  if (off == 0x0A && size == 2) {
    // INTSR: enables in bits 0-1; status bits 2-3 are write-1-to-clear.
    uint16_t status = g_pe[0x0A / 2] & 0xC & ~(uint16_t)(v & 0xC);
    g_pe[0x0A / 2] = (uint16_t)((v & 3) | status);
    return;
  }
  reg16Write(g_pe, off, v, size);
}

uint32_t g_piRegs[0x40];

uint32_t piRead(uint32_t off, int size) {
  (void)size;
  off &= 0xFF;
  switch (off) {
  case 0x0C: return g_piFifoBase;
  case 0x10: return g_piFifoEnd;
  case 0x14: return g_piFifoWptr;
  case 0x2C: return 0x20000000;  // console revision (HW2)
  }
  return g_piRegs[off / 4];
}

void piWrite(uint32_t off, uint32_t v, int size) {
  (void)size;
  off &= 0xFF;
  switch (off) {
  case 0x0C: g_piFifoBase = v & 0x03FFFFE0; return;
  case 0x10: g_piFifoEnd = v & 0x03FFFFE0; return;
  case 0x14: g_piFifoWptr = v & 0x07FFFFE0; return;
  }
  g_piRegs[off / 4] = v;
}

const guest::MmioDevice kCp = {cpRead, cpMmioWrite};
const guest::MmioDevice kPe = {peRead, peWrite};
const guest::MmioDevice kPi = {piRead, piWrite};

}  // namespace

void vertexLayout(uint32_t vat, Attr out[kNumAttrs]) {
  const uint32_t a = g_vat[0][vat], b = g_vat[1][vat], c = g_vat[2][vat];
  auto set = [&](int i, uint32_t type, uint32_t directSize, uint32_t indices) {
    out[i].type = type;
    out[i].indices = type >= 2 ? indices : 0;
    out[i].size = type == 1 ? directSize : type == 2 ? indices : type == 3 ? 2 * indices : 0;
  };
  // Matrix indices: one byte each when present (always direct).
  for (int i = 0; i < 9; i++) set(i, field(g_vcdLo, 1, i), 1, 0);
  set(kAttrPos, field(g_vcdLo, 2, 9), (field(a, 1, 0) ? 3 : 2) * compSize(field(a, 3, 1)), 1);
  {
    bool nbt = field(a, 1, 9);
    uint32_t idx = (nbt && field(a, 1, 31)) ? 3 : 1;
    set(kAttrNrm, field(g_vcdLo, 2, 11), (nbt ? 9 : 3) * compSize(field(a, 3, 10)), idx);
  }
  set(kAttrClr0, field(g_vcdLo, 2, 13), colorSize(field(a, 3, 14)), 1);
  set(kAttrClr0 + 1, field(g_vcdLo, 2, 15), colorSize(field(a, 3, 18)), 1);
  // Texture coordinates 0-7: (count bit, format) per VAT layout.
  const uint32_t texCnt[8] = {field(a, 1, 21), field(b, 1, 0), field(b, 1, 9), field(b, 1, 18),
                              field(b, 1, 27), field(c, 1, 5), field(c, 1, 14), field(c, 1, 23)};
  const uint32_t texFmt[8] = {field(a, 3, 22), field(b, 3, 1), field(b, 3, 10), field(b, 3, 19),
                              field(b, 3, 28), field(c, 3, 6), field(c, 3, 15), field(c, 3, 24)};
  for (int t = 0; t < 8; t++)
    set(kAttrTex0 + t, field(g_vcdHi, 2, 2 * t), (texCnt[t] ? 2 : 1) * compSize(texFmt[t]), 1);
}

uint32_t vatReg(int group, uint32_t vat) { return g_vat[group][vat & 7]; }

uint32_t vertexSize(uint32_t vat) {
  Attr attrs[kNumAttrs];
  vertexLayout(vat, attrs);
  uint32_t size = 0;
  for (const Attr& at : attrs) size += at.size;
  return size;
}

void init() {
  guest::registerMmio(0xCC000000, 0x400, &kCp);
  guest::registerMmio(0xCC001000, 0x400, &kPe);
  guest::registerMmio(0xCC003000, 0x400, &kPi);
  guest::setFifoSink(wgpWrite);
}

void setSink(Sink* sink) { g_sink = sink; }

Stats stats() { return g_stats; }

}  // namespace gxfifo
