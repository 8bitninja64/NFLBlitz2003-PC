// Windowed runner: Aurora (WebGPU + SDL3) on the main thread; the game runs on its own
// host threads and hands each finished frame over through gx_bridge.
#include "boot.h"
#include "audio_out.h"
#include "gx_bridge.h"
#include "hw.h"
#include "log.h"
#include "first_start.h"
#include "update_check.h"
#include "frame_shot.h"
#include "overlay.h"
#include "pad_input.h"
#include "guest_sched.h"
#include "disc.h"
#include "settings.h"
#include "widescreen.h"

#include "gx/fifo.hpp"  // Aurora internal: fifo::drain

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <psapi.h>
#else
#include <sys/wait.h>
#endif

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_scancode.h>
#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <aurora/texture.hpp>  // after the dolphin headers (uses GX types)

#include <chrono>
#include <mutex>
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

// Game audio (AI DMA output, 32 kHz stereo) to the default playback device. The AI DMA thread keeps
// submitting until the process ends: the lock lets quitting close the stream before SDL shuts down.
static SDL_AudioStream* g_audioStream = nullptr;
static std::mutex g_audioLock;

static void audioSubmit(const int16_t* lr, uint32_t frames) {
  std::lock_guard lk(g_audioLock);
  if (g_audioStream) SDL_PutAudioStreamData(g_audioStream, lr, (int)(frames * 4));
}

static uint32_t audioQueued() {
  std::lock_guard lk(g_audioLock);
  int bytes = g_audioStream ? SDL_GetAudioStreamQueued(g_audioStream) : 0;
  return bytes > 0 ? (uint32_t)bytes / 4 : 0;
}

static void setVolume(float gain) {
  std::lock_guard lk(g_audioLock);
  if (g_audioStream) SDL_SetAudioStreamGain(g_audioStream, gain);
}

static void closeAudio() {
  std::lock_guard lk(g_audioLock);
  if (g_audioStream) SDL_DestroyAudioStream(g_audioStream);
  g_audioStream = nullptr;
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
  LOG_INFO("audio: %s driver, device open", SDL_GetCurrentAudioDriver());
  static const audioout::Sink sink{audioSubmit, audioQueued, 32000};
  audioout::setSink(&sink);
}

// Samples host controllers into pad_input and applies the game's rumble requests.
static void pollPads() {
  PADStatus st[PAD_MAX_CONTROLLERS] = {};
  PADRead(st);
  padinput::Pad pads[padinput::kPorts];
  unsigned short buttons[4];
  signed char sx[4], sy[4];
  for (int i = 0; i < 4; i++) {
    bool ok = i < PAD_MAX_CONTROLLERS && st[i].err == PAD_ERR_NONE;
    buttons[i] = ok ? st[i].button : 0;
    sx[i] = ok ? st[i].stickX : 0;
    sy[i] = ok ? st[i].stickY : 0;
  }
  // The Back/View/Select button of a PC gamepad also opens the menu (GameCube pads: Z+Start).
  static bool backWas = false;
  bool back = false;
  int nGamepads = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&nGamepads)) {
    for (int i = 0; i < nGamepads; i++)
      if (SDL_Gamepad* gp = SDL_GetGamepadFromID(ids[i])) back |= SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK);
    SDL_free(ids);
  }
  if (back && !backWas) overlay::toggle();
  backWas = back;
  overlay::padInput(buttons, sx, sy);
  for (int i = 0; i < padinput::kPorts; i++) {
    pads[i] = {st[i].button, st[i].stickX, st[i].stickY, st[i].substickX, st[i].substickY, st[i].triggerLeft,
               st[i].triggerRight, st[i].analogA, st[i].analogB, st[i].err};
    if (overlay::ownsPads()) pads[i] = {0, 0, 0, 0, 0, 0, 0, 0, 0, st[i].err};  // the menu has the input
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
  // SDL's rule ($XDG_DATA_HOME, else ~/.local/share), which Aurora's memory card and caches follow too.
  std::string dir;
  if (char* pref = SDL_GetPrefPath(nullptr, "NFL Blitz 2003")) {
    dir = pref;
    SDL_free(pref);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  } else {
    const char* home = getenv("HOME");
    dir = std::string(home ? home : ".") + "/.local/share/NFL Blitz 2003";
  }
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

static size_t processMemory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) return pmc.PrivateUsage;
#endif
  return 0;
}

static bool g_quit = false;
static void requestQuit() { g_quit = true; }

// Menu > Update / Disc > Restart now: the game starts again with this one's arguments (without a
// --disc, which would override a newly chosen disc).
static int g_argc = 0;
static char** g_argv = nullptr;
static void restartGame() {
  if (update::restart(g_argc, g_argv)) requestQuit();
  else LOG_ERROR("could not start the game again");
}

// Alt+Enter / F11: fullscreen. Esc: opens and closes the settings menu (pauses), unless a key
// binding or a popup in the menu is waiting for it.
static void handleHotkeys() {
  static bool wasDown = false, escWasDown = false;
  const bool* keys = SDL_GetKeyboardState(nullptr);
  bool esc = keys[SDL_SCANCODE_ESCAPE];
  if (esc && !escWasDown && !overlay::wantsEscape()) overlay::toggle();
  escWasDown = esc;
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
  ofn.lpstrFilter = L"GameCube disc image (*.iso;*.gcm;*.rvz;*.wia;*.ciso;*.gcz;*.nfs)\0"
                    L"*.iso;*.gcm;*.rvz;*.wia;*.ciso;*.gcz;*.nfs\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? narrow(file) : std::string();
}

static void showError(const std::string& msg) {
  MessageBoxW(nullptr, widen(msg).c_str(), L"NFL Blitz 2003", MB_OK | MB_ICONERROR);
}

// First run: compile the game code from the player's disc with tools/make_module.ps1 (next to
// the executable), in a console window that shows the progress.
static bool buildModule(const std::string& dolPath, const std::string& outPath) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::filesystem::path root = std::filesystem::path(exe).parent_path();
  std::filesystem::path script = root / "tools" / "make_module.ps1";
  if (!std::filesystem::exists(script)) {
    showError("The game code has not been compiled yet, and the setup files (tools/make_module.ps1) are missing "
              "next to NFLBlitz2003.exe.");
    return false;
  }
  if (!getenv("BLITZ_AUTO_BUILD") &&
      MessageBoxW(nullptr,
                  L"NFL Blitz 2003 needs to compile the game code from your disc. This is done once and takes "
                  L"about a minute.\n\nRequirements: Python 3 and Visual Studio 2022 (or its Build Tools) with the "
                  L"C++ and Clang components.\n\nCompile now?",
                  L"NFL Blitz 2003 - first start", MB_YESNO | MB_ICONINFORMATION) != IDYES)
    return false;
  std::filesystem::path work = std::filesystem::u8path(dolPath).parent_path();
  std::wstring cmd = L"powershell -NoProfile -ExecutionPolicy Bypass -Command \"& '" + script.wstring() +
                     L"' -Dol '" + widen(dolPath) + L"' -Out '" + widen(outPath) + L"' -Work '" + work.wstring() +
                     L"'; $c = $LASTEXITCODE; if ($c) { Read-Host 'Compiling failed. Press Enter to close' }; exit $c\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr,
                      root.wstring().c_str(), &si, &pi)) {
    showError("Cannot start PowerShell to compile the game code.");
    return false;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) showError("Compiling the game code failed (see the console output).");
  return code == 0;
}
#else
static std::string exeDir() {
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string();
}

// The desktop's file chooser (zenity on GNOME, SteamOS and most others; kdialog on KDE).
static bool g_noChooser = false;

static std::string pickDisc() {
  const char* cmds[] = {
      "zenity --file-selection --title='Select your NFL Blitz 2003 (USA) GameCube disc image' "
      "--file-filter='GameCube disc image | *.[iI][sS][oO] *.[gG][cC][mM] *.[rR][vV][zZ] *.[wW][iI][aA] "
      "*.[cC][iI][sS][oO] *.[gG][cC][zZ] *.[nN][fF][sS]' --file-filter='All files | *' 2>/dev/null",
      "kdialog --title 'Select your NFL Blitz 2003 (USA) GameCube disc image' "
      "--getopenfilename . 'GameCube disc image (*.iso *.gcm *.rvz *.wia *.ciso *.gcz *.nfs)' 2>/dev/null"};
  for (const char* cmd : cmds) {
    FILE* p = popen(cmd, "r");
    if (!p) continue;
    std::string out;
    char buf[512];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (status == 0 && !out.empty()) return out;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 1) return {};  // cancelled
    // 127: not installed (sh: command not found); anything else: it could not show itself.
  }
  g_noChooser = true;
  LOG_ERROR("no file chooser (zenity or kdialog) could be shown: start the game with --disc <path to your disc image>");
  return {};
}

static void showError(const std::string& msg) {
  std::string text = msg;
  if (g_noChooser)
    text += "\n\nNo file chooser (zenity or kdialog) could be shown to pick it. Start the game with "
            "--disc \"/path/to/NFL Blitz 2003 (USA).iso\" (in Steam: Properties > Launch Options).";
  if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "NFL Blitz 2003", text.c_str(), nullptr))
    LOG_ERROR("cannot show the error message (%s)", SDL_GetError());
}

// First run: compile the game code from the player's disc with tools/make_module.sh (next to the
// executable), in a window that shows the progress (first_start.cpp).
static bool buildModule(const std::string& dolPath, const std::string& outPath) {
  std::filesystem::path script = std::filesystem::path(exeDir()) / "tools" / "make_module.sh";
  if (!std::filesystem::exists(script)) {
    showError("The game code has not been compiled yet, and the setup files (tools/make_module.sh) are missing "
              "next to NFLBlitz2003.");
    return false;
  }
  return firststart::build(script.string(), dolPath, outPath, !getenv("BLITZ_AUTO_BUILD"));
}
#endif

// Menu > Disc: the player picks another disc image; it must be NFL Blitz 2003 (USA). Saved for the
// next start (the running game keeps its disc).
static bool chooseDisc(std::string* message) {
  std::string path = pickDisc();
  if (path.empty()) {
#ifndef _WIN32
    if (g_noChooser) *message = "No file chooser (zenity or kdialog) could be shown: set disc = in " + settings::path() + ".";
#endif
    return false;
  }
  Disc disc;
  if (!disc.open(path)) {
    *message = "Cannot open " + path + ".";
    return false;
  }
  if (disc.gameId() != "GO3E5D") {
    *message = "That is not NFL Blitz 2003 (USA) (game ID " + disc.gameId() + ", expected GO3E5D).";
    return false;
  }
  std::error_code ec;
  auto u8 = std::filesystem::absolute(std::filesystem::u8path(path), ec).u8string();
  settings::get().disc = ec ? path : std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
  settings::save();
  LOG_INFO("disc for the next start: %s", settings::get().disc.c_str());
  return true;
}

// Aurora's persistent shader caches (pipeline_cache.db, dawn_cache.db) do not notice when the
// shader generator changes, so a new build could keep using shaders from an older one (players
// lost helmets and jersey panels that way). Clear them whenever this executable changes.
static void invalidateShaderCachesOnUpdate(const std::string& dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
#ifdef _WIN32
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  fs::path self(exe);
#else
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
#endif
  auto size = fs::file_size(self, ec);
  auto time = fs::last_write_time(self, ec).time_since_epoch().count();
  std::string stamp = std::to_string(size) + ":" + std::to_string(time);
  fs::path stampFile = fs::u8path(dir) / "shader_cache.stamp";
  std::string old;
  if (FILE* f = fopen(stampFile.string().c_str(), "rb")) {
    char buf[64] = {};
    old.assign(buf, fread(buf, 1, sizeof(buf) - 1, f));
    fclose(f);
  }
  if (old == stamp) return;
  for (const char* name : {"pipeline_cache.db", "pipeline_cache.db-shm", "pipeline_cache.db-wal", "dawn_cache.db",
                           "dawn_cache.db-shm", "dawn_cache.db-wal"})
    fs::remove(fs::u8path(dir) / name, ec);
  if (FILE* f = fopen(stampFile.string().c_str(), "wb")) {
    fwrite(stamp.data(), 1, stamp.size(), f);
    fclose(f);
  }
  LOG_INFO("new build: cleared the shader caches");
}

int main(int argc, char** argv) {
  g_argc = argc;
  g_argv = argv;
  update::cleanup();  // the files the last update replaced
  invalidateShaderCachesOnUpdate(userDataDir());
  settings::load(userDataDir() + "/settings.ini");
  settings::Settings& st = settings::get();
  guest::BootOptions boot;
  boot.rememberedDisc = st.disc;
  boot.pickDisc = pickDisc;
  boot.showError = showError;
  boot.buildModule = buildModule;
  boot.userDir = userDataDir();
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
  config.allowCpuAdapter = getenv("BLITZ_CPU_GPU") != nullptr;  // software renderers (lavapipe): tests only
  config.allowTextureDumps = st.dumpTextures;
  config.mem1Size = 0;  // guest memory is owned by the runtime
  config.mem2Size = 0;
  aurora_initialize(argc, argv, &config);
  // Aurora falls back to its Null backend when no graphics adapter works: the game would run with
  // sound and no picture. Say why instead (BLITZ_ALLOW_NULL_GPU=1 keeps running, for tests).
  if (aurora_get_backend() == BACKEND_NULL && !getenv("BLITZ_ALLOW_NULL_GPU")) {
    showError("No usable graphics driver was found. NFL Blitz 2003 needs Vulkan"
#ifdef _WIN32
              " or Direct3D 11/12"
#endif
              " (update or install your GPU driver; on Linux, Mesa's Vulkan drivers or NVIDIA's). The log "
              "names the adapters that were tried.");
    aurora_shutdown();
    fflush(nullptr);
    std::_Exit(1);
  }
  if (st.windowWidth <= 0 || st.windowHeight <= 0) fitWindowToDisplay(aurora_get_window(), st.widescreen);
  applyDisplay();
  if (st.texturePacks) {
    // Dolphin-format packs (tex1_WxH_hash_fmt.png ...), e.g. from an existing GO3E5D pack.
    auto packDir = std::filesystem::u8path(userDataDir()) / "Textures" / "GO3E5D";
    auto group = aurora::texture::load_replacement_directory(packDir);
    (void)group;
  }
  overlay::init({setFullscreen, applyDisplay, setVolume, requestQuit, restartGame, chooseDisc});
  if (st.updateCheck && !getenv("BLITZ_NO_UPDATE_CHECK")) update::checkStart();
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
  frameshot::init();
  initAudio();
  hw::start();
  std::thread([] { sched::runMainThread(guest::g_boot.entry); }).detach();

  std::vector<uint8_t> frame;
  uint64_t frames = 0;
  bool exiting = false;
  bool holding = false;  // a guest frame kept (not consumed) while paused
  unsigned long long consumedFrames = 0;
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
    // Test hook (with BLITZ_UPDATE_API): install a found update and restart into it, no clicks.
    if (static const bool autoUpdate = getenv("BLITZ_UPDATE_AUTOINSTALL") != nullptr; autoUpdate) {
      static bool started = false, restarted = false;
      if (!started && update::checkStatus().state == update::Release::Available) {
        started = true;
        update::installStart(update::checkStatus());
      }
      if (!restarted && update::installStatus().state == update::Install::Done) {
        restarted = true;
        restartGame();
      }
    }
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
      static const bool memWatch = getenv("BLITZ_MEMWATCH") != nullptr;  // leak hunting
      size_t before = memWatch ? processMemory() : 0;
      if (haveFrame && !empty) GXCallDisplayList(frame.data(), (u32)frame.size());
      if (haveFrame && !holding) frameshot::capture(consumedFrames);
      frameshot::poll();
      overlay::draw(fps, consumedFrames);
      // Once the FIFO is processed Aurora holds its own copies of everything it read from
      // guest memory: let the game build its next frame while this one is presented.
      aurora::gx::fifo::drain();
      if (memWatch) {
        size_t after = processMemory();
        if (after > before + (256u << 20)) {
          LOG_ERROR("frame %llu grew the process by %zu MB (%zu bytes of commands); saved build/leakframe.bin",
                    (unsigned long long)frames, (after - before) >> 20, frame.size());
          if (FILE* f = fopen("build/leakframe.bin", "wb")) {
            fwrite(frame.data(), 1, frame.size(), f);
            fclose(f);
          }
        }
      }
    }
    if (haveFrame && !paused) {
      gxbridge::frameConsumed();
      holding = false;
      fpsFrames++;
      consumedFrames++;
    } else if (haveFrame) {
      holding = true;
    }
    // Devices stop only once a frame is held: the game needs retraces to finish that frame.
    hw::g_paused.store(paused && holding);
    if (began) {
      // Present game frames on the VI retrace tick (the game has already been released to build the
      // next one): frames finished early or late in a period no longer shift the presentation, which
      // otherwise swung between ~9 and ~27 ms apart around the 60 Hz average. With vsync the display
      // paces instead.
      static const bool tickPacing = !getenv("BLITZ_NO_TICK_PACING");
      const bool shown = haveFrame && !paused;  // a new game frame (not the held one under the menu)
      if (tickPacing && shown && !settings::get().vsync) {
        const int64_t tick = hw::nextRetraceNs();
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
        if (tick > now && tick - now < 40'000'000) {
          // Sleep to ~1 ms before, then spin: sleeps alone are 1 ms coarse.
          if (tick - now > 1'500'000) std::this_thread::sleep_for(std::chrono::nanoseconds(tick - now - 1'200'000));
          while (std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count() < tick) {}
        }
      }
      auto t2 = clk::now();
      aurora_end_frame();
      auto t3 = clk::now();
      // BLITZ_PERF: frame pacing once per second, measured at presentation: game frames shown, the
      // longest gap between two and how many gaps exceeded 20 ms (a missed 60 Hz frame).
      static const bool pacing = getenv("BLITZ_PERF") != nullptr;
      if (pacing && shown) {
        static clk::time_point last{}, secStart = clk::now();
        static int n = 0, hitches = 0;
        static double worst = 0;
        auto now = clk::now();
        if (last.time_since_epoch().count()) {
          double gap = std::chrono::duration<double, std::milli>(now - last).count();
          worst = std::max(worst, gap);
          hitches += gap > 20.0;
        }
        last = now;
        n++;
        if (now - secStart >= std::chrono::seconds(1)) {
          LOG_INFO("pacing: %d frames/s, longest gap %.1f ms, %d over 20 ms%s", n, worst, hitches,
                   widescreen::inMatch() ? " (match)" : "");
          n = hitches = 0;
          worst = 0;
          secStart = now;
        }
      }
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
    // Match start/end in the log (test runs check that scripted input reached live play).
    static bool wasInMatch = false;
    if (bool m = widescreen::inMatch(); m != wasInMatch) {
      wasInMatch = m;
      LOG_INFO("match %s at frame %llu", m ? "on" : "off", (unsigned long long)frames);
    }
  }
  hw::g_paused.store(false);
  gxbridge::shutdown();
  closeAudio();
  aurora_shutdown();
  // The game's threads are still running: returning would destroy the runtime's globals (the guest
  // thread list ...) under them, and a thread that then looks itself up aborts the process.
  fflush(nullptr);
  std::_Exit(0);
}
