// Loads the DOL from the user's disc into guest memory and fills in the low-memory
// boot block that the IPL/apploader would normally set up.
#include "boot.h"

#include "disc.h"
#include "log.h"
#include "memory.h"

#include <vector>

namespace guest {

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

BootInfo g_boot;

bool loadDol() {
  uint8_t hdr[0x100];
  if (!g_disc.read(g_disc.dolOffset(), hdr, sizeof(hdr))) return false;
  for (int i = 0; i < 18; i++) {
    uint32_t off = be32(hdr + i * 4), addr = be32(hdr + 0x48 + i * 4), size = be32(hdr + 0x90 + i * 4);
    if (!size) continue;
    if (!g_disc.read(g_disc.dolOffset() + off, hostp(addr), size)) return false;
  }
  // .bss is cleared by the game's own __init_data from _bss_init_info.
  g_boot.entry = be32(hdr + 0xE0);
  return true;
}

bool setupLowMemory() {
  uint8_t* m = hostp(0x80000000);
  if (!g_disc.read(0, m, 0x20)) return false;  // disc ID block
  wr32(0x80000020, 0x0D15EA5E);                 // boot magic
  wr32(0x80000024, 1);
  wr32(0x80000028, kMem1Size);
  wr32(0x8000002C, 0x00000003);  // console type: retail HW2
  // FST at the top of MEM1, as the apploader leaves it; reserve a host-owned region
  // under it for runtime stacks.
  uint32_t fstSize = (g_disc.fstSize() + 31) & ~31u;
  uint32_t fstAddr = (0x80000000 + kMem1Size - fstSize) & ~31u;
  if (!g_disc.read(g_disc.fstOffset(), hostp(fstAddr), g_disc.fstSize())) return false;
  g_boot.reservedTop = fstAddr;
  g_boot.reservedBase = fstAddr - kReservedSize;
  wr32(0x80000030, 0);                    // ArenaLo: let OSInit use __ArenaLo
  wr32(0x80000034, g_boot.reservedBase);  // ArenaHi
  wr32(0x80000038, fstAddr);
  wr32(0x8000003C, fstSize);
  wr32(0x800000CC, 0);  // VI TV mode: NTSC
  wr32(0x800000F0, kMem1Size);
  wr32(0x800000F8, 162000000);  // bus clock
  wr32(0x800000FC, 486000000);  // CPU clock
  wr16(0x800030E6, 0);
  return true;
}

uint32_t allocReserved(uint32_t size) {
  static uint32_t next = 0;
  if (!next) next = g_boot.reservedBase;
  uint32_t a = next;
  next += (size + 31) & ~31u;
  if (next > g_boot.reservedTop) {
    logf(LogLevel::Fatal, "runtime reserved guest memory exhausted");
    return 0;
  }
  return a;
}

}  // namespace guest
