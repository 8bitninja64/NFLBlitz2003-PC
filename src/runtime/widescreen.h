// Game-specific widescreen support (hle_widescreen.cpp): what the GX bridge needs to know about the
// game's state, and the culling the game does against its own 4:3 view.
#pragma once

namespace widescreen {

// An on-field match is running (Hor+ 3D); otherwise the front-end menus are showing.
bool inMatch();
// The end-of-quarter break (match state 0x200000): 3D field, then possibly a loading and tips screen.
bool inQuarterBreak();

}  // namespace widescreen
