// Compiled into the game module (DLL) only: the services generated code expects from
// ctx.h, forwarded to the runtime that loaded the module.
#include "module_abi.h"

#ifdef _WIN32
#define BLITZ_EXPORT extern "C" __declspec(dllexport)
#else
#define BLITZ_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace guest {

static const RuntimeApi* g_api = nullptr;

uint8_t* g_mem = nullptr;
uint8_t* g_lockedCache = nullptr;
volatile uint32_t g_irqPending = 0;  // unused in a module; generated code polls the runtime's flag
volatile uint32_t* g_irqPendingPtr = &g_irqPending;
thread_local TraceRing t_trace;
const Fn* g_moduleHle = nullptr;

uint32_t mmioRead(uint32_t ea, int size) { return g_api->mmioRead(ea, size); }
void mmioWrite(uint32_t ea, uint32_t v, int size) { g_api->mmioWrite(ea, v, size); }
uint8_t* slowPtr(uint32_t ea) { return g_api->slowPtr(ea); }
void callIndirect(Ctx& c, uint32_t target) { g_api->callIndirect(c, target); }
void checkIrq(Ctx& c) { g_api->checkIrq(c); }
void trap(Ctx& c, uint32_t pc, const char* what) { g_api->trap(c, pc, what); }
uint32_t readSpr(Ctx& c, uint32_t spr, uint32_t pc) { return g_api->readSpr(c, spr, pc); }
void writeSpr(Ctx& c, uint32_t spr, uint32_t v, uint32_t pc) { g_api->writeSpr(c, spr, v, pc); }
void writeMsr(Ctx& c, uint32_t v) { g_api->writeMsr(c, v); }
uint64_t timeBase() { return g_api->timeBase(); }
void psqLoad(Ctx& c, int fd, uint32_t ea, int w, int i) { g_api->psqLoad(c, fd, ea, w, i); }
void psqStore(Ctx& c, int fs, uint32_t ea, int w, int i) { g_api->psqStore(c, fs, ea, w, i); }
void dcbzL(Ctx& c, uint32_t ea) { g_api->dcbzL(c, ea); }
void rfi(Ctx& c) { g_api->rfi(c); }

}  // namespace guest

void guest_unresolved(guest::Ctx& c) { guest::trap(c, 0, "branch to unresolved address"); }

BLITZ_EXPORT const guest::GameModule* blitz_module_init(const guest::RuntimeApi* api) {
  if (!api || api->abiVersion != guest::kModuleAbiVersion) return nullptr;
  guest::g_api = api;
  guest::g_mem = api->mem;
  guest::g_lockedCache = api->lockedCache;
  guest::g_irqPendingPtr = api->irqPending;
  guest::g_moduleHle = api->hle;
  return guest::builtinModule();
}
