// Shared startup: command line, disc, guest memory, devices, executable.
#include "boot.h"
#include "disc.h"
#include "func_table.h"
#include "gx_fifo.h"
#include "hw.h"
#include "log.h"
#include "memory.h"

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
  return ".";
#endif
}

bool bootFromArgs(int argc, char** argv, BootOptions* opts) {
#ifdef _WIN32
  // 1 ms scheduler granularity: retrace timing and Aurora's GPU-progress sleeps depend on it
  // (the default 15.6 ms tick turns sub-millisecond sleeps into whole ticks).
  timeBeginPeriod(1);
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
  if (!loadDol() || !setupLowMemory()) {
    LOG_ERROR("failed to load the executable from the disc");
    return false;
  }
  LOG_INFO("entry %08X, %u recompiled functions", g_boot.entry, g_funcTableSize);
  return true;
}

}  // namespace guest
