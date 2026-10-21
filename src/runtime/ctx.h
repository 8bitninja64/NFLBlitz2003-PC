// Guest CPU context and memory access for recompiled GameCube code.
// Included by every generated file: keep it small and header-only.
#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define BL_LIKELY(x) (x)
#define BL_UNLIKELY(x) (x)
#define BL_INLINE __forceinline
static inline uint32_t bl_bswap32(uint32_t v) { return _byteswap_ulong(v); }
static inline uint16_t bl_bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint64_t bl_bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
#define BL_LIKELY(x) __builtin_expect(!!(x), 1)
#define BL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BL_INLINE inline __attribute__((always_inline))
static inline uint32_t bl_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bl_bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint64_t bl_bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

namespace guest {

constexpr uint32_t kMem1Size = 0x01800000;   // 24 MB
constexpr uint32_t kMemMask = 0x01FFFFFF;
constexpr uint32_t kLockedCacheSize = 0x4000;

extern uint8_t* g_mem;          // 32 MB host allocation backing MEM1
extern uint8_t* g_lockedCache;  // 16 KB L2 locked cache (0xE0000000)
extern volatile uint32_t g_irqPending;     // runtime storage
extern volatile uint32_t* g_irqPendingPtr;  // what generated code polls (a game module gets the runtime's)

union FPR {
  double d;
  uint64_t u;
};

struct Ctx {
  uint32_t r[32];
  FPR f[32];
  double ps1[32];
  uint8_t cr[8];  // each field: LT=8 GT=4 EQ=2 SO=1
  uint32_t lr, ctr;
  uint32_t ca, so, ov;
  uint32_t fpscr;
  uint32_t gqr[8];
  uint32_t msr;
  uint32_t hid2;
  uint32_t srr0, srr1;
  void* thread;   // owning host thread state
};

using Fn = void (*)(Ctx&);

// Slow paths (MMIO, locked cache), implemented by the runtime.
uint32_t mmioRead(uint32_t ea, int size);
void mmioWrite(uint32_t ea, uint32_t v, int size);
uint8_t* slowPtr(uint32_t ea);
void callIndirect(Ctx& c, uint32_t target);
void checkIrq(Ctx& c);
void trap(Ctx& c, uint32_t pc, const char* what);
uint32_t readSpr(Ctx& c, uint32_t spr, uint32_t pc);
void writeSpr(Ctx& c, uint32_t spr, uint32_t v, uint32_t pc);
void writeMsr(Ctx& c, uint32_t v);
uint64_t timeBase();
void setTimeWarp(uint32_t factor);  // runtime only: guest time runs factor x real time
uint32_t timeWarp();
void psqLoad(Ctx& c, int fd, uint32_t ea, int w, int i);
void psqStore(Ctx& c, int fs, uint32_t ea, int w, int i);
void dcbzL(Ctx& c, uint32_t ea);
void rfi(Ctx& c);

BL_INLINE bool isSlow(uint32_t ea) { return ea >= 0xCC000000u; }
BL_INLINE uint8_t* hostp(uint32_t ea) { return isSlow(ea) ? slowPtr(ea) : g_mem + (ea & kMemMask); }

BL_INLINE uint8_t rd8(uint32_t ea) {
  if (BL_UNLIKELY(isSlow(ea))) return (uint8_t)mmioRead(ea, 1);
  return g_mem[ea & kMemMask];
}
BL_INLINE uint16_t rd16(uint32_t ea) {
  if (BL_UNLIKELY(isSlow(ea))) return (uint16_t)mmioRead(ea, 2);
  uint16_t v;
  memcpy(&v, g_mem + (ea & kMemMask), 2);
  return bl_bswap16(v);
}
BL_INLINE uint32_t rd32(uint32_t ea) {
  if (BL_UNLIKELY(isSlow(ea))) return mmioRead(ea, 4);
  uint32_t v;
  memcpy(&v, g_mem + (ea & kMemMask), 4);
  return bl_bswap32(v);
}
BL_INLINE uint64_t rd64(uint32_t ea) {
  if (BL_UNLIKELY(isSlow(ea))) return ((uint64_t)mmioRead(ea, 4) << 32) | mmioRead(ea + 4, 4);
  uint64_t v;
  memcpy(&v, g_mem + (ea & kMemMask), 8);
  return bl_bswap64(v);
}
BL_INLINE void wr8(uint32_t ea, uint8_t v) {
  if (BL_UNLIKELY(isSlow(ea))) return mmioWrite(ea, v, 1);
  g_mem[ea & kMemMask] = v;
}
BL_INLINE void wr16(uint32_t ea, uint16_t v) {
  if (BL_UNLIKELY(isSlow(ea))) return mmioWrite(ea, v, 2);
  v = bl_bswap16(v);
  memcpy(g_mem + (ea & kMemMask), &v, 2);
}
BL_INLINE void wr32(uint32_t ea, uint32_t v) {
  if (BL_UNLIKELY(isSlow(ea))) return mmioWrite(ea, v, 4);
  v = bl_bswap32(v);
  memcpy(g_mem + (ea & kMemMask), &v, 4);
}
BL_INLINE void wr64(uint32_t ea, uint64_t v) {
  if (BL_UNLIKELY(isSlow(ea))) {
    mmioWrite(ea, (uint32_t)(v >> 32), 4);
    mmioWrite(ea + 4, (uint32_t)v, 4);
    return;
  }
  v = bl_bswap64(v);
  memcpy(g_mem + (ea & kMemMask), &v, 8);
}

BL_INLINE float asF32(uint32_t v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}
BL_INLINE uint32_t f32Bits(float f) {
  uint32_t v;
  memcpy(&v, &f, 4);
  return v;
}
BL_INLINE double asF64(uint64_t v) {
  double d;
  memcpy(&d, &v, 8);
  return d;
}

BL_INLINE uint32_t rotl(uint32_t v, uint32_t n) {
  n &= 31;
  return n ? (v << n) | (v >> (32 - n)) : v;
}

BL_INLINE uint8_t cmpS(Ctx& c, int32_t a, int32_t b) {
  return (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (c.so & 1));
}
BL_INLINE uint8_t cmpU(Ctx& c, uint32_t a, uint32_t b) {
  return (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | (c.so & 1));
}
BL_INLINE void setCr0(Ctx& c, uint32_t v) { c.cr[0] = cmpS(c, (int32_t)v, 0); }
BL_INLINE uint8_t cmpF(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return 1;
  return a < b ? 8 : a > b ? 4 : 2;
}

BL_INLINE uint32_t cntlz(uint32_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long i;
  return _BitScanReverse(&i, v) ? 31 - i : 32;
#else
  return v ? __builtin_clz(v) : 32;
#endif
}

BL_INLINE uint32_t divwS(int32_t a, int32_t b) {
  if (b == 0) return a < 0 ? 0xFFFFFFFFu : 0;
  if (a == INT32_MIN && b == -1) return 0xFFFFFFFFu;
  return (uint32_t)(a / b);
}
BL_INLINE uint32_t divwU(uint32_t a, uint32_t b) { return b ? a / b : 0; }

BL_INLINE uint32_t sraw(Ctx& c, uint32_t s, uint32_t n) {
  n &= 63;
  int32_t v = (int32_t)s;
  if (n >= 32) {
    c.ca = v < 0;
    return v < 0 ? 0xFFFFFFFFu : 0;
  }
  c.ca = (v < 0) && n && (s & ((1u << n) - 1));
  return (uint32_t)(v >> n);
}

// Results of FP arithmetic. FPSCR[NI] (non-IEEE mode; OSInit always sets it with
// PPCSetFpNonIEEEMode) makes the FPU return a denormalized result as a zero of the same sign.
// Done here rather than with the host's MXCSR flush-to-zero, which would also change host code on
// guest threads and the exact conversions of lfs/stfs/psq_l/psq_st.
constexpr uint32_t kFpscrNI = 0x4;  // FPSCR bit 29
BL_INLINE double roundDouble(Ctx& c, double v) {
  uint64_t u;
  memcpy(&u, &v, 8);
  if (c.fpscr & kFpscrNI) u &= (u & 0x7FF0000000000000ull) ? ~0ull : 0x8000000000000000ull;
  return asF64(u);
}
BL_INLINE double roundSingle(Ctx& c, double v) {
  uint32_t b = f32Bits((float)v);
  if (c.fpscr & kFpscrNI) b &= (b & 0x7F800000u) ? ~0u : 0x80000000u;
  return asF32(b);
}

BL_INLINE uint64_t fctiw(Ctx& c, double v, bool truncate) {
  int32_t r;
  if (std::isnan(v)) r = INT32_MIN;
  else if (v >= 2147483648.0) r = INT32_MAX;
  else if (v < -2147483648.0) r = INT32_MIN;
  else r = (int32_t)(truncate ? std::trunc(v) : std::nearbyint(v));
  (void)c;
  return 0xFFF8000000000000ull | (uint32_t)r;
}

BL_INLINE double fsel(double a, double t, double f) { return a >= 0.0 ? t : f; }
BL_INLINE double fres(Ctx& c, double v) { return roundSingle(c, 1.0 / v); }
BL_INLINE double frsqrte(double v) { return 1.0 / std::sqrt(v); }

}  // namespace guest

// Call trace ring (last functions entered on this host thread), for hang/crash reports.
namespace guest {
struct TraceRing {
  uint32_t loop;  // last loop back-edge taken
  uint32_t pos;
  uint32_t addr[64];
};
extern thread_local TraceRing t_trace;
}  // namespace guest
#ifndef BLITZ_NO_TRACE
#define GUEST_ENTER(a) (::guest::t_trace.addr[::guest::t_trace.pos++ & 63] = (a))
#else
#define GUEST_ENTER(a) ((void)0)
#endif

#ifndef BLITZ_NO_TRACE
#define TRACE_LOOP(pc) (::guest::t_trace.loop = (pc))
#else
#define TRACE_LOOP(pc) ((void)0)
#endif

#define CHECK_IRQ(c, pc) \
  do { \
    TRACE_LOOP(pc); \
    if (BL_UNLIKELY(*::guest::g_irqPendingPtr)) ::guest::checkIrq(c); \
  } while (0)
