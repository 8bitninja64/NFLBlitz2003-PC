// Helpers for host implementations (HLE) of guest functions.
#pragma once

#include "ctx.h"
#include "memory.h"
#include "recomp_funcs.h"

// Defines the host replacement for a hooked guest function (see config/hooks.txt).
#define HLE(name) void HLE_##name(guest::Ctx& c)

namespace hle {

// Integer argument n (0-based): r3..r10, then the caller's parameter area.
inline uint32_t arg(guest::Ctx& c, int n) { return n < 8 ? c.r[3 + n] : guest::rd32(c.r[1] + 8 + 4 * (n - 8)); }
inline double farg(guest::Ctx& c, int n) { return c.f[1 + n].d; }
inline void ret(guest::Ctx& c, uint32_t v) { c.r[3] = v; }

}  // namespace hle
