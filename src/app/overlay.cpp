#include "overlay.h"

#include "cheats.h"
#include "settings.h"
#include "update_check.h"

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
  ImGui::TextDisabled("Esc: this menu   Alt+Enter / F11: fullscreen");
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
enum Section { kDisplay, kAudio, kControls, kCheats, kDisc, kUpdate, kAbout, kSectionCount };
const char* kSectionNames[kSectionCount] = {"Display", "Audio", "Controls", "Cheats", "Disc", "Update", "About"};
// Two panes: the section list and the section's options. Choosing a section moves the highlight
// into its options; B (Backspace) moves it back to the list. Only the menu key closes the menu.
enum class Focus { None, Sections, Content };
Focus g_focusTo = Focus::Sections;  // a focus move requested for this frame
bool g_popupWasOpen = false;        // last frame: B then belongs to the popup (ImGui closes it)

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

// Disc: the disc image the game starts from, and choosing another one (used from the next start).
std::string g_discMessage;
bool g_discChanged = false;

void drawDisc(settings::Settings& st) {
  ImGui::TextDisabled("The game reads your NFL Blitz 2003 (USA) disc image every time it starts:");
  ImGui::TextWrapped("%s", st.disc.empty() ? "(none chosen yet)" : st.disc.c_str());
  ImGui::Spacing();
  if (ImGui::Button("Choose a different disc image...") && g_actions.chooseDisc) {
    std::string message;
    if (g_actions.chooseDisc(&message)) {
      g_discChanged = true;
      g_discMessage.clear();
    } else if (!message.empty()) {
      g_discMessage = message;
    }
  }
  if (!g_discMessage.empty()) ImGui::TextColored(kRed, "%s", g_discMessage.c_str());
  if (g_discChanged) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Saved. The game starts from this disc after a restart.");
    if (ImGui::Button("Restart now") && g_actions.restart) g_actions.restart();
  }
}

// Update: this version, the latest GitHub release, and installing it (update_check.h).
void drawUpdate(settings::Settings& st, bool* changed) {
  update::Release r = update::checkStatus();
  update::Install in = update::installStatus();
  ImGui::Text("This version: %s", update::version());
  ImGui::Spacing();
  switch (r.state) {
  case update::Release::Idle: ImGui::TextDisabled("Not checked yet."); break;
  case update::Release::Checking: ImGui::Text("Checking GitHub for a new release..."); break;
  case update::Release::UpToDate:
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Up to date (latest release: %s).", r.latest.c_str());
    break;
  case update::Release::NoReleases: ImGui::TextDisabled("No release has been published yet."); break;
  case update::Release::Failed: ImGui::TextColored(kRed, "Could not check: %s", r.error.c_str()); break;
  case update::Release::Available: ImGui::TextColored(kGold, "%s is available.", r.latest.c_str()); break;
  }
  ImGui::Spacing();
  const bool busy = in.state == update::Install::Downloading || in.state == update::Install::Verifying ||
                    in.state == update::Install::Extracting || in.state == update::Install::Installing;
  if (r.state == update::Release::Available && in.state != update::Install::Done) {
    std::string why;
    if (update::canInstall(r, why)) {
      ImGui::BeginDisabled(busy);
      if (ImGui::Button(("Download and install " + r.latest).c_str())) update::installStart(r);
      ImGui::EndDisabled();
    } else {
      ImGui::TextWrapped("It cannot be installed here (%s).", why.c_str());
      if (ImGui::Button("Open the release page")) update::openPage(r.page);
    }
  }
  if (busy) {
    const char* step = in.state == update::Install::Downloading  ? "Downloading"
                       : in.state == update::Install::Verifying  ? "Checking the download"
                       : in.state == update::Install::Extracting ? "Unpacking"
                                                                 : "Installing";
    ImGui::Text("%s...", step);
    ImGui::ProgressBar(in.state == update::Install::Downloading ? in.fraction : 1.0f, ImVec2(-1, 0));
  }
  if (in.state == update::Install::Failed) ImGui::TextColored(kRed, "The update failed: %s", in.error.c_str());
  if (in.state == update::Install::Done) {
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "%s is installed. Restart to play it.", r.latest.c_str());
    if (ImGui::Button("Restart now") && g_actions.restart) g_actions.restart();
  }
  if (!busy && r.state != update::Release::Checking && in.state != update::Install::Done) {
    if (ImGui::Button("Check now")) update::checkStart();
  }
  ImGui::Spacing();
  ImGui::Separator();
  *changed |= ImGui::Checkbox("Check for updates when the game starts", &st.updateCheck);
  if (!r.notes.empty() && r.state == update::Release::Available) {
    ImGui::Spacing();
    ImGui::TextDisabled("Release notes");
    ImGui::TextWrapped("%s", r.notes.c_str());
  }
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
  // Menu-level actions: L/R switch sections; B (Backspace) goes back to the section list (a popup
  // or an active widget takes it first). The menu itself closes only with the menu key.
  if (g_padPressed & PAD_TRIGGER_L) g_section = (g_section + kSectionCount - 1) % kSectionCount;
  if (g_padPressed & PAD_TRIGGER_R) g_section = (g_section + 1) % kSectionCount;
  const bool back = (g_padPressed & PAD_BUTTON_B) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
  if (back && !g_popupWasOpen && !ImGui::IsAnyItemActive() && g_capture < 0) g_focusTo = Focus::Sections;
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
    if (sel && g_focusTo == Focus::Sections) {
      ImGui::SetKeyboardFocusHere();
      g_focusTo = Focus::None;
    }
    if (ImGui::Selectable(kSectionNames[i], sel, 0, ImVec2(0, ImGui::GetTextLineHeight() * 1.8f))) {
      g_section = i;
      g_focusTo = Focus::Content;  // into the section's options
    }
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
  if (g_focusTo == Focus::Content) {
    ImGui::SetKeyboardFocusHere();  // the section's first option
    g_focusTo = Focus::None;
  }
  switch (g_section) {
  case kDisplay: changed |= drawDisplay(st); break;
  case kAudio: changed |= drawAudio(st); break;
  case kControls: drawControls(); break;
  case kCheats: changed |= drawCheats(st); break;
  case kDisc: drawDisc(st); break;
  case kUpdate: drawUpdate(st, &changed); break;
  case kAbout: drawAbout(st); break;
  }
  ImGui::PopItemWidth();
  ImGui::EndChild();
  // Footer: controller hints and the resume/quit buttons.
  ImGui::Spacing();
  hint("A", ImVec4(0.3f, 0.85f, 0.4f, 1), "select");
  hint("B", kRed, "back");
  hint("L / R", kBlue, "section");
  hint("Esc / Z+Start", kGold, "close");
  // Update (highlighted when a newer release is out), Resume, Quit game (asks first).
  const bool updateReady = update::checkStatus().state == update::Release::Available;
  const char* updateLabel = updateReady ? "Update available" : "Update";
  const ImGuiStyle& style = ImGui::GetStyle();
  auto width = [&](const char* s) { return ImGui::CalcTextSize(s).x + style.FramePadding.x * 2; };
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width(updateLabel) - width("Resume") - width("Quit game") -
                       style.ItemSpacing.x * 2);
  if (updateReady) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.08f, 1.0f));
  if (ImGui::Button(updateLabel)) {
    g_section = kUpdate;
    g_focusTo = Focus::Content;
  }
  if (updateReady) ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button("Resume")) g_open = false;
  ImGui::SameLine();
  if (ImGui::Button("Quit game")) ImGui::OpenPopup("Quit?");
  if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::Text("Quit NFL Blitz 2003?");
    ImGui::Spacing();
    if (ImGui::Button("Quit", ImVec2(120, 0)) && g_actions.quit) g_actions.quit();
    ImGui::SameLine();
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  g_popupWasOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
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
    g_focusTo = Focus::Sections;
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
bool wantsEscape() { return g_open && (g_capture >= 0 || g_popupWasOpen); }
void toggle() {
  g_open = !g_open;
  g_focusTo = Focus::Sections;  // a menu opens on its section list
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
