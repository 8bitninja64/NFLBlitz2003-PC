// Boundary between the runtime (the shipped executable) and the game module (the DLL
// recompiled from the user's own disc). Bump kModuleAbiVersion whenever Ctx, RuntimeApi
// or GameModule change.
#pragma once

#include "ctx.h"
#include "func_table.h"

namespace guest {

constexpr uint32_t kModuleAbiVersion = 1;

// Runtime services the module's code calls (see ctx.h for their meaning).
struct RuntimeApi {
  uint32_t abiVersion;
  uint8_t* mem;
  uint8_t* lockedCache;
  volatile uint32_t* irqPending;
  uint32_t (*mmioRead)(uint32_t ea, int size);
  void (*mmioWrite)(uint32_t ea, uint32_t v, int size);
  uint8_t* (*slowPtr)(uint32_t ea);
  void (*callIndirect)(Ctx& c, uint32_t target);
  void (*checkIrq)(Ctx& c);
  void (*trap)(Ctx& c, uint32_t pc, const char* what);
  uint32_t (*readSpr)(Ctx& c, uint32_t spr, uint32_t pc);
  void (*writeSpr)(Ctx& c, uint32_t spr, uint32_t v, uint32_t pc);
  void (*writeMsr)(Ctx& c, uint32_t v);
  uint64_t (*timeBase)();
  void (*psqLoad)(Ctx& c, int fd, uint32_t ea, int w, int i);
  void (*psqStore)(Ctx& c, int fs, uint32_t ea, int w, int i);
  void (*dcbzL)(Ctx& c, uint32_t ea);
  void (*rfi)(Ctx& c);
  const Fn* hle;  // resolved for GameModule::hleNames, same order
};

// What a module provides.
struct GameModule {
  uint32_t abiVersion;
  const FuncEntry* table;  // sorted by address
  uint32_t tableSize;
  uint32_t entryPoint;
  const char* const* hleNames;  // host implementations the module calls
  uint32_t hleCount;
  TraceRing* (*traceRing)();  // the calling thread's trace ring inside the module
  const char* dolHash;        // identifies the executable the module was built from
};

// Exported by the module DLL. Returns nullptr if the ABI version does not match.
using ModuleInitFn = const GameModule* (*)(const RuntimeApi* api);
constexpr const char* kModuleInitSymbol = "blitz_module_init";

// Implemented by the generated recomp_table.cpp (both builds).
const GameModule* builtinModule();

}  // namespace guest
