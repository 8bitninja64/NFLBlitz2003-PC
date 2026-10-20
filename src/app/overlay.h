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
bool isCapturingKey();  // the Controls tab waits for a key (Esc must not close the menu)
void toggle();
void close();

// Draw for this frame (call between aurora_begin_frame and aurora_end_frame).
// presentedFps: frames presented per second; frame: guest frames consumed (for BLITZ_FRAMENO).
void draw(float presentedFps, unsigned long long frame);

}  // namespace overlay
