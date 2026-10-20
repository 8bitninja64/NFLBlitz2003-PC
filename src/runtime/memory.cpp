// Guest memory: MEM1, locked cache, and the memory-mapped hardware registers.
#include "memory.h"

#include "log.h"

#include <cstdlib>
#include <mutex>

namespace guest {

uint8_t* g_mem = nullptr;
uint8_t* g_lockedCache = nullptr;
volatile uint32_t g_irqPending = 0;
volatile uint32_t* g_irqPendingPtr = &g_irqPending;

static MmioHandler g_handlers[64];  // indexed by (ea >> 10) & 63 within 0xCC00xxxx
static uint32_t g_regShadow[0x10000 / 4];
static FifoSink g_fifoSink = nullptr;

void initMemory() {
  g_mem = static_cast<uint8_t*>(calloc(1, kMemMask + 1));
  g_lockedCache = static_cast<uint8_t*>(calloc(1, kLockedCacheSize));
  if (!g_mem || !g_lockedCache) {
    logf(LogLevel::Fatal, "out of memory allocating guest RAM");
    std::abort();
  }
}

void registerMmio(uint32_t base, uint32_t size, MmioHandler handler) {
  for (uint32_t a = base; a < base + size; a += 0x400) g_handlers[((a - 0xCC000000) >> 10) & 63] = handler;
}

void setFifoSink(FifoSink sink) { g_fifoSink = sink; }

uint8_t* slowPtr(uint32_t ea) {
  if ((ea >> 28) == 0xE) return g_lockedCache + (ea & (kLockedCacheSize - 1));
  static uint8_t dummy[64];
  logf(LogLevel::Warn, "host pointer requested for MMIO address %08X", ea);
  return dummy;
}

static uint32_t lockedRead(uint32_t ea, int size) {
  uint8_t* p = g_lockedCache + (ea & (kLockedCacheSize - 1));
  uint32_t v = 0;
  for (int i = 0; i < size; i++) v = (v << 8) | p[i];
  return v;
}

static void lockedWrite(uint32_t ea, uint32_t v, int size) {
  uint8_t* p = g_lockedCache + (ea & (kLockedCacheSize - 1));
  for (int i = size - 1; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v;
}

uint32_t mmioRead(uint32_t ea, int size) {
  if ((ea >> 28) == 0xE) return lockedRead(ea, size);
  if ((ea & 0xFFFF0000u) != 0xCC000000u) {
    logf(LogLevel::Error, "read%d from unmapped address %08X", size * 8, ea);
    return 0;
  }
  MmioHandler h = g_handlers[((ea - 0xCC000000) >> 10) & 63];
  uint32_t off = ea & 0xFFFF;
  if (h && h->read) return h->read(off, size);
  LOG_DEBUG("unhandled MMIO read%d %08X", size * 8, ea);
  uint32_t w = g_regShadow[off >> 2];
  if (size == 4) return w;
  if (size == 2) return (w >> ((2 - (off & 2)) * 8)) & 0xFFFF;
  return (w >> ((3 - (off & 3)) * 8)) & 0xFF;
}

void mmioWrite(uint32_t ea, uint32_t v, int size) {
  if ((ea >> 28) == 0xE) return lockedWrite(ea, v, size);
  if ((ea & 0xFFFF0000u) != 0xCC000000u) {
    logf(LogLevel::Error, "write%d %08X to unmapped address %08X", size * 8, v, ea);
    return;
  }
  uint32_t off = ea & 0xFFFF;
  if (off >= 0x8000) {
    if (g_fifoSink) g_fifoSink(v, size);
    return;
  }
  MmioHandler h = g_handlers[(off >> 10) & 63];
  if (h && h->write) return h->write(off, v, size);
  LOG_DEBUG("unhandled MMIO write%d %08X = %08X", size * 8, ea, v);
  uint32_t& w = g_regShadow[off >> 2];
  if (size == 4) w = v;
  else if (size == 2) {
    int sh = (2 - (off & 2)) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh);
  } else {
    int sh = (3 - (off & 3)) * 8;
    w = (w & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
  }
}

}  // namespace guest
