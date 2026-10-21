#include "overlay.h"

#include "settings.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <dolphin/pad.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace overlay {

namespace {

Actions g_actions{};
bool g_open = false;

struct ResolutionChoice {
  const char* label;
  float scale;
};
constexpr ResolutionChoice kResolutions[] = {
    {"Match window", 0.0f}, {"960p (2x)", 2.0f},  {"1440p (3x)", 3.0f},
    {"1920p (4x)", 4.0f},   {"2160p (4.5x)", 4.5f}, {"2880p (6x)", 6.0f},
};

int resolutionIndex(float scale) {
  for (int i = 0; i < (int)(sizeof(kResolutions) / sizeof(kResolutions[0])); i++)
    if (std::fabs(kResolutions[i].scale - scale) < 0.01f) return i;
  return 0;
}

void drawFps(float fps, unsigned long long frame) {
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  static const bool frameNo = getenv("BLITZ_FRAMENO") != nullptr;  // debugging: match screenshots to dumps
  if (ImGui::Begin("##fps", nullptr, flags)) {
    if (frameNo) ImGui::Text("%.0f fps  frame %llu", fps, frame);
    else ImGui::Text("%.0f fps", fps);
  }
  ImGui::End();
}

// --- controls ---------------------------------------------------------------------
const char* buttonLabel(PADButton b) {
  switch (b) {
  case PAD_BUTTON_LEFT: return "D-pad left";
  case PAD_BUTTON_RIGHT: return "D-pad right";
  case PAD_BUTTON_DOWN: return "D-pad down";
  case PAD_BUTTON_UP: return "D-pad up";
  case PAD_TRIGGER_Z: return "Z";
  case PAD_TRIGGER_R: return "R (digital)";
  case PAD_TRIGGER_L: return "L (digital)";
  case PAD_BUTTON_A: return "A";
  case PAD_BUTTON_B: return "B";
  case PAD_BUTTON_X: return "X";
  case PAD_BUTTON_Y: return "Y";
  case PAD_BUTTON_START: return "Start";
  default: return "?";
  }
}

const char* axisLabel(PADAxis a) {
  static const char* names[PAD_AXIS_COUNT] = {"Stick right", "Stick left", "Stick up", "Stick down",
                                              "C-stick right", "C-stick left", "C-stick up", "C-stick down",
                                              "L (analog)", "R (analog)"};
  return a < PAD_AXIS_COUNT ? names[a] : "?";
}

// Key capture: which binding waits for a key (-1 none; buttons 0..11, axes 100..109).
int g_capture = -1;
bool g_captureArmed = false;  // all keys released since the capture started

void pollCapture() {
  if (g_capture < 0) return;
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  bool any = false;
  int pressed = -1;
  for (int sc = 1; sc < count; sc++) {
    if (!keys[sc]) continue;
    any = true;
    if (pressed < 0) pressed = sc;
  }
  if (!g_captureArmed) {
    g_captureArmed = !any;  // wait until the click/Enter that started it is released
    return;
  }
  if (pressed < 0) return;
  if (pressed != SDL_SCANCODE_ESCAPE) {
    u32 n = 0;
    if (g_capture < 100) {
      PADKeyButtonBinding* bindings = PADGetKeyButtonBindings(0, &n);
      if (bindings && (u32)g_capture < n) PADSetKeyButtonBinding(0, {pressed, bindings[g_capture].padButton});
    } else {
      PADKeyAxisBinding* axes = PADGetKeyAxisBindings(0, &n);
      if (axes && (u32)(g_capture - 100) < n) {
        PADKeyAxisBinding b = axes[g_capture - 100];
        b.scancode = pressed;
        PADSetKeyAxisBinding(0, b);
      }
    }
    PADSerializeMappings();
  }
  g_capture = -1;
}

void bindingRow(int id, const char* label, s32 scancode) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  if (g_capture == id) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "press a key (Esc cancels)");
  else ImGui::TextUnformatted(scancode > 0 ? SDL_GetScancodeName((SDL_Scancode)scancode) : "-");
  ImGui::TableSetColumnIndex(2);
  ImGui::PushID(id);
  if (ImGui::SmallButton("Change")) {
    g_capture = id;
    g_captureArmed = false;
  }
  ImGui::PopID();
}

void drawControls() {
  pollCapture();
  u32 pads = PADCount();
  if (pads == 0) ImGui::TextDisabled("No gamepad connected (GameCube adapters and SDL gamepads are supported).");
  for (u32 i = 0; i < pads; i++) ImGui::Text("Gamepad %u: %s", i + 1, PADGetNameForControllerIndex(i));
  ImGui::Separator();
  ImGui::TextUnformatted("Keyboard (player 1):");
  u32 nb = 0, na = 0;
  PADKeyButtonBinding* buttons = PADGetKeyButtonBindings(0, &nb);
  PADKeyAxisBinding* axes = PADGetKeyAxisBindings(0, &na);
  float h = ImGui::GetTextLineHeightWithSpacing() * 12;
  if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, h))) {
    for (u32 i = 0; axes && i < na; i++) bindingRow(100 + (int)i, axisLabel(axes[i].padAxis), axes[i].scancode);
    for (u32 i = 0; buttons && i < nb; i++) bindingRow((int)i, buttonLabel(buttons[i].padButton), buttons[i].scancode);
    ImGui::EndTable();
  }
  if (ImGui::Button("Reset keyboard to defaults")) {
    PADRestoreDefaultKeyBindings(0);
    PADSerializeMappings();
  }
  ImGui::TextDisabled("F1: this menu   Alt+Enter / F11: fullscreen");
}

void drawSettings() {
  settings::Settings& st = settings::get();
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460 * io.FontGlobalScale, 0), ImGuiCond_Appearing);
  bool open = true;
  if (!ImGui::Begin("NFL Blitz 2003 - Settings (game paused)", &open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  bool changed = false;
  if (ImGui::BeginTabBar("tabs")) {
    if (ImGui::BeginTabItem("Display")) {
      bool fs = st.fullscreen;
      if (ImGui::Checkbox("Fullscreen (Alt+Enter / F11)", &fs)) {
        st.fullscreen = fs;
        if (g_actions.setFullscreen) g_actions.setFullscreen(fs);
        changed = true;
      }
      if (ImGui::Checkbox("Widescreen (Hor+)", &st.widescreen)) {
        if (g_actions.applyDisplay) g_actions.applyDisplay();
        changed = true;
      }
      ImGui::BeginDisabled(!st.widescreen);
      int hud = st.hud == settings::Hud2D::Stretched ? 1 : 0;
      if (ImGui::Combo("2D screens / HUD", &hud, "4:3 centered\0Stretched to fill\0")) {
        st.hud = hud ? settings::Hud2D::Stretched : settings::Hud2D::Centered;
        changed = true;
      }
      ImGui::EndDisabled();
      int res = resolutionIndex(st.resolutionScale);
      if (ImGui::BeginCombo("Internal resolution", kResolutions[res].label)) {
        for (int i = 0; i < (int)(sizeof(kResolutions) / sizeof(kResolutions[0])); i++) {
          if (ImGui::Selectable(kResolutions[i].label, i == res)) {
            st.resolutionScale = kResolutions[i].scale;
            if (g_actions.applyDisplay) g_actions.applyDisplay();
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      changed |= ImGui::Checkbox("Show FPS counter", &st.showFps);
      changed |= ImGui::Checkbox("Skip intro movies at start", &st.skipLogos);
      ImGui::Separator();
      ImGui::TextDisabled("Applied on the next start:");
      int msaa = st.msaa == 8 ? 3 : st.msaa == 4 ? 2 : st.msaa == 2 ? 1 : 0;
      if (ImGui::Combo("Anti-aliasing (MSAA)", &msaa, "Off\0" "2x\0" "4x\0" "8x\0")) {
        st.msaa = 1u << msaa;
        changed = true;
      }
      int aniso = (int)st.anisotropy;
      if (ImGui::SliderInt("Anisotropic filtering", &aniso, 1, 16)) {
        st.anisotropy = (uint32_t)aniso;
        changed = true;
      }
      changed |= ImGui::Checkbox("VSync", &st.vsync);
      changed |= ImGui::Checkbox("Load texture packs (Textures/GO3E5D)", &st.texturePacks);
      changed |= ImGui::Checkbox("Dump textures (for pack makers)", &st.dumpTextures);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Audio")) {
      if (ImGui::SliderInt("Volume", &st.volume, 0, 100, "%d%%")) {
        if (g_actions.setVolume) g_actions.setVolume(st.mute ? 0.0f : st.volume / 100.0f);
        changed = true;
      }
      if (ImGui::Checkbox("Mute", &st.mute)) {
        if (g_actions.setVolume) g_actions.setVolume(st.mute ? 0.0f : st.volume / 100.0f);
        changed = true;
      }
      ImGui::EndTabItem();
    }
    // BLITZ_MENU_TAB=Controls selects that tab once (screenshots).
    static const char* startTab = getenv("BLITZ_MENU_TAB");
    auto tabFlags = [](const char* name) {
      if (!startTab || strcmp(startTab, name)) return ImGuiTabItemFlags_None;
      startTab = nullptr;
      return ImGuiTabItemFlags_SetSelected;
    };
    if (ImGui::BeginTabItem("Controls", nullptr, tabFlags("Controls"))) {
      drawControls();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("About")) {
      ImGui::TextWrapped("NFL Blitz 2003 PC port: the GameCube executable from your own disc, statically "
                         "recompiled to native code, rendered with Aurora.");
      ImGui::Spacing();
      ImGui::TextWrapped("Disc: %s", st.disc.c_str());
      ImGui::TextWrapped("Settings: %s", settings::path().c_str());
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::Separator();
  if (ImGui::Button("Resume (F1)")) open = false;
  ImGui::SameLine();
  if (ImGui::Button("Quit game") && g_actions.quit) g_actions.quit();
  ImGui::End();
  if (changed) settings::save();
  if (!open) g_open = false;
}

}  // namespace

void init(const Actions& actions) { g_actions = actions; }
bool isOpen() { return g_open; }
bool isCapturingKey() { return g_open && g_capture >= 0; }
void toggle() { g_open = !g_open; }
void close() {
  g_open = false;
  g_capture = -1;
}

void draw(float presentedFps, unsigned long long frame) {
  // Scale the UI with the window (the default font is tiny on large or high-DPI windows).
  ImGuiIO& io = ImGui::GetIO();
  float scale = io.DisplaySize.y / 540.0f;
  io.FontGlobalScale = scale < 1.0f ? 1.0f : scale > 4.0f ? 4.0f : scale;
  if (settings::get().showFps || getenv("BLITZ_FRAMENO")) drawFps(presentedFps, frame);
  if (g_open) drawSettings();
}

}  // namespace overlay
