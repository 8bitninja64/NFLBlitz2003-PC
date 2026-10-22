// Cheats from the F1 menu, using the game's own versus-screen codes: entering a code sets a bit in the
// per-controller-port masks at 0x80286040 + 0x64 + port*8 (word id>>5, bit 1<<(id&31); helpers
// 800595AC set / 800595E0 test). Gameplay asks per athlete (80061B3C) or per team (8005929C, the
// team's first human port); the front end clears the masks, nothing clears them during a match.
// The menu's cheats are set on all four ports when a match starts (pre-hook on 8004A098), after the
// versus screen and before match init caches head sizes; cheats read live also apply mid-match.
#include "cheats.h"

#include "hle.h"
#include "log.h"
#include "pad_input.h"
#include "settings.h"
#include "widescreen.h"

namespace cheats {

const Cheat kList[] = {
    {0x0F, "Infinite turbo", "Turbo never runs out (human players)", true, 0xFF},
    {0x09, "Fast passes", "Passes fly faster", true, 0xFF},
    {0x0A, "Fast running", "Players run faster", true, 0xFF},
    {0x04, "Big head", "Big head for the ball carrier", false, 0xFF},
    {0x0E, "Huge head", "Huge head for the ball carrier", false, 0xFF},
    {0x24, "Team big heads", "Every player gets a big head", false, 0xFF},
    {0x21, "Super blitzing", "Defenders blitz harder", true, 0xFF},
    {0x1C, "Power-up offense", "Stronger offensive players", true, 0xFF},
    {0x1A, "Power-up defense", "Stronger defensive players", true, 0xFF},
    {0x19, "Power-up linemen", "Stronger linemen", true, 0xFF},
    {0x23, "Super field goals", "Kicks go much farther", true, 0xFF},
    {0x15, "No punting", "Punts are not allowed", true, 0xFF},
    {0x14, "No interceptions", "Passes can't be picked off", true, 0xFF},
    {0x11, "No first downs", "No first down markers", true, 0xFF},
    {0x16, "No random fumbles", "Only big hits cause fumbles", true, 0xFF},
    {0x10, "No CPU assistance", "The CPU doesn't help the trailing team", true, 0xFF},
    {0x22, "Smart CPU teammates", "Better AI teammates", true, 0xFF},
    {0x13, "No receiver highlight", "Hides the target receiver marker", true, 0xFF},
    {0x0D, "Hide audible names", "Audible names are not shown", true, 0xFF},
    {0x08, "Extra offense plays", "More plays in the playbook", false, 0xFF},
    {0x0C, "Ground fog", "Fog on the field", false, 0xFF},
    {0x06, "Chrome ball", "A shiny chrome football", false, 0x07},
    {0x07, "Classic ball", "The classic NFL Blitz ball", false, 0x06},
    {0x30, "Butter fingaz", "More fumbles", true, 0xFF},
    {0x12, "Noftle mode", "Developer's joke mode", false, 0xFF},
    {0x33, "No replays", "Skips instant replays", true, 0xFF},
};
const int kCount = sizeof(kList) / sizeof(kList[0]);

namespace {

constexpr uint32_t kMasks = 0x80286040 + 0x64;  // u32 mask[2] per controller port

void setBits(uint64_t on, uint64_t off) {
  for (uint32_t port = 0; port < 4; port++) {
    for (uint32_t w = 0; w < 2; w++) {
      uint32_t ea = kMasks + port * 8 + w * 4;
      uint32_t v = guest::rd32(ea);
      v = (v | (uint32_t)(on >> (32 * w))) & ~(uint32_t)(off >> (32 * w));
      guest::wr32(ea, v);
    }
  }
}

uint64_t bit(uint32_t id) { return 1ull << id; }

}  // namespace

void applyNow(uint64_t mask, uint64_t previous) {
  if (!widescreen::inMatch()) return;  // the versus screen and front end manage the masks themselves
  uint64_t on = 0, off = 0;
  for (int i = 0; i < kCount; i++) {
    if (!kList[i].live) continue;
    uint64_t b = bit(kList[i].id);
    if (mask & b) on |= b;
    else if (previous & b) off |= b;
  }
  setBits(on, off);
}

}  // namespace cheats

// 8004A098 builds a match (Blitz Proc, return address 80033E24; the attract demo calls it too).
HLE(MatchCheats) {
  LOG_INFO("match setup: %s", c.lr == 0x80033E24 ? "players' match" : "attract demo");
  if (c.lr != 0x80033E24) return;
  padinput::playersMatchSetup();
  uint64_t mask = settings::get().cheats, on = 0, off = 0;
  for (int i = 0; i < cheats::kCount; i++) {
    const cheats::Cheat& ch = cheats::kList[i];
    if (!(mask & cheats::bit(ch.id))) continue;
    on |= cheats::bit(ch.id);
    if (ch.excludes != 0xFF) off |= cheats::bit(ch.excludes);
  }
  if (on) cheats::setBits(on, off);
}
