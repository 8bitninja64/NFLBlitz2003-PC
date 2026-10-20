#include "audio_out.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>

namespace audioout {

static std::atomic<const Sink*> g_sink{nullptr};

void setSink(const Sink* s) { g_sink.store(s); }
const Sink* sink() { return g_sink.load(); }

// BLITZ_WAV=<file>: also record everything the AI plays (32 kHz stereo s16).
void capture(const int16_t* lr, uint32_t frames) {
  static const char* path = getenv("BLITZ_WAV");
  if (!path) return;
  static std::mutex m;
  static FILE* f = nullptr;
  static uint32_t total = 0;
  std::lock_guard lk(m);
  if (!f) {
    f = fopen(path, "wb+");
    if (!f) {
      path = nullptr;
      return;
    }
    uint8_t hdr[44] = {};
    fwrite(hdr, 1, 44, f);
  }
  fwrite(lr, 4, frames, f);
  total += frames;
  // Rewrite the header so the file is valid whenever the process stops.
  long pos = ftell(f);
  auto le32 = [](uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
  };
  uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
  le32(h + 4, 36 + total * 4);
  le32(h + 24, 32000);
  le32(h + 28, 32000 * 4);
  h[32] = 4;
  h[34] = 16;
  memcpy(h + 36, "data", 4);
  le32(h + 40, total * 4);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, 44, f);
  fseek(f, pos, SEEK_SET);
  fflush(f);
}

}  // namespace audioout
