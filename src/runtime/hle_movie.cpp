// Movie playback (the game's own movie player): a pre-hook on its play function skips the
// Midway and Point of View logos and the football intro (intro.pss) when skip_logos is on.
#include "hle.h"
#include "log.h"
#include "settings.h"

#include <cctype>
#include <string>

// 8010A41C (name, callback) plays a movie to the end. Returning 0 is what it does when no
// player is available; the boot sequence then goes on to the title screen.
HLE(MoviePlay) {
  std::string name;
  for (uint32_t a = hle::arg(c, 0); name.size() < 64; a++) {
    char ch = (char)guest::rd8(a);
    if (!ch) break;
    name += (char)std::tolower((unsigned char)ch);
  }
  if ((name == "logo.pss" || name == "povlogo.pss" || name == "intro.pss") && settings::get().skipLogos) {
    LOG_INFO("skipping movie %s", name.c_str());
    hle::ret(c, 0);
    c.r[0] = 1;  // handled: the player does not run
  }
}
