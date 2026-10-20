// F1 settings overlay (Dear ImGui via Aurora) and the FPS counter.
#pragma once

namespace overlay {

struct Actions {
  void (*setFullscreen)(bool on);
  void (*applyDisplay)();      // widescreen / HUD / resolution changed
  void (*setVolume)(float gain);
  void (*quit)();
};

void init(const Actions& actions);
bool isOpen();
void toggle();
void close();

// Draw for this frame (call between aurora_begin_frame and aurora_end_frame).
// presentedFps: frames presented per second, for the counter.
void draw(float presentedFps);

}  // namespace overlay
