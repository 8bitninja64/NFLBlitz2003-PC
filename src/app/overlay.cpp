#include "overlay.h"

#include "cheats.h"
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

// --- look ---------------------------------------------------------------------------
// Blitz colours: dark navy panels, red accent, steel blue highlights.
const ImVec4 kRed(0.86f, 0.12f, 0.16f, 1.0f);
const ImVec4 kBlue(0.16f, 0.42f, 0.86f, 1.0f);
const ImVec4 kGold(1.0f, 0.80f, 0.25f, 1.0f);
ImFont* g_fontBody = nullptr;
ImFont* g_fontTitle = nullptr;
constexpr float kFontPx = 40.0f;  // rasterized large, scaled down to the window (stays sharp)

void applyStyle() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10;
  s.ChildRounding = 8;
  s.FrameRounding = 6;
  s.GrabRounding = 6;
  s.PopupRounding = 6;
  s.WindowBorderSize = 0;
  s.FrameBorderSize = 0;
  s.WindowPadding = ImVec2(18, 16);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 9);
  s.ScrollbarSize = 14;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.06f, 0.12f, 0.96f);
  c[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.10f, 0.19f, 1.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.09f, 0.17f, 0.98f);
  c[ImGuiCol_Text] = ImVec4(0.94f, 0.95f, 0.98f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.60f, 0.72f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.17f, 0.30f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.25f, 0.44f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.30f, 0.52f, 1.0f);
  c[ImGuiCol_CheckMark] = kGold;
  c[ImGuiCol_SliderGrab] = kRed;
  c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.14f, 0.20f, 0.36f, 1.0f);
  c[ImGuiCol_ButtonHovered] = kBlue;
  c[ImGuiCol_ButtonActive] = kRed;
  c[ImGuiCol_Header] = ImVec4(0.16f, 0.24f, 0.44f, 1.0f);
  c[ImGuiCol_HeaderHovered] = kBlue;
  c[ImGuiCol_HeaderActive] = kRed;
  c[ImGuiCol_Separator] = ImVec4(0.25f, 0.32f, 0.50f, 1.0f);
  c[ImGuiCol_NavHighlight] = kGold;
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// A system UI font when there is one (nothing to ship): Segoe UI on Windows, DejaVu/Noto on Linux.
void loadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  const char* body[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"};
  const char* title[] = {"C:/Windows/Fonts/seguibl.ttf", "C:/Windows/Fonts/segoeuib.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                         "/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"};
  auto load = [&](const char* const* paths, size_t n, float px) -> ImFont* {
    for (size_t i = 0; i < n; i++) {
      if (FILE* f = fopen(paths[i], "rb")) {
        fclose(f);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(paths[i], px)) return font;
      }
    }
    return nullptr;
  };
  g_fontBody = load(body, sizeof(body) / sizeof(body[0]), kFontPx);
  if (!g_fontBody) {
    ImFontConfig cfg;
    cfg.SizePixels = 13;
    g_fontBody = io.Fonts->AddFontDefault(&cfg);
  }
  g_fontTitle = load(title, sizeof(title) / sizeof(title[0]), kFontPx * 1.4f);
  if (!g_fontTitle) g_fontTitle = g_fontBody;
  io.FontDefault = g_fontBody;
}

// --- controller ----------------------------------------------------------------------
// Pad buttons arrive as GameCube PAD bits; the menu is driven as ImGui keyboard navigation so GC
// adapters and any SDL gamepad behave the same.
unsigned short g_padHeld = 0, g_padPrev = 0;
bool g_holdInput = false;  // after the menu toggled: pads stay away from the game until released
int g_section = 0;  // the selected menu section
enum Section { kDisplay, kAudio, kControls, kCheats, kAbout, kSectionCount };
const char* kSectionNames[kSectionCount] = {"Display", "Audio", "Controls", "Cheats", "About"};

void feedNav() {
  ImGuiIO& io = ImGui::GetIO();
  unsigned short now = g_padHeld, changed = now ^ g_padPrev;
  auto key = [&](unsigned short bit, ImGuiKey k) {
    if (changed & bit) io.AddKeyEvent(k, (now & bit) != 0);
  };
  key(PAD_BUTTON_UP, ImGuiKey_UpArrow);
  key(PAD_BUTTON_DOWN, ImGuiKey_DownArrow);
  key(PAD_BUTTON_LEFT, ImGuiKey_LeftArrow);
  key(PAD_BUTTON_RIGHT, ImGuiKey_RightArrow);
  key(PAD_BUTTON_A, ImGuiKey_Space);   // activate / toggle
  key(PAD_BUTTON_B, ImGuiKey_Escape);  // cancel
  g_padPrev = now;
}

// Edge-triggered pad buttons for this frame (menu-level actions: sections, closing).
unsigned short g_padPressed = 0;

// --- sections ------------------------------------------------------------------------
bool drawDisplay(settings::Settings& st) {
  bool changed = false;
  {
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
      if (ImGui::Checkbox("Stretch menus to fill the screen", &st.stretchMenus)) changed = true;
      int hud = st.hud == settings::Hud2D::Stretched ? 1 : 0;
      if (ImGui::Combo("In-game HUD", &hud, "4:3 centered\0Stretched to fill\0")) {
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
  }
  return changed;
}

bool drawAudio(settings::Settings& st) {
  bool changed = false;
  if (ImGui::SliderInt("Volume", &st.volume, 0, 100, "%d%%")) {
    if (g_actions.setVolume) g_actions.setVolume(st.mute ? 0.0f : st.volume / 100.0f);
    changed = true;
  }
  if (ImGui::Checkbox("Mute", &st.mute)) {
    if (g_actions.setVolume) g_actions.setVolume(st.mute ? 0.0f : st.volume / 100.0f);
    changed = true;
  }
  return changed;
}

bool drawCheats(settings::Settings& st) {
  bool changed = false;
  ImGui::TextWrapped("The game's own cheat codes, without entering them on the matchup screen. "
                     "Gold ones work right away; the rest from the next game.");
  ImGui::Spacing();
  if (ImGui::BeginTable("cheats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (int i = 0; i < cheats::kCount; i++) {
      const cheats::Cheat& ch = cheats::kList[i];
      uint64_t b = 1ull << ch.id;
      bool on = (st.cheats & b) != 0;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (ch.live) ImGui::PushStyleColor(ImGuiCol_Text, kGold);
      bool clicked = ImGui::Checkbox(ch.name, &on);
      if (ch.live) ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("%s", ch.info);
      if (clicked) {
        uint64_t before = st.cheats;
        st.cheats = on ? (st.cheats | b) : (st.cheats & ~b);
        if (on && ch.excludes != 0xFF) st.cheats &= ~(1ull << ch.excludes);
        cheats::applyNow(st.cheats, before);  // the game is paused while the menu is open
        changed = true;
      }
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("All off")) {
    cheats::applyNow(0, st.cheats);
    st.cheats = 0;
    changed = true;
  }
  return changed;
}

void drawAbout(settings::Settings& st) {
  ImGui::TextWrapped("NFL Blitz 2003 PC port: the GameCube executable from your own disc, statically "
                     "recompiled to native code, rendered with Aurora.");
  ImGui::Spacing();
  ImGui::TextWrapped("Disc: %s", st.disc.c_str());
  ImGui::TextWrapped("Settings: %s", settings::path().c_str());
}

void hint(const char* button, ImVec4 color, const char* action) {
  ImGui::TextColored(color, "%s", button);
  ImGui::SameLine(0, 4);
  ImGui::TextDisabled("%s", action);
  ImGui::SameLine(0, 22);
}

void drawSettings() {
  settings::Settings& st = settings::get();
  ImGuiIO& io = ImGui::GetIO();
  // Menu-level pad actions: L/R switch sections, B closes the menu when nothing else wants it.
  if (g_padPressed & PAD_TRIGGER_L) g_section = (g_section + kSectionCount - 1) % kSectionCount;
  if (g_padPressed & PAD_TRIGGER_R) g_section = (g_section + 1) % kSectionCount;
  if ((g_padPressed & PAD_BUTTON_B) && !ImGui::IsAnyItemActive() &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && g_capture < 0) {
    g_open = false;
    g_holdInput = true;
    return;
  }
  // BLITZ_MENU_TAB=<section> selects that section once (screenshots).
  if (static const char* startTab = getenv("BLITZ_MENU_TAB"); startTab) {
    for (int i = 0; i < kSectionCount; i++)
      if (!strcmp(startTab, kSectionNames[i])) g_section = i;
    startTab = nullptr;
  }
  ImVec2 size(io.DisplaySize.x * 0.72f, io.DisplaySize.y * 0.80f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  if (!ImGui::Begin("##settings", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  // Header: title with a red/blue stripe.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  float stripe = ImGui::GetFontSize() / 3.0f;
  dl->AddRectFilled(wp, ImVec2(wp.x + size.x, wp.y + stripe), ImGui::GetColorU32(kRed), 10, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilled(ImVec2(wp.x, wp.y + stripe), ImVec2(wp.x + size.x, wp.y + stripe * 1.6f), ImGui::GetColorU32(kBlue));
  ImGui::Dummy(ImVec2(0, stripe));
  ImGui::PushFont(g_fontTitle);
  ImGui::TextColored(ImVec4(1, 1, 1, 1), "NFL BLITZ 2003");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight() * 0.35f);
  ImGui::TextDisabled("SETTINGS  -  game paused");
  ImGui::Spacing();

  float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
  float sideW = size.x * 0.22f;
  bool changed = false;
  // Sections.
  ImGui::BeginChild("##sections", ImVec2(sideW, -footer), ImGuiChildFlags_None);
  ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.1f, 0.5f));
  for (int i = 0; i < kSectionCount; i++) {
    bool sel = g_section == i;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Header, kRed);
    if (ImGui::Selectable(kSectionNames[i], sel, 0, ImVec2(0, ImGui::GetTextLineHeight() * 1.8f))) g_section = i;
    if (sel) ImGui::PopStyleColor();
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::SameLine();
  // Content.
  ImGui::BeginChild("##content", ImVec2(0, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PushFont(g_fontTitle);
  ImGui::TextColored(kGold, "%s", kSectionNames[g_section]);
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
  switch (g_section) {
  case kDisplay: changed |= drawDisplay(st); break;
  case kAudio: changed |= drawAudio(st); break;
  case kControls: drawControls(); break;
  case kCheats: changed |= drawCheats(st); break;
  case kAbout: drawAbout(st); break;
  }
  ImGui::PopItemWidth();
  ImGui::EndChild();
  // Footer: controller hints and the resume/quit buttons.
  ImGui::Spacing();
  hint("A", ImVec4(0.3f, 0.85f, 0.4f, 1), "select");
  hint("B", kRed, "back");
  hint("L / R", kBlue, "section");
  hint("Z+Start / F1", kGold, "close");
  float bw = ImGui::CalcTextSize("Quit game").x + ImGui::GetStyle().FramePadding.x * 2;
  float rw = ImGui::CalcTextSize("Resume").x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw - rw - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::Button("Resume")) g_open = false;
  ImGui::SameLine();
  if (ImGui::Button("Quit game") && g_actions.quit) g_actions.quit();
  ImGui::End();
  if (changed) settings::save();
}

}  // namespace

void init(const Actions& actions) {
  g_actions = actions;
  loadFonts();
  applyStyle();
  ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

void padInput(const unsigned short buttons[4], const signed char stickX[4], const signed char stickY[4]) {
  unsigned short held = 0;
  for (int i = 0; i < 4; i++) {
    unsigned short b = buttons[i];
    // The main stick navigates like the D-pad.
    if (stickY[i] > 50) b |= PAD_BUTTON_UP;
    if (stickY[i] < -50) b |= PAD_BUTTON_DOWN;
    if (stickX[i] < -50) b |= PAD_BUTTON_LEFT;
    if (stickX[i] > 50) b |= PAD_BUTTON_RIGHT;
    held |= b;
  }
  static unsigned short last = 0;
  unsigned short pressed = held & ~last;
  last = held;
  const unsigned short combo = PAD_TRIGGER_Z | PAD_BUTTON_START;
  if (g_holdInput && !held) g_holdInput = false;
  if ((held & combo) == combo && (pressed & combo)) {
    g_open = !g_open;
    g_capture = -1;
    g_padHeld = g_padPrev = 0;
    g_holdInput = true;  // the game gets the pads back once everything is released
    return;
  }
  g_padPressed |= pressed;
  if (g_open) g_padHeld = held;
}

bool isOpen() { return g_open; }
bool ownsPads() { return g_open || g_holdInput; }
bool isCapturingKey() { return g_open && g_capture >= 0; }
void toggle() {
  g_open = !g_open;
  g_holdInput = true;
}
void close() {
  g_open = false;
  g_capture = -1;
}

void draw(float presentedFps, unsigned long long frame) {
  // Scale the UI with the window (the default font is tiny on large or high-DPI windows).
  ImGuiIO& io = ImGui::GetIO();
  // The fonts are rasterized at kFontPx: scale them to ~18 px per 540 lines of window.
  float px = 18.0f * io.DisplaySize.y / 540.0f;
  px = px < 15.0f ? 15.0f : px > 72.0f ? 72.0f : px;
  io.FontGlobalScale = px / (g_fontBody && g_fontBody->FontSize > 20 ? kFontPx : 13.0f);
  if (settings::get().showFps || getenv("BLITZ_FRAMENO")) drawFps(presentedFps, frame);
  if (g_open) {
    feedNav();
    drawSettings();
  }
  g_padPressed = 0;
}

}  // namespace overlay
