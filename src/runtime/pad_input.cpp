#include "pad_input.h"

#include "log.h"
#include "widescreen.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace padinput {

namespace {

std::mutex g_mutex;
Pad g_pads[kPorts];
uint32_t g_motor[kPorts];
bool g_motorDirty[kPorts];
std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_playersMatchSetup{false};
uint32_t g_matchStart = 0;  // frame the players' match started (0: not yet)

// BLITZ_INPUT="<frame>:<BUTTON[+BUTTON]>[*<frames held>],..." on port 0, frames = VI retraces.
// <frame> may also be m+<n> (n frames after the players' match started: gameplay input that must not
// land in the menus when loading is slow) or <first>-m/<period> (every <period> frames from <first>
// until the players' match starts: menu presses for however long the menus take).
// Buttons: A B X Y Z L R START UP DOWN LEFT RIGHT, plus STICKUP/STICKDOWN/STICKLEFT/STICKRIGHT.
struct Step {
  uint32_t frame, hold;
  uint16_t buttons;
  int8_t sx, sy;
  bool afterMatch = false;   // frame counts from the players' match start
  uint32_t untilMatch = 0;   // repeat period until the players' match starts (0: once)
};
std::vector<Step> g_script;
bool g_scriptLoaded = false;

void loadScript() {
  g_scriptLoaded = true;
  const char* env = getenv("BLITZ_INPUT");
  if (!env) return;
  std::string s = env;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(pos, end - pos);
    pos = end + 1;
    size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    Step st{(uint32_t)atoi(item.c_str()), 6, 0, 0, 0};
    if (item[0] == 'm') {
      st.afterMatch = true;
      st.frame = (uint32_t)atoi(item.c_str() + 2);
    } else if (size_t dash = item.find("-m/"); dash < colon) {
      st.untilMatch = (uint32_t)atoi(item.c_str() + dash + 3);
    }
    std::string rest = item.substr(colon + 1);
    size_t star = rest.find('*');
    if (star != std::string::npos) {
      st.hold = (uint32_t)atoi(rest.c_str() + star + 1);
      rest = rest.substr(0, star);
    }
    size_t p = 0;
    while (p <= rest.size()) {
      size_t plus = rest.find('+', p);
      if (plus == std::string::npos) plus = rest.size();
      std::string b = rest.substr(p, plus - p);
      p = plus + 1;
      static const struct {
        const char* name;
        uint16_t bit;
      } kNames[] = {{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008},  {"Z", 0x0010},
                    {"R", 0x0020},    {"L", 0x0040},     {"A", 0x0100},    {"B", 0x0200},   {"X", 0x0400},
                    {"Y", 0x0800},    {"START", 0x1000}};
      bool found = false;
      for (auto& n : kNames)
        if (b == n.name) st.buttons |= n.bit, found = true;
      if (b == "STICKUP") st.sy = 100, found = true;
      if (b == "STICKDOWN") st.sy = -100, found = true;
      if (b == "STICKLEFT") st.sx = -100, found = true;
      if (b == "STICKRIGHT") st.sx = 100, found = true;
      if (!found && !b.empty()) LOG_WARN("BLITZ_INPUT: unknown button '%s'", b.c_str());
    }
    g_script.push_back(st);
  }
  LOG_INFO("scripted input: %zu steps", g_script.size());
}

}  // namespace

void publish(const Pad pads[kPorts]) {
  std::lock_guard lk(g_mutex);
  memcpy(g_pads, pads, sizeof(g_pads));
}

void read(Pad pads[kPorts]) {
  {
    std::lock_guard lk(g_mutex);
    if (!g_scriptLoaded) loadScript();
    memcpy(pads, g_pads, sizeof(g_pads));
  }
  if (g_script.empty()) return;
  uint32_t f = g_frame.load();
  if (!g_matchStart && g_playersMatchSetup.load() && widescreen::inMatch()) {
    g_matchStart = f;
    LOG_INFO("scripted input: players' match from frame %u", f);
  }
  pads[0].err = 0;
  for (const Step& st : g_script) {
    uint32_t at = st.frame;
    if (st.afterMatch) {
      if (!g_matchStart) continue;
      at += g_matchStart;
    } else if (st.untilMatch) {
      if (g_matchStart || f < st.frame) continue;
      at = f - (f - st.frame) % st.untilMatch;
    }
    if (f >= at && f < at + st.hold) {
      pads[0].button |= st.buttons;
      if (st.sx) pads[0].stickX = st.sx;
      if (st.sy) pads[0].stickY = st.sy;
      if (st.buttons & 0x0100) pads[0].analogA = 0xFF;
      if (st.buttons & 0x0200) pads[0].analogB = 0xFF;
      if (st.buttons & 0x0040) pads[0].triggerL = 0xFF;
      if (st.buttons & 0x0020) pads[0].triggerR = 0xFF;
    }
  }
}

void setMotor(int port, uint32_t cmd) {
  if (port < 0 || port >= kPorts) return;
  std::lock_guard lk(g_mutex);
  if (g_motor[port] != cmd) {
    g_motor[port] = cmd;
    g_motorDirty[port] = true;
  }
}

bool takeMotor(int port, uint32_t* cmd) {
  std::lock_guard lk(g_mutex);
  if (!g_motorDirty[port]) return false;
  g_motorDirty[port] = false;
  *cmd = g_motor[port];
  return true;
}

void tick() { g_frame.fetch_add(1); }

void playersMatchSetup() { g_playersMatchSetup.store(true); }

}  // namespace padinput
