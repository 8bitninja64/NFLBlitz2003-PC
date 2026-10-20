#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace guest {

std::string defaultModulePath();  // BLITZ_MODULE, else blitz_game.dll next to the executable
bool loadGameModule(const std::string& path, std::string* error);
const char* moduleDolHash();      // of the DOL the loaded module was recompiled from
std::string dolHash(const uint8_t* data, size_t size);  // same FNV-1a 64 as the recompiler

}  // namespace guest
