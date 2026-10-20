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

#include <cstdlib>
#include <cstring>
#include <string>

namespace guest {

bool bootFromArgs(int argc, char** argv) {
#ifdef _WIN32
  // 1 ms scheduler granularity: retrace timing and Aurora's GPU-progress sleeps depend on it
  // (the default 15.6 ms tick turns sub-millisecond sleeps into whole ticks).
  timeBeginPeriod(1);
#endif
  installCrashHandler();
  std::string discPath = "NFL Blitz 2003 (USA).iso";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--disc") && i + 1 < argc) discPath = argv[++i];
    else if (!strcmp(argv[i], "-v")) setLogLevel(LogLevel::Debug);
  }
  if (const char* d = getenv("BLITZ_DISC")) discPath = d;

  if (!g_disc.open(discPath)) {
    LOG_ERROR("cannot open disc image '%s' (use --disc <path>)", discPath.c_str());
    return false;
  }
  if (g_disc.gameId() != "GO3E5D") LOG_WARN("unexpected game ID %s (expected GO3E5D)", g_disc.gameId().c_str());

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
