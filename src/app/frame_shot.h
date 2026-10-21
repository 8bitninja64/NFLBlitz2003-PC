// In-process screenshots of chosen frames (BLITZ_SHOT), independent of the window.
#pragma once

#include <cstdint>

namespace frameshot {

void init();                     // reads BLITZ_SHOT; call after Aurora is up
void capture(uint64_t frame);    // after the frame's commands, before the overlay
void poll();                     // each frame: saves finished readbacks

}  // namespace frameshot
