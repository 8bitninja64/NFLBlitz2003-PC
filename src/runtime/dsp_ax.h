// Host implementation of the AX audio microcode's command-list protocol, which the
// game's MusyX library drives (MusyX on GameCube runs on AX). Voices are read from
// parameter blocks in main memory, samples from ARAM; the result is 5 ms (160 frames at
// 32 kHz) of big-endian stereo written where the command list says.
#pragma once

#include <cstdint>

namespace ax {

// Processes the command list at guest address `list`.
void processCommandList(uint32_t list);

}  // namespace ax
