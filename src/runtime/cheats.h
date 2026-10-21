// The game's own versus-screen cheats (table 0x80274A6C), switched on from the F1 menu (hle_cheats.cpp).
#pragma once

#include <cstdint>

namespace cheats {

struct Cheat {
  uint32_t id;           // the game's cheat id (bit in the per-port masks)
  const char* name;
  const char* info;      // one line for the menu
  bool live;             // takes effect mid-match; otherwise from the next match
  uint32_t excludes;     // id of a cheat that can't be on at the same time (0xFF none)
};

extern const Cheat kList[];
extern const int kCount;

// Settings mask (settings::cheats) -> the game, for a match in progress (only call while the game is
// paused, e.g. from the F1 menu). Cheats switched off in the menu are cleared too.
void applyNow(uint64_t mask, uint64_t previous);

}  // namespace cheats
