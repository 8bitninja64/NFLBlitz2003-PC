#pragma once

#include <cstdint>

namespace guest {

constexpr uint32_t kReservedSize = 0x40000;  // guest memory below the FST owned by the runtime

struct BootInfo {
  uint32_t entry = 0;
  uint32_t reservedBase = 0;
  uint32_t reservedTop = 0;
};
extern BootInfo g_boot;

bool bootFromArgs(int argc, char** argv);  // disc, memory, devices, DOL (boot_args.cpp)
bool loadDol();
bool setupLowMemory();
uint32_t allocReserved(uint32_t size);  // guest stacks etc. for host-created contexts

}  // namespace guest
