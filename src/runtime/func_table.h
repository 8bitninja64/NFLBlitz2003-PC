// Address -> recompiled function table (provided by the game module).
#pragma once

#include "ctx.h"

namespace guest {

struct FuncEntry {
  uint32_t addr;
  Fn fn;
};

extern const FuncEntry* g_funcTable;  // sorted by addr
extern uint32_t g_funcTableSize;
extern uint32_t g_entryPoint;

Fn lookupFunc(uint32_t addr);

}  // namespace guest
