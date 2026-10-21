// CPU-level runtime support for recompiled code: indirect calls, SPRs,
// paired-single quantization, traps.
#include "ctx.h"
#include "func_table.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace guest {

Fn lookupFunc(uint32_t addr) {
  uint32_t lo = 0, hi = g_funcTableSize;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t a = g_funcTable[mid].addr;
    if (a == addr) return g_funcTable[mid].fn;
    if (a < addr) lo = mid + 1;
    else hi = mid;
  }
  return nullptr;
}

void callIndirect(Ctx& c, uint32_t target) {
  Fn fn = lookupFunc(target);
  if (!fn) {
    logf(LogLevel::Fatal, "indirect call to unknown address %08X (lr=%08X)", target, c.lr);
    trap(c, target, "bad indirect call");
    return;
  }
  fn(c);
}

[[noreturn]] void fatalTrap(Ctx& c, uint32_t pc, const char* what) {
  logf(LogLevel::Fatal, "guest trap '%s' at %08X lr=%08X r1=%08X r3=%08X", what, pc, c.lr, c.r[1], c.r[3]);
  dumpCallTrace();
  std::abort();
}

void trap(Ctx& c, uint32_t pc, const char* what) { fatalTrap(c, pc, what); }

// Timer runs at bus clock / 4 = 40.5 MHz. It can run faster than real time (fast boot): from
// (g_warpNs, g_warpTb) on it advances g_warp ticks per real tick.
namespace {
std::atomic<int64_t> g_warpNs{0};
std::atomic<uint64_t> g_warpTb{0};
std::atomic<uint32_t> g_warp{1};
std::atomic<uint64_t> g_lastTb{0};
std::mutex g_warpMutex;

int64_t hostNs() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return duration_cast<nanoseconds>(steady_clock::now() - start).count();
}
}  // namespace

uint64_t timeBase() {
  int64_t ns = hostNs();
  uint64_t tb = g_warpTb.load() + (uint64_t)(ns - g_warpNs.load()) * 81ull / 2000ull * g_warp.load();
  // Monotonic across warp changes.
  uint64_t last = g_lastTb.load();
  while (tb > last && !g_lastTb.compare_exchange_weak(last, tb)) {
  }
  return tb > last ? tb : last;
}

void setTimeWarp(uint32_t factor) {
  std::lock_guard lk(g_warpMutex);
  uint64_t tb = timeBase();
  g_warp = 1;
  g_warpNs = hostNs();
  g_warpTb = tb;
  g_warp = factor ? factor : 1;
}

uint32_t timeWarp() { return g_warp.load(); }

uint32_t readSpr(Ctx& c, uint32_t spr, uint32_t pc) {
  switch (spr) {
  case 1: return (c.so << 31) | (c.ov << 30) | (c.ca << 29);
  case 268: return (uint32_t)timeBase();
  case 269: return (uint32_t)(timeBase() >> 32);
  case 920: return c.hid2;
  case 26: return c.srr0;
  case 27: return c.srr1;
  case 287: return 0x00083214;  // PVR: Gekko
  default:
    logf(LogLevel::Debug, "mfspr %u at %08X", spr, pc);
    return 0;
  }
}

void writeSpr(Ctx& c, uint32_t spr, uint32_t v, uint32_t pc) {
  switch (spr) {
  case 1:
    c.so = (v >> 31) & 1;
    c.ov = (v >> 30) & 1;
    c.ca = (v >> 29) & 1;
    break;
  case 920: c.hid2 = v; break;
  case 26: c.srr0 = v; break;
  case 27: c.srr1 = v; break;
  default: logf(LogLevel::Debug, "mtspr %u = %08X at %08X", spr, v, pc); break;
  }
}

static const float kDequant[64] = {
    1.0f / (1ull << 0),  1.0f / (1ull << 1),  1.0f / (1ull << 2),  1.0f / (1ull << 3),  1.0f / (1ull << 4),
    1.0f / (1ull << 5),  1.0f / (1ull << 6),  1.0f / (1ull << 7),  1.0f / (1ull << 8),  1.0f / (1ull << 9),
    1.0f / (1ull << 10), 1.0f / (1ull << 11), 1.0f / (1ull << 12), 1.0f / (1ull << 13), 1.0f / (1ull << 14),
    1.0f / (1ull << 15), 1.0f / (1ull << 16), 1.0f / (1ull << 17), 1.0f / (1ull << 18), 1.0f / (1ull << 19),
    1.0f / (1ull << 20), 1.0f / (1ull << 21), 1.0f / (1ull << 22), 1.0f / (1ull << 23), 1.0f / (1ull << 24),
    1.0f / (1ull << 25), 1.0f / (1ull << 26), 1.0f / (1ull << 27), 1.0f / (1ull << 28), 1.0f / (1ull << 29),
    1.0f / (1ull << 30), 1.0f / (1ull << 31), (float)(1ull << 32), (float)(1ull << 31), (float)(1ull << 30),
    (float)(1ull << 29), (float)(1ull << 28), (float)(1ull << 27), (float)(1ull << 26), (float)(1ull << 25),
    (float)(1ull << 24), (float)(1ull << 23), (float)(1ull << 22), (float)(1ull << 21), (float)(1ull << 20),
    (float)(1ull << 19), (float)(1ull << 18), (float)(1ull << 17), (float)(1ull << 16), (float)(1ull << 15),
    (float)(1ull << 14), (float)(1ull << 13), (float)(1ull << 12), (float)(1ull << 11), (float)(1ull << 10),
    (float)(1ull << 9),  (float)(1ull << 8),  (float)(1ull << 7),  (float)(1ull << 6),  (float)(1ull << 5),
    (float)(1ull << 4),  (float)(1ull << 3),  (float)(1ull << 2),  (float)(1ull << 1),
};

static float loadQuant(uint32_t ea, uint32_t type, uint32_t scale, uint32_t* size) {
  switch (type) {
  case 4: *size = 1; return (float)rd8(ea) * kDequant[scale];
  case 5: *size = 2; return (float)rd16(ea) * kDequant[scale];
  case 6: *size = 1; return (float)(int8_t)rd8(ea) * kDequant[scale];
  case 7: *size = 2; return (float)(int16_t)rd16(ea) * kDequant[scale];
  default: *size = 4; return asF32(rd32(ea));
  }
}

void psqLoad(Ctx& c, int fd, uint32_t ea, int w, int i) {
  uint32_t gqr = c.gqr[i];
  uint32_t type = (gqr >> 16) & 7;
  uint32_t scale = (gqr >> 24) & 63;
  uint32_t size;
  float a = loadQuant(ea, type, scale, &size);
  float b = w ? 1.0f : loadQuant(ea + size, type, scale, &size);
  c.f[fd].d = a;
  c.ps1[fd] = b;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void psqStore(Ctx& c, int fs, uint32_t ea, int w, int i) {
  uint32_t gqr = c.gqr[i];
  uint32_t type = gqr & 7;
  uint32_t scale = (gqr >> 8) & 63;
  // Quantize scale is the reciprocal of the dequantize table entry.
  float q = 1.0f / kDequant[scale];
  float v[2] = {(float)c.f[fs].d, (float)c.ps1[fs]};
  int n = w ? 1 : 2;
  for (int k = 0; k < n; k++) {
    switch (type) {
    case 4: wr8(ea + k, (uint8_t)clampf(v[k] * q, 0.f, 255.f)); break;
    case 5: wr16(ea + 2 * k, (uint16_t)clampf(v[k] * q, 0.f, 65535.f)); break;
    case 6: wr8(ea + k, (uint8_t)(int8_t)clampf(v[k] * q, -128.f, 127.f)); break;
    case 7: wr16(ea + 2 * k, (uint16_t)(int16_t)clampf(v[k] * q, -32768.f, 32767.f)); break;
    default: wr32(ea + 4 * k, f32Bits(v[k])); break;
    }
  }
}

// Return from interrupt: continue at SRR0 with MSR = SRR1. The SDK uses it to run a
// function in real mode and to come back from it.
void rfi(Ctx& c) {
  writeMsr(c, c.srr1);
  uint32_t target = c.srr0 | 0x80000000u;
  if (target == (c.lr | 0x80000000u)) return;
  callIndirect(c, target);
}

void dcbzL(Ctx& c, uint32_t ea) {
  (void)c;
  memset(hostp(ea & ~31u), 0, 32);
}

}  // namespace guest

void guest_unresolved(guest::Ctx& c) { guest::trap(c, 0, "branch to unresolved address"); }
