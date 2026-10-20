#include "overlay.h"

#include "settings.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

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

void drawFps(float fps) {
  ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.45f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
  if (ImGui::Begin("##fps", nullptr, flags)) ImGui::Text("%.0f fps", fps);
  ImGui::End();
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
    if (ImGui::BeginTabItem("Controls")) {
      ImGui::TextWrapped("Gamepads and GameCube controller adapters work automatically. Keyboard (player 1):");
      ImGui::Spacing();
      if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        const char* rows[][2] = {{"Control stick", "W A S D"}, {"D-pad", "Arrow keys"}, {"A", "K"}, {"B", "J"},
                                 {"X", "L"},  {"Y", "I"}, {"Z", "Space"}, {"L / R", "Q / E"},
                                 {"Start", "Enter"}, {"C-stick", "Numpad 8 4 2 6"}};
        for (auto& r : rows) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(r[0]);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextUnformatted(r[1]);
        }
        ImGui::EndTable();
      }
      ImGui::Spacing();
      ImGui::TextDisabled("F1: this menu   Alt+Enter / F11: fullscreen");
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
void toggle() { g_open = !g_open; }
void close() { g_open = false; }

void draw(float presentedFps) {
  // Scale the UI with the window (the default font is tiny on large or high-DPI windows).
  ImGuiIO& io = ImGui::GetIO();
  float scale = io.DisplaySize.y / 540.0f;
  io.FontGlobalScale = scale < 1.0f ? 1.0f : scale > 4.0f ? 4.0f : scale;
  if (settings::get().showFps) drawFps(presentedFps);
  if (g_open) drawSettings();
}

}  // namespace overlay
