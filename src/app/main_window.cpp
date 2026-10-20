// Windowed runner: Aurora (WebGPU + SDL3) on the main thread; the game runs on its own
// host threads and hands each finished frame over through gx_bridge.
#include "boot.h"
#include "audio_out.h"
#include "gx_bridge.h"
#include "hw.h"
#include "log.h"
#include "pad_input.h"
#include "sched.h"

#include "gx/fifo.hpp"  // Aurora internal: fifo::drain

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_scancode.h>
#include <dolphin/gx.h>
#include <dolphin/pad.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Keyboard defaults for port 1 (remappable later): WASD stick, arrows D-pad, IJKL face
// buttons, Q/E triggers, Space Z, Enter Start, numpad C-stick.
static void setDefaultKeys() {
  PADDefaultKeyBindings b{};
  const PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
      {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT}, {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT}, {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
      {SDL_SCANCODE_UP, PAD_BUTTON_UP},     {SDL_SCANCODE_SPACE, PAD_TRIGGER_Z},    {SDL_SCANCODE_E, PAD_TRIGGER_R},
      {SDL_SCANCODE_Q, PAD_TRIGGER_L},      {SDL_SCANCODE_K, PAD_BUTTON_A},         {SDL_SCANCODE_J, PAD_BUTTON_B},
      {SDL_SCANCODE_L, PAD_BUTTON_X},       {SDL_SCANCODE_I, PAD_BUTTON_Y},         {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
  };
  const PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
      {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 100},   {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 100},
      {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 100},   {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 100},
      {SDL_SCANCODE_KP_6, PAD_AXIS_RIGHT_X_POS, 100}, {SDL_SCANCODE_KP_4, PAD_AXIS_RIGHT_X_NEG, 100},
      {SDL_SCANCODE_KP_8, PAD_AXIS_RIGHT_Y_POS, 100}, {SDL_SCANCODE_KP_2, PAD_AXIS_RIGHT_Y_NEG, 100},
      {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 100},    {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_R, 100},
  };
  memcpy(b.buttons, buttons, sizeof(buttons));
  memcpy(b.axes, axes, sizeof(axes));
  if (!PADSetDefaultKeyBindings(0, &b)) LOG_WARN("could not set default keyboard bindings");
}

// Game audio (AI DMA output, 32 kHz stereo) to the default playback device.
static SDL_AudioStream* g_audioStream = nullptr;

static void audioSubmit(const int16_t* lr, uint32_t frames) {
  SDL_PutAudioStreamData(g_audioStream, lr, (int)(frames * 4));
}

static uint32_t audioQueued() {
  int bytes = SDL_GetAudioStreamQueued(g_audioStream);
  return bytes > 0 ? (uint32_t)bytes / 4 : 0;
}

static void initAudio() {
  if (getenv("BLITZ_MUTE")) return;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    LOG_WARN("audio unavailable: %s", SDL_GetError());
    return;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
  g_audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!g_audioStream) {
    LOG_WARN("could not open an audio device: %s", SDL_GetError());
    return;
  }
  SDL_ResumeAudioStreamDevice(g_audioStream);
  static const audioout::Sink sink{audioSubmit, audioQueued, 32000};
  audioout::setSink(&sink);
}

// Samples host controllers into pad_input and applies the game's rumble requests.
static void pollPads() {
  PADStatus st[PAD_MAX_CONTROLLERS] = {};
  PADRead(st);
  padinput::Pad pads[padinput::kPorts];
  for (int i = 0; i < padinput::kPorts; i++) {
    pads[i] = {st[i].button, st[i].stickX, st[i].stickY, st[i].substickX, st[i].substickY, st[i].triggerLeft,
               st[i].triggerRight, st[i].analogA, st[i].analogB, st[i].err};
    uint32_t cmd;
    if (padinput::takeMotor(i, &cmd)) PADControlMotor((u32)i, cmd);
  }
  padinput::publish(pads);
}

// 4:3 window at ~85% of the usable display height, centered. A window larger than the
// display gets its presents throttled by Windows.
static void fitWindowToDisplay(SDL_Window* win) {
  SDL_Rect usable;
  if (!win || !SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable)) return;
  int h = usable.h * 85 / 100;
  int w = h * 4 / 3;
  if (w > usable.w * 9 / 10) {
    w = usable.w * 9 / 10;
    h = w * 3 / 4;
  }
  SDL_SetWindowSize(win, w, h);
  SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

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
  config.vsync = getenv("BLITZ_VSYNC") != nullptr;  // the game paces itself on the VI retrace
#ifdef _WIN32
  // Dawn's D3D12 and Vulkan presents ran at ~20 fps even for empty frames on the dev
  // laptop (NVIDIA Optimus); D3D11 presents in ~4 ms.
  config.desiredBackend = BACKEND_D3D11;
#endif
  if (const char* b = getenv("BLITZ_BACKEND")) {  // d3d12 | vulkan | d3d11
    if (!strcmp(b, "vulkan")) config.desiredBackend = BACKEND_VULKAN;
    else if (!strcmp(b, "d3d12")) config.desiredBackend = BACKEND_D3D12;
    else if (!strcmp(b, "d3d11")) config.desiredBackend = BACKEND_D3D11;
  }
  config.mem1Size = 0;  // guest memory is owned by the runtime
  config.mem2Size = 0;
  aurora_initialize(argc, argv, &config);
  fitWindowToDisplay(aurora_get_window());
  if (getenv("BLITZ_TOPMOST")) SDL_SetWindowAlwaysOnTop(aurora_get_window(), true);  // for screenshots
  // Aurora's GX state defaults; the game's own GXInit stream then sets everything it uses.
  static uint8_t auroraFifo[0x40000];
  GXInit(auroraFifo, sizeof(auroraFifo));
  setDefaultKeys();
  PADInit();
  PADSetKeyboardActive(0, TRUE);

  if (getenv("BLITZ_AURORA_BENCH")) {  // bare Aurora loop timing, no game
    auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    while (n < 300) {
      aurora_update();
      if (aurora_begin_frame()) {
        aurora_end_frame();
        n++;
      }
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("aurora bench: %.2f ms per frame", ms / n);
    aurora_shutdown();
    return 0;
  }

  gxbridge::init();
  initAudio();
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
    pollPads();
    if (!gxbridge::takeFrame(frame, 8)) continue;
    using clk = std::chrono::steady_clock;
    static double tBegin = 0, tProc = 0, tEnd = 0, tTake = 0;
    auto t0 = clk::now();
    bool began = aurora_begin_frame();
    auto t1 = clk::now();
    if (began) {
      static const bool empty = getenv("BLITZ_EMPTY_FRAMES") != nullptr;  // pacing diagnostics
      if (!empty) GXCallDisplayList(frame.data(), (u32)frame.size());
      // Once the FIFO is processed Aurora holds its own copies of everything it read from
      // guest memory: let the game build its next frame while this one is presented.
      aurora::gx::fifo::drain();
      gxbridge::frameConsumed();
      auto t2 = clk::now();
      aurora_end_frame();
      auto t3 = clk::now();
      tBegin += std::chrono::duration<double, std::milli>(t1 - t0).count();
      tProc += std::chrono::duration<double, std::milli>(t2 - t1).count();
      tEnd += std::chrono::duration<double, std::milli>(t3 - t2).count();
      static const bool perf = getenv("BLITZ_PERF") != nullptr;
      if (perf && frames % 100 == 99) {
        LOG_INFO("aurora: begin %.1f ms, process %.1f ms, end %.1f ms per frame", tBegin / 100, tProc / 100, tEnd / 100);
        tBegin = tProc = tEnd = 0;
      }
    } else {
      gxbridge::frameConsumed();
    }
    if (++frames % 600 == 0) LOG_INFO("presented %llu frames", (unsigned long long)frames);
  }
  gxbridge::shutdown();
  aurora_shutdown();
  return 0;
}
