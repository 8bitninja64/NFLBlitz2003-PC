#pragma once

#include <cstdint>
#include <string>

namespace guest {

constexpr uint32_t kReservedSize = 0x40000;  // guest memory below the FST owned by the runtime

struct BootInfo {
  uint32_t entry = 0;
  uint32_t reservedBase = 0;
  uint32_t reservedTop = 0;
};
extern BootInfo g_boot;

struct BootOptions {
  std::string rememberedDisc;                       // from settings
  std::string (*pickDisc)() = nullptr;              // asks the user; "" = cancelled
  void (*showError)(const std::string&) = nullptr;  // user-facing error
  std::string modulePath;                           // game module; default: next to the executable
  std::string userDir;                              // also looks for (and builds) the module here
  // Compiles the game module from the disc's executable (written to dolPath) into outPath;
  // called when no module matching the disc exists. nullptr: report an error instead.
  bool (*buildModule)(const std::string& dolPath, const std::string& outPath) = nullptr;
  std::string chosenDisc;                           // out: the disc that was opened
};
bool bootFromArgs(int argc, char** argv, BootOptions* opts = nullptr);  // disc, memory, devices, DOL
bool loadDol();
bool setupLowMemory();
uint32_t allocReserved(uint32_t size);  // guest stacks etc. for host-created contexts

}  // namespace guest
