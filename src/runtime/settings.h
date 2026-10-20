// User settings (settings.ini in the user data directory), shared by the runtime and
// the window app. Environment variables override for testing.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace settings {

enum class Hud2D { Centered, Stretched };

struct Settings {
  // Display
  bool widescreen = true;      // Hor+ 3D at the window's aspect ratio
  Hud2D hud = Hud2D::Centered;  // 2D (orthographic) screens: 4:3 centered or stretched
  bool fullscreen = false;
  bool vsync = false;
  uint32_t msaa = 4;            // 1, 2, 4, 8
  uint32_t anisotropy = 16;     // 1..16
  std::string backend = "auto";  // auto | d3d11 | d3d12 | vulkan
  int windowWidth = 0, windowHeight = 0;  // 0: fit to the display
  // Audio
  int volume = 100;  // percent
  bool mute = false;
};

Settings& get();
void load(const std::string& path);  // creates the file with defaults when missing
void save();
const std::string& path();

// Aspect ratio (width / height) of the presented image, updated by the window app.
extern std::atomic<float> g_displayAspect;

}  // namespace settings
