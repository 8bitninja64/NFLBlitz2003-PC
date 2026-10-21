// Debugging aids for the game's 3D code, enabled by adding pre-hooks to config/hooks.txt:
//   ?<addr> = CallLog          BLITZ_DEBUG_CALLS=1 logs each call (r3-r6 and the guest call stack)
//   ?0x801DDF98 = ModelListDebug   BLITZ_DEBUG_MODELS=<frame>[,...] dumps what the scene renderer draws
#include "hle.h"
#include "hw.h"
#include "log.h"
#include "sched.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// A draw list: {?, count, elements}; element {n, ?, atomic table, entries}; entry {matrix, player group}.
// The atomic drawn for an entry is table[group+0x08] (the group's LOD).
void dumpList(const char* name, uint32_t list) {
  uint32_t count = guest::rd32(list + 4), arr = guest::rd32(list + 8);
  for (uint32_t j = 0; j < count && j < 16; j++) {
    uint32_t e = arr + j * 16;
    uint32_t num = guest::rd32(e), table = guest::rd32(e + 8), entries = guest::rd32(e + 12);
    std::string line;
    for (uint32_t k = 0; k < num && k < 24; k++) {
      uint32_t ent = entries + k * 8, grp = guest::rd32(ent + 4);
      uint32_t lod = guest::rd32(grp + 8);
      char b[96];
      snprintf(b, sizeof(b), " [mtx %08X group %08X flags %X lod %u atomic %08X]", guest::rd32(ent), grp,
               guest::rd32(grp), lod, lod < 16 ? guest::rd32(table + lod * 4) : 0);
      line += b;
    }
    LOG_INFO("  %s[%u] n=%u table %08X:%s", name, j, num, table, line.c_str());
  }
}

}  // namespace

HLE(CallLog) {
  static const bool on = getenv("BLITZ_DEBUG_CALLS") != nullptr;
  static int logged = 0;
  if (!on || logged >= 400) return;
  logged++;
  LOG_INFO("call %08X frame %u: r3 %08X r4 %08X r5 %08X r6 %08X; %s", currentGuestFunction(), hw::g_framesBuilt.load(),
           c.r[3], c.r[4], c.r[5], c.r[6], sched::currentBacktrace().c_str());
}

// 801DDF98(scene): +0x10 player groups at +0x70 (0xFC bytes: +0 flags, +8 LOD, +0x1C element refs),
// +0x68/+0x6C model elements (40 bytes: LOD atomics, +0x1C kind (1 bone-attached, 2 skinned), +0x24 users),
// draw lists A (+0x28, skinned) and B (+0x1C, bone-attached).
HLE(ModelListDebug) {
  static const char* frames = getenv("BLITZ_DEBUG_MODELS");
  if (!frames) return;
  uint32_t frame = hw::g_framesBuilt.load();
  char key[16];
  snprintf(key, sizeof(key), ",%u,", frame);
  if ((std::string(",") + frames + ",").find(key) == std::string::npos) return;
  uint32_t s = c.r[3];
  uint32_t n = guest::rd32(s + 0x10), groups = guest::rd32(s + 0x70);
  LOG_INFO("models: frame %u scene %08X, %u groups", frame, s, n);
  for (uint32_t i = 0; i < n && i < 32; i++) {
    uint32_t g = groups + i * 0xFC;
    if (guest::rd32(g) & 1)
      LOG_INFO("  group %u %08X: flags %X lod %u", i, g, guest::rd32(g), guest::rd32(g + 8));
  }
  dumpList("listA", s + 0x28);
  dumpList("listB", s + 0x1C);
  uint32_t nElems = guest::rd32(s + 0x68), elems = guest::rd32(s + 0x6C), perGroup = guest::rd32(s + 0x0C);
  std::string line;
  for (uint32_t p = 0; p < nElems && p < 400; p++) {
    uint32_t e = elems + p * 40;
    uint32_t kind = guest::rd32(e + 0x1C), users = guest::rd32(e + 0x24);
    if (!users) continue;
    char b[64];
    snprintf(b, sizeof(b), " e%u[%08X kind %u users %u]", p, guest::rd32(e), kind, users);
    line += b;
  }
  LOG_INFO("  %u elements, %u per group, in use:%s", nElems, perGroup, line.c_str());
  for (uint32_t i = 0; i < n && i < 32; i++) {
    uint32_t g = groups + i * 0xFC;
    if (!(guest::rd32(g) & 1)) continue;
    std::string refs;
    uint32_t list = guest::rd32(g + 0x1C);
    for (uint32_t k = 0; k < perGroup && k < 64; k++) {
      char b[16];
      snprintf(b, sizeof(b), " %d", (int32_t)guest::rd32(list + 4 * k));
      refs += b;
    }
    LOG_INFO("  group %u elements:%s", i, refs.c_str());
  }
}
