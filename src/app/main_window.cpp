// Windowed runner: Aurora (WebGPU + SDL3) on the main thread; the game runs on its own
// host threads and hands each finished frame over through gx_bridge.
#include "boot.h"
#include "gx_bridge.h"
#include "hw.h"
#include "log.h"
#include "sched.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <SDL3/SDL_video.h>
#include <dolphin/gx.h>

#include <cstdlib>
#include <thread>
#include <vector>

static void auroraLog(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
  LogLevel l = level == LOG_DEBUG ? LogLevel::Debug
               : level == LOG_INFO ? LogLevel::Info
               : level == LOG_WARNING ? LogLevel::Warn
               : level == LOG_ERROR ? LogLevel::Error
                                    : LogLevel::Fatal;
  logf(l, "[aurora:%s] %.*s", module, (int)len, message);
  if (level == LOG_FATAL) abort();
}

int main(int argc, char** argv) {
  if (!guest::bootFromArgs(argc, argv)) return 1;

  AuroraConfig config{};
  config.appName = "NFL Blitz 2003";
  config.logCallback = auroraLog;
  config.logLevel = getenv("BLITZ_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
  config.windowWidth = 1280;
  config.windowHeight = 960;
  config.vsync = true;
  config.mem1Size = 0;  // guest memory is owned by the runtime
  config.mem2Size = 0;
  aurora_initialize(argc, argv, &config);
  if (getenv("BLITZ_TOPMOST")) SDL_SetWindowAlwaysOnTop(aurora_get_window(), true);  // for screenshots
  // Aurora's GX state defaults; the game's own GXInit stream then sets everything it uses.
  static uint8_t auroraFifo[0x40000];
  GXInit(auroraFifo, sizeof(auroraFifo));

  gxbridge::init();
  hw::start();
  std::thread([] { sched::runMainThread(guest::g_boot.entry); }).detach();

  std::vector<uint8_t> frame;
  uint64_t frames = 0;
  bool exiting = false;
  while (!exiting) {
    const AuroraEvent* event = aurora_update();
    while (event && event->type != AURORA_NONE) {
      if (event->type == AURORA_EXIT) exiting = true;
      ++event;
    }
    if (exiting) break;
    if (!gxbridge::takeFrame(frame, 16)) continue;
    if (aurora_begin_frame()) {
      GXCallDisplayList(frame.data(), (u32)frame.size());
      aurora_end_frame();
    }
    gxbridge::frameConsumed();
    if (++frames % 600 == 0) LOG_INFO("presented %llu frames", (unsigned long long)frames);
  }
  gxbridge::shutdown();
  aurora_shutdown();
  return 0;
}
