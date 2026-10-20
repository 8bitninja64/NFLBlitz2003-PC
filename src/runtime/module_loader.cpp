// Loads the game module (the DLL recompiled from the user's disc) and connects it to the
// runtime: services in, function table and HLE names out.
#include "module_loader.h"

#include "hle.h"
#include "log.h"
#include "module_abi.h"

#include <cstdlib>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace guest {

const FuncEntry* g_funcTable = nullptr;
uint32_t g_funcTableSize = 0;
uint32_t g_entryPoint = 0;

static const GameModule* g_module = nullptr;
static std::vector<Fn> g_hleTable;

static RuntimeApi makeApi() {
  RuntimeApi api{};
  api.abiVersion = kModuleAbiVersion;
  api.mem = g_mem;
  api.lockedCache = g_lockedCache;
  api.irqPending = &g_irqPending;
  api.mmioRead = mmioRead;
  api.mmioWrite = mmioWrite;
  api.slowPtr = slowPtr;
  api.callIndirect = callIndirect;
  api.checkIrq = checkIrq;
  api.trap = trap;
  api.readSpr = readSpr;
  api.writeSpr = writeSpr;
  api.writeMsr = writeMsr;
  api.timeBase = timeBase;
  api.psqLoad = psqLoad;
  api.psqStore = psqStore;
  api.dcbzL = dcbzL;
  api.rfi = rfi;
  return api;
}

std::string defaultModulePath() {
  if (const char* env = getenv("BLITZ_MODULE")) return env;
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s(buf, n);
  size_t slash = s.find_last_of("\\/");
  return (slash == std::string::npos ? std::string(".") : s.substr(0, slash)) + "/blitz_game.dll";
#else
  return "./blitz_game.so";
#endif
}

bool loadGameModule(const std::string& path, std::string* error) {
#ifdef _WIN32
  HMODULE lib = LoadLibraryA(path.c_str());
  if (!lib) {
    *error = "cannot load the game module '" + path + "' (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  auto init = reinterpret_cast<ModuleInitFn>(GetProcAddress(lib, kModuleInitSymbol));
#else
  ModuleInitFn init = nullptr;
#endif
  if (!init) {
    *error = "'" + path + "' is not a game module";
    return false;
  }
  // The HLE table is filled after init (the module tells us which functions it needs);
  // it is sized up front so the pointer handed over stays valid.
  g_hleTable.assign(512, nullptr);
  static RuntimeApi api = makeApi();
  api.hle = g_hleTable.data();
  const GameModule* mod = init(&api);
  if (!mod) {
    *error = "the game module was built for a different runtime version; rebuild it";
    return false;
  }
  std::string missing;
  for (uint32_t i = 0; i < mod->hleCount && i < g_hleTable.size(); i++) {
    g_hleTable[i] = hle::find(mod->hleNames[i]);
    if (!g_hleTable[i]) missing += std::string(missing.empty() ? "" : ", ") + mod->hleNames[i];
  }
  if (!missing.empty()) {
    *error = "the runtime lacks host functions the game module needs: " + missing;
    return false;
  }
  g_module = mod;
  g_funcTable = mod->table;
  g_funcTableSize = mod->tableSize;
  g_entryPoint = mod->entryPoint;
  setTraceRingSource(mod->traceRing);
  LOG_INFO("game module %s: %u functions, %u host functions, built from DOL %s", path.c_str(), mod->tableSize,
           mod->hleCount, mod->dolHash);
  return true;
}

const char* moduleDolHash() { return g_module ? g_module->dolHash : ""; }

std::string dolHash(const uint8_t* data, size_t size) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * 0x100000001B3ull;
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
  return buf;
}

}  // namespace guest
