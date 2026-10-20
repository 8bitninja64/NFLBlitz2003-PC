// Helpers for host implementations (HLE) of guest functions.
#pragma once

#include "ctx.h"
#include "memory.h"

namespace hle {

// Host implementations register themselves by name; a game module asks for them by name.
void registerFunction(const char* name, guest::Fn fn);
guest::Fn find(const char* name);

struct Registrar {
  Registrar(const char* name, guest::Fn fn) { registerFunction(name, fn); }
};

// Integer argument n (0-based): r3..r10, then the caller's parameter area.
inline uint32_t arg(guest::Ctx& c, int n) { return n < 8 ? c.r[3 + n] : guest::rd32(c.r[1] + 8 + 4 * (n - 8)); }
inline double farg(guest::Ctx& c, int n) { return c.f[1 + n].d; }
inline void ret(guest::Ctx& c, uint32_t v) { c.r[3] = v; }

}  // namespace hle

// Defines the host replacement for a hooked guest function (see config/hooks.txt).
#define HLE(name) \
  void HLE_##name(guest::Ctx& c); \
  static ::hle::Registrar hleRegistrar_##name(#name, HLE_##name); \
  void HLE_##name(guest::Ctx& c)
