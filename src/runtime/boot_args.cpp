// Shared startup: command line, disc, guest memory, devices, executable.
#include "boot.h"
#include "disc.h"
#include "func_table.h"
#include "gx_fifo.h"
#include "hw.h"
#include "log.h"
#include "memory.h"
#include "module_loader.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace guest {

static bool fileExists(const std::string& p) {
  if (p.empty()) return false;
  FILE* f = fopen(p.c_str(), "rb");
  if (f) fclose(f);
  return f != nullptr;
}

static std::string exeDir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s(buf, n);
  size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
#else
  std::error_code ec;
  std::string dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string();
  return dir.empty() ? std::string(".") : dir;
#endif
}

#ifdef _WIN32
static const char* kModuleName = "blitz_game.dll";
static const char* kModuleTool = "tools/make_module.ps1";
#else
static const char* kModuleName = "blitz_game.so";
static const char* kModuleTool = "tools/make_module.sh";
#endif

bool bootFromArgs(int argc, char** argv, BootOptions* opts) {
#ifdef _WIN32
  // 1 ms scheduler granularity: retrace timing and Aurora's GPU-progress sleeps depend on it
  // (the default 15.6 ms tick turns sub-millisecond sleeps into whole ticks).
  timeBeginPeriod(1);
  // Windows 11 ignores that request (and slows timers down) while the window is not the visible
  // foreground one, which turned the 59.94 Hz pacing into 3 x 15.6 ms ticks (20 fps): opt out of
  // timer-resolution and execution-speed (EcoQoS) throttling.
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
#endif
  installCrashHandler();
  // Disc: --disc, BLITZ_DISC, the remembered path, the default name in the working
  // directory or next to the executable, else ask (window app).
  std::string discPath;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--disc") && i + 1 < argc) discPath = argv[++i];
    else if (!strcmp(argv[i], "-v")) setLogLevel(LogLevel::Debug);
  }
  if (const char* d = getenv("BLITZ_DISC")) discPath = d;
  const char* kDefault = "NFL Blitz 2003 (USA).iso";
  std::string candidates[] = {discPath, opts ? opts->rememberedDisc : std::string(), kDefault,
                              exeDir() + "/" + kDefault};
  discPath.clear();
  for (const std::string& cand : candidates)
    if (fileExists(cand)) {
      discPath = cand;
      break;
    }
  if (discPath.empty() && opts && opts->pickDisc) discPath = opts->pickDisc();
  if (discPath.empty() || !g_disc.open(discPath)) {
    std::string msg = discPath.empty() ? "No NFL Blitz 2003 disc image was selected."
                                       : "Cannot open the disc image '" + discPath + "'.";
    LOG_ERROR("%s (use --disc <path>)", msg.c_str());
    if (opts && opts->showError)
      opts->showError(msg + "\n\nPlease provide your own NFL Blitz 2003 (USA) GameCube disc image (.iso or .rvz).");
    return false;
  }
  if (g_disc.gameId() != "GO3E5D") {
    std::string msg = "'" + discPath + "' is not NFL Blitz 2003 (USA) (game ID " + g_disc.gameId() + ", expected GO3E5D).";
    LOG_ERROR("%s", msg.c_str());
    if (opts && opts->showError) opts->showError(msg);
    return false;
  }
  if (opts) opts->chosenDisc = discPath;
  LOG_INFO("disc: %s", discPath.c_str());

  initMemory();
  hw::init();
  gxfifo::init();

  // The game code: a module recompiled from this disc's executable.
  auto fail = [&](const std::string& msg) {
    LOG_ERROR("%s", msg.c_str());
    if (opts && opts->showError) opts->showError(msg);
    return false;
  };
  uint8_t hdr[0x100];
  if (!g_disc.read(g_disc.dolOffset(), hdr, sizeof(hdr))) return fail("cannot read the executable from the disc");
  uint32_t dolSize = 0;
  for (int i = 0; i < 18; i++) {
    auto be = [&](int o) { return (uint32_t)hdr[o] << 24 | hdr[o + 1] << 16 | hdr[o + 2] << 8 | hdr[o + 3]; };
    uint32_t end = be(i * 4) + be(0x90 + i * 4);
    if (be(0x90 + i * 4) && end > dolSize) dolSize = end;
  }
  std::vector<uint8_t> dol(dolSize);
  if (!g_disc.read(g_disc.dolOffset(), dol.data(), dolSize)) return fail("cannot read the executable from the disc");
  const std::string hash = dolHash(dol.data(), dol.size());

  // Modules: the given/default path, then one built earlier into the user directory. The first
  // that loads and was recompiled from this disc's executable wins.
  std::string error;
  auto tryLoad = [&](const std::string& path) {
    if (!fileExists(path)) return false;
    std::string err;
    if (!loadGameModule(path, &err)) {
      error = err;
      return false;
    }
    if (hash == moduleDolHash()) return true;
    error = "The game module " + path + " was built from a different executable (" + moduleDolHash() +
            ", this disc has " + hash + ").";
    unloadGameModule();
    return false;
  };
  std::string primary = opts && !opts->modulePath.empty() ? opts->modulePath : defaultModulePath();
  std::string built = opts && !opts->userDir.empty() ? opts->userDir + "/" + kModuleName : std::string();
  bool loaded = tryLoad(primary) || (!built.empty() && tryLoad(built));
  if (!loaded && opts && opts->buildModule && !built.empty()) {
    std::string dolPath = opts->userDir + "/module_build/main.dol";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(opts->userDir + "/module_build"), ec);
    std::ofstream out(std::filesystem::u8path(dolPath), std::ios::binary);
    out.write(reinterpret_cast<const char*>(dol.data()), (std::streamsize)dol.size());
    out.close();
    if (!out) return fail("cannot write " + dolPath);
    if (!opts->buildModule(dolPath, built)) return false;  // it reported why
    loaded = tryLoad(built);
  }
  if (!loaded) {
    if (error.empty()) error = "The game module (" + primary + ") is missing. Build it from your disc with " + kModuleTool + ".";
    return fail(error);
  }

  if (!loadDol() || !setupLowMemory()) return fail("failed to load the executable from the disc");
  LOG_INFO("entry %08X, %u recompiled functions", g_boot.entry, g_funcTableSize);
  return true;
}

}  // namespace guest
