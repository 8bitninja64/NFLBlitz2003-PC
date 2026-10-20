// Address -> recompiled function table (generated recomp_table.cpp).
#pragma once

#include "ctx.h"

namespace guest {

struct FuncEntry {
  uint32_t addr;
  Fn fn;
};

extern const FuncEntry g_funcTable[];  // sorted by addr
extern const uint32_t g_funcTableSize;
extern const uint32_t g_entryPoint;

Fn lookupFunc(uint32_t addr);

}  // namespace guest
