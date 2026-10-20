// Host audio sink for the game's AI DMA output (set by the window app; without one the
// AI engine paces itself on the clock and discards the samples).
#pragma once

#include <cstdint>

namespace audioout {

struct Sink {
  void (*submit)(const int16_t* lr, uint32_t frames);  // interleaved L, R (host order)
  uint32_t (*queuedFrames)();
  uint32_t rate;
};

void setSink(const Sink* sink);
const Sink* sink();
void capture(const int16_t* lr, uint32_t frames);  // BLITZ_WAV recording

}  // namespace audioout
