// Settings overlay (Esc; Dear ImGui via Aurora) and the FPS counter.
#pragma once

#include <string>

namespace overlay {

struct Actions {
  void (*setFullscreen)(bool on);
  void (*applyDisplay)();      // widescreen / HUD / resolution changed
  void (*setVolume)(float gain);
  void (*quit)();
  void (*restart)();  // start the game again (an installed update, another disc), then quit
  // Lets the player pick another disc image: true when a valid one was saved for the next start;
  // otherwise `message` says why not ("" when the player cancelled).
  bool (*chooseDisc)(std::string* message);
};

void init(const Actions& actions);  // after aurora_initialize (loads the menu fonts)
bool isOpen();
bool ownsPads();  // the game must not see the pads (menu open, or its buttons still held)
// Controller input for the menu, per port (GameCube PAD buttons/sticks, as the game reads them).
// Z+Start (or the Back button of a PC gamepad, see main_window) opens and closes the menu.
void padInput(const unsigned short buttons[4], const signed char stickX[4], const signed char stickY[4]);
bool isCapturingKey();  // the Controls tab waits for a key (Esc must not close the menu)
bool wantsEscape();     // a key capture or a popup takes Esc (it must not toggle the menu)
void toggle();
void close();

// Draw for this frame (call between aurora_begin_frame and aurora_end_frame).
// presentedFps: frames presented per second; frame: guest frames consumed (for BLITZ_FRAMENO).
void draw(float presentedFps, unsigned long long frame);

}  // namespace overlay
