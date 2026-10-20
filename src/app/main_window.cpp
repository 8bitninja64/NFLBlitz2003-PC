// Windowed runner: Aurora (WebGPU + SDL3) on the main thread; the game runs on its own
// host threads and hands each finished frame over through gx_bridge.
#include "boot.h"
#include "audio_out.h"
#include "gx_bridge.h"
#include "hw.h"
#include "log.h"
#include "overlay.h"
#include "pad_input.h"
#include "sched.h"
#include "settings.h"

#include "gx/fifo.hpp"  // Aurora internal: fifo::drain

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#endif

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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

static void setVolume(float gain) {
  if (g_audioStream) SDL_SetAudioStreamGain(g_audioStream, gain);
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
  setVolume(settings::get().mute ? 0.0f : settings::get().volume / 100.0f);
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
    if (overlay::isOpen()) pads[i] = {0, 0, 0, 0, 0, 0, 0, 0, 0, st[i].err};  // the menu has the input
    uint32_t cmd;
    if (padinput::takeMotor(i, &cmd)) PADControlMotor((u32)i, cmd);
  }
  padinput::publish(pads);
}

// Window at ~85% of the usable display height (16:9 with widescreen, else 4:3), centered.
static void fitWindowToDisplay(SDL_Window* win, bool widescreen) {
  SDL_Rect usable;
  if (!win || !SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(win), &usable)) return;
  const int aw = widescreen ? 16 : 4, ah = widescreen ? 9 : 3;
  int h = usable.h * 85 / 100;
  int w = h * aw / ah;
  if (w > usable.w * 9 / 10) {
    w = usable.w * 9 / 10;
    h = w * ah / aw;
  }
  SDL_SetWindowSize(win, w, h);
  SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(win, &pw, &ph);
  LOG_INFO("window: usable %dx%d, size %dx%d, pixels %dx%d, display scale %.2f", usable.w, usable.h, w, h, pw, ph,
           SDL_GetWindowDisplayScale(win));
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

static std::string userDataDir() {
#ifdef _WIN32
  const char* base = getenv("APPDATA");
  std::string dir = std::string(base ? base : ".") + "/NFL Blitz 2003";
#else
  const char* home = getenv("HOME");
  std::string dir = std::string(home ? home : ".") + "/.local/share/NFL Blitz 2003";
#endif
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  return dir;
}

static AuroraBackend backendFromSettings(const std::string& name) {
  if (name == "vulkan") return BACKEND_VULKAN;
  if (name == "d3d12") return BACKEND_D3D12;
  if (name == "d3d11") return BACKEND_D3D11;
#ifdef _WIN32
  // Dawn's D3D12 and Vulkan presents ran at ~20 fps even for empty frames on the dev
  // laptop (NVIDIA Optimus); D3D11 presents in ~4 ms.
  return BACKEND_D3D11;
#else
  return BACKEND_AUTO;
#endif
}

static void setFullscreen(bool on) {
  VISetWindowFullscreen(on);
  settings::get().fullscreen = on;
  settings::save();
}

static void applyDisplay() {
  const settings::Settings& st = settings::get();
  AuroraSetViewportPolicy(st.widescreen ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
  VISetFrameBufferScale(st.resolutionScale);  // 0 = window size, N = N x 480 lines
}

static bool g_quit = false;
static void requestQuit() { g_quit = true; }

// Alt+Enter / F11: fullscreen. F1: settings menu (pauses). Esc: close the menu.
static void handleHotkeys() {
  static bool wasDown = false, f1WasDown = false;
  const bool* keys = SDL_GetKeyboardState(nullptr);
  bool f1 = keys[SDL_SCANCODE_F1];
  if (f1 && !f1WasDown) overlay::toggle();
  f1WasDown = f1;
  if (keys[SDL_SCANCODE_ESCAPE] && overlay::isOpen()) overlay::close();
  bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
  bool down = keys[SDL_SCANCODE_F11] || (alt && keys[SDL_SCANCODE_RETURN]);
  if (down && !wasDown) setFullscreen(!VIGetWindowFullscreen());
  wasDown = down;
}

// Aspect ratio of the content framebuffer, which the bridge's projection patch targets.
static void updateAspect() {
  AuroraWindowSize ws = aurora_get_window_size();
  float aspect = 4.0f / 3.0f;
  if (settings::get().widescreen && ws.fb_width && ws.fb_height) aspect = (float)ws.fb_width / (float)ws.fb_height;
  settings::g_displayAspect.store(aspect);
  static uint32_t lastW = 0, lastH = 0;
  if (ws.fb_width != lastW || ws.fb_height != lastH) {
    lastW = ws.fb_width;
    lastH = ws.fb_height;
    LOG_INFO("render target %ux%u (window %ux%u)", ws.fb_width, ws.fb_height, ws.native_fb_width, ws.native_fb_height);
  }
}

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

static std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

static std::string pickDisc() {
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrTitle = L"Select your NFL Blitz 2003 (USA) GameCube disc image";
  ofn.lpstrFilter = L"GameCube disc image (*.iso;*.gcm)\0*.iso;*.gcm\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? narrow(file) : std::string();
}

static void showError(const std::string& msg) {
  MessageBoxW(nullptr, widen(msg).c_str(), L"NFL Blitz 2003", MB_OK | MB_ICONERROR);
}
#endif

int main(int argc, char** argv) {
  settings::load(userDataDir() + "/settings.ini");
  settings::Settings& st = settings::get();
  guest::BootOptions boot;
  boot.rememberedDisc = st.disc;
#ifdef _WIN32
  boot.pickDisc = pickDisc;
  boot.showError = showError;
#endif
  if (!guest::bootFromArgs(argc, argv, &boot)) return 1;
  if (!getenv("BLITZ_DISC")) {
    std::error_code ec;
    auto u8 = std::filesystem::absolute(std::filesystem::u8path(boot.chosenDisc), ec).u8string();
    std::string abs(reinterpret_cast<const char*>(u8.c_str()), u8.size());
    if (!ec && abs != st.disc) {
      st.disc = abs;
      settings::save();
    }
  }

  AuroraConfig config{};
  config.appName = "NFL Blitz 2003";
  config.logCallback = auroraLog;
  config.logLevel = getenv("BLITZ_AURORA_DEBUG") ? LOG_DEBUG : LOG_INFO;
  config.windowWidth = st.windowWidth > 0 ? (uint32_t)st.windowWidth : 1280;
  config.windowHeight = st.windowHeight > 0 ? (uint32_t)st.windowHeight : 720;
  config.vsync = st.vsync || getenv("BLITZ_VSYNC");  // the game paces itself on the VI retrace
  config.msaa = st.msaa;
  config.maxTextureAnisotropy = (uint16_t)st.anisotropy;
  config.startFullscreen = st.fullscreen;
  config.desiredBackend = backendFromSettings(getenv("BLITZ_BACKEND") ? getenv("BLITZ_BACKEND") : st.backend);
  config.mem1Size = 0;  // guest memory is owned by the runtime
  config.mem2Size = 0;
  aurora_initialize(argc, argv, &config);
  if (st.windowWidth <= 0 || st.windowHeight <= 0) fitWindowToDisplay(aurora_get_window(), st.widescreen);
  applyDisplay();
  overlay::init({setFullscreen, applyDisplay, setVolume, requestQuit});
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
  bool holding = false;  // a guest frame kept (not consumed) while paused
  float fps = 0.0f;
  uint64_t fpsFrames = 0;
  auto fpsStart = std::chrono::steady_clock::now();
  while (!exiting && !g_quit) {
    const AuroraEvent* event = aurora_update();
    while (event && event->type != AURORA_NONE) {
      if (event->type == AURORA_EXIT) exiting = true;
      ++event;
    }
    if (exiting) break;
    handleHotkeys();
    if (const char* at = getenv("BLITZ_MENU_AT")) {  // test hook: open the menu after N seconds
      static bool opened = false;
      static const auto start = std::chrono::steady_clock::now();
      if (!opened && std::chrono::steady_clock::now() - start > std::chrono::seconds(atoi(at))) {
        opened = true;
        if (!overlay::isOpen()) overlay::toggle();
      }
    }
    updateAspect();
    pollPads();
    const bool paused = overlay::isOpen();

    // While paused the last frame is shown again (its data stays valid: the guest waits at
    // the next handoff), so the menu has something to draw over.
    bool haveFrame = holding || gxbridge::takeFrame(frame, 8);
    if (!haveFrame && !paused) continue;
    if (paused) std::this_thread::sleep_for(std::chrono::milliseconds(12));

    using clk = std::chrono::steady_clock;
    static double tBegin = 0, tProc = 0, tEnd = 0;
    auto t0 = clk::now();
    bool began = aurora_begin_frame();
    auto t1 = clk::now();
    if (began) {
      static const bool empty = getenv("BLITZ_EMPTY_FRAMES") != nullptr;  // pacing diagnostics
      if (haveFrame && !empty) GXCallDisplayList(frame.data(), (u32)frame.size());
      overlay::draw(fps);
      // Once the FIFO is processed Aurora holds its own copies of everything it read from
      // guest memory: let the game build its next frame while this one is presented.
      aurora::gx::fifo::drain();
    }
    if (haveFrame && !paused) {
      gxbridge::frameConsumed();
      holding = false;
      fpsFrames++;
    } else if (haveFrame) {
      holding = true;
    }
    // Devices stop only once a frame is held: the game needs retraces to finish that frame.
    hw::g_paused.store(paused && holding);
    if (began) {
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
    }
    auto now = clk::now();
    double span = std::chrono::duration<double>(now - fpsStart).count();
    if (span >= 0.5) {
      fps = (float)(fpsFrames / span);
      fpsFrames = 0;
      fpsStart = now;
    }
    if (++frames % 600 == 0) LOG_INFO("presented %llu frames", (unsigned long long)frames);
  }
  hw::g_paused.store(false);
  gxbridge::shutdown();
  aurora_shutdown();
  return 0;
}
