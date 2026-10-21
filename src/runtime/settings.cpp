#include "settings.h"

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace settings {

std::atomic<float> g_displayAspect{4.0f / 3.0f};

static Settings g_settings;
static std::string g_path;

Settings& get() { return g_settings; }
const std::string& path() { return g_path; }

static bool parseBool(const std::string& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

static void applySetting(const std::string& key, const std::string& v) {
  Settings& s = g_settings;
  if (key == "disc") s.disc = v;
  else if (key == "widescreen") s.widescreen = parseBool(v);
  else if (key == "hud") s.hud = v == "stretched" ? Hud2D::Stretched : Hud2D::Centered;
  else if (key == "stretch_menus") s.stretchMenus = parseBool(v);
  else if (key == "fullscreen") s.fullscreen = parseBool(v);
  else if (key == "vsync") s.vsync = parseBool(v);
  else if (key == "msaa") s.msaa = (uint32_t)atoi(v.c_str());
  else if (key == "anisotropy") s.anisotropy = (uint32_t)atoi(v.c_str());
  else if (key == "backend") s.backend = v;
  else if (key == "resolution_scale") s.resolutionScale = (float)atof(v.c_str());
  else if (key == "show_fps") s.showFps = parseBool(v);
  else if (key == "skip_logos") s.skipLogos = parseBool(v);
  else if (key == "cheats") s.cheats = strtoull(v.c_str(), nullptr, 16);
  else if (key == "texture_packs") s.texturePacks = parseBool(v);
  else if (key == "dump_textures") s.dumpTextures = parseBool(v);
  else if (key == "window_width") s.windowWidth = atoi(v.c_str());
  else if (key == "window_height") s.windowHeight = atoi(v.c_str());
  else if (key == "volume") s.volume = atoi(v.c_str());
  else if (key == "mute") s.mute = parseBool(v);
  else LOG_WARN("settings: unknown key '%s'", key.c_str());
}

static void sanitize() {
  Settings& s = g_settings;
  if (s.msaa != 1 && s.msaa != 2 && s.msaa != 4 && s.msaa != 8) s.msaa = 4;
  if (s.anisotropy < 1) s.anisotropy = 1;
  if (s.anisotropy > 16) s.anisotropy = 16;
  if (s.resolutionScale < 0.0f) s.resolutionScale = 0.0f;
  if (s.resolutionScale > 8.0f) s.resolutionScale = 8.0f;
  if (s.volume < 0) s.volume = 0;
  if (s.volume > 100) s.volume = 100;
}

void load(const std::string& p) {
  g_path = p;
  std::ifstream in(p);
  if (!in) {
    save();
  } else {
    std::string line;
    while (std::getline(in, line)) {
      size_t hash = line.find_first_of("#;");
      if (hash != std::string::npos) line.resize(hash);
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      auto trim = [](std::string t) {
        size_t a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
      };
      std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      if (!key.empty()) applySetting(key, value);
    }
  }
  // Environment overrides (testing): BLITZ_SET="key=value;key=value"
  if (const char* env = getenv("BLITZ_SET")) {
    std::stringstream ss(env);
    std::string item;
    while (std::getline(ss, item, ';')) {
      size_t eq = item.find('=');
      if (eq != std::string::npos) applySetting(item.substr(0, eq), item.substr(eq + 1));
    }
  }
  sanitize();
  // Rewrite so options added by newer versions show up in the file (BLITZ_SET values too,
  // which is why tests use it sparingly).
  if (!getenv("BLITZ_SET")) save();
}

void save() {
  if (g_path.empty()) return;
  const Settings& s = g_settings;
  std::ofstream out(g_path, std::ios::trunc);
  if (!out) {
    LOG_WARN("settings: cannot write %s", g_path.c_str());
    return;
  }
  out << "# NFL Blitz 2003 PC settings\n"
      << "\n[game]\n"
      << "disc = " << s.disc << "   # your NFL Blitz 2003 (USA) disc image\n"
      << "skip_logos = " << (s.skipLogos ? "on" : "off") << "   # skip the logo movies and the football intro at boot\n"
      << "cheats = " << std::hex << s.cheats << std::dec << "   # F1 > Cheats (bit = the game's cheat id)\n"
      << "\n[display]\n"
      << "widescreen = " << (s.widescreen ? "on" : "off") << "   # Hor+ 3D at the window's aspect ratio\n"
      << "hud = " << (s.hud == Hud2D::Stretched ? "stretched" : "centered") << "   # in-match HUD/2D: centered | stretched\n"
      << "stretch_menus = " << (s.stretchMenus ? "on" : "off") << "   # menus fill the 16:9 screen\n"
      << "fullscreen = " << (s.fullscreen ? "on" : "off") << "   # Alt+Enter / F11 toggles\n"
      << "vsync = " << (s.vsync ? "on" : "off") << "\n"
      << "msaa = " << s.msaa << "   # 1 2 4 8\n"
      << "anisotropy = " << s.anisotropy << "   # 1..16\n"
      << "resolution_scale = " << s.resolutionScale << "   # 0 = window size; N = N x 480 lines (3 = 1440p, 4.5 = 2160p)\n"
      << "backend = " << s.backend << "   # auto | d3d11 | d3d12 | vulkan\n"
      << "show_fps = " << (s.showFps ? "on" : "off") << "\n"
      << "texture_packs = " << (s.texturePacks ? "on" : "off") << "   # Dolphin-format packs in Textures/GO3E5D\n"
      << "dump_textures = " << (s.dumpTextures ? "on" : "off") << "   # write the game's textures (pack making)\n"
      << "window_width = " << s.windowWidth << "   # 0 = fit to the display\n"
      << "window_height = " << s.windowHeight << "\n"
      << "\n[audio]\n"
      << "volume = " << s.volume << "   # percent\n"
      << "mute = " << (s.mute ? "on" : "off") << "\n";
}

}  // namespace settings
