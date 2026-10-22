// Linux first start: compiles the game code from the player's disc in a window of its own.
#pragma once

#include <string>

namespace firststart {

// Runs `sh <script> <dol> <out> <work dir of dol>` (tools/make_module.sh): asks first unless `ask` is
// false, then shows the build's steps and progress; on failure the end of its output (all of it goes
// to build.log beside the dol). True when <out> was built.
bool build(const std::string& script, const std::string& dolPath, const std::string& outPath, bool ask);

}  // namespace firststart
