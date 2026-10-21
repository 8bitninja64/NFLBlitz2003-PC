// BLITZ_SHOT=<frame>[,<frame>...] or <first>-<last>/<step>: saves the finished EFB of those frames
// (640x480 RGBA8, before the overlay) to build/shots/frame_N.png and frame_N_alpha.png, through
// Aurora's copy readback. Works with the window hidden or covered; needs MSAA off (msaa=1).
#include "frame_shot.h"

#include "log.h"

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace frameshot {
namespace {

constexpr uint32_t kW = 640, kH = 480;
std::set<uint64_t> g_wanted, g_pending;
AuroraReadbackStream g_stream = 0;
alignas(32) uint8_t g_copyKeys[8][32];  // unique "destinations" (copy identities)
std::vector<uint8_t> g_buf;

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// Minimal PNG writer: 8-bit RGB, stored (uncompressed) deflate blocks.
void writePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve((w * 3 + 1) * h);
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t off = 0; off < raw.size(); off += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n == raw.size() ? 1 : 0);
    z.push_back((uint8_t)n), z.push_back((uint8_t)(n >> 8));
    z.push_back((uint8_t)~n), z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
  }
  uint32_t adler = b << 16 | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((uint8_t)(adler >> s));
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    uint8_t len[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
    fwrite(len, 1, 4, f);
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    fwrite(td.data(), 1, td.size(), f);
    uint32_t c = crc32(td.data(), td.size());
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(cb, 1, 4, f);
  };
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  chunk("IHDR", {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w, (uint8_t)(h >> 24), (uint8_t)(h >> 16),
                 (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  fclose(f);
}

void save(uint64_t frame, const std::vector<uint8_t>& tiled) {
  // GX RGBA8 tiles: 4x4 texels, 32 bytes of AR pairs then 32 bytes of GB pairs.
  std::vector<uint8_t> rgb(kW * kH * 3), alpha(kW * kH * 3);
  const uint8_t* p = tiled.data();
  for (uint32_t ty = 0; ty < kH; ty += 4)
    for (uint32_t tx = 0; tx < kW; tx += 4, p += 64)
      for (int k = 0; k < 16; k++) {
        uint32_t o = ((ty + k / 4) * kW + tx + k % 4) * 3;
        rgb[o] = p[2 * k + 1], rgb[o + 1] = p[32 + 2 * k], rgb[o + 2] = p[32 + 2 * k + 1];
        alpha[o] = alpha[o + 1] = alpha[o + 2] = p[2 * k];
      }
  std::error_code ec;
  std::filesystem::create_directories("build/shots", ec);
  std::string base = "build/shots/frame_" + std::to_string(frame);
  writePng(base + ".png", rgb, kW, kH);
  writePng(base + "_alpha.png", alpha, kW, kH);
  LOG_INFO("saved %s.png", base.c_str());
}

}  // namespace

void init() {
  const char* env = getenv("BLITZ_SHOT");
  if (!env) return;
  for (const char* q = env; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) {
    unsigned long long a = 0, b = 0, step = 1;
    int n = sscanf(q, "%llu-%llu/%llu", &a, &b, &step);
    if (n >= 2) {
      for (unsigned long long f = a; f <= b; f += step ? step : 1) g_wanted.insert(f);
    } else if (n == 1) {
      g_wanted.insert(a);
    }
  }
  g_stream = AuroraReadbackCreateStream();
  g_buf.resize(GXGetTexBufferSize(kW, kH, GX_TF_RGBA8, GX_FALSE, 0));
}

void capture(uint64_t frame) {
  if (!g_stream || !g_wanted.count(frame)) return;
  uint8_t* key = g_copyKeys[frame % 8];
  GXSetTexCopySrc(0, 0, kW, kH);
  GXSetTexCopyDst(kW, kH, GX_TF_RGBA8, GX_FALSE);
  GXCopyTex(key, GX_FALSE);
  AuroraReadbackCopySrc src{frame, key};
  if (AuroraReadbackCaptureCopies(g_stream, &src, 1)) g_pending.insert(frame);
  else LOG_WARN("frame shot %llu: readback unavailable (MSAA must be off: BLITZ_SET=msaa=1)", (unsigned long long)frame);
}

void poll() {
  if (g_pending.empty()) return;
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    AuroraReadbackDest dest{*it, g_buf.data(), (u32)g_buf.size(), GX_FALSE};
    if (AuroraReadbackGet(g_stream, &dest, 1) && dest.found) {
      save(*it, g_buf);
      it = g_pending.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace frameshot
