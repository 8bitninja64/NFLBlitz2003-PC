#include "gx_bridge.h"

#include "gx_fifo.h"
#include "log.h"
#include "memory.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXTexture.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace gxbridge {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t field(uint32_t v, int bits, int shift) { return (v >> shift) & ((1u << bits) - 1); }

uint8_t* hostPhys(uint32_t phys) { return guest::g_mem + (phys & guest::kMemMask); }

class Bridge final : public gxfifo::Sink {
public:
  void command(const uint8_t* p, uint32_t size) override;

  // Frame handoff (guest thread <-> Aurora thread).
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<uint8_t> ready;
  bool hasReady = false;
  bool consumed = true;
  bool shutdown = false;

private:
  std::vector<uint8_t> m_out;

  // Output helpers (big-endian, as Aurora's reader expects).
  void put8(uint32_t v) { m_out.push_back((uint8_t)v); }
  void put16(uint32_t v) {
    put8(v >> 8);
    put8(v);
  }
  void put32(uint32_t v) {
    put16(v >> 16);
    put16(v);
  }
  void put64(uint64_t v) {
    put32((uint32_t)(v >> 32));
    put32((uint32_t)v);
  }
  void putRaw(const uint8_t* p, uint32_t n) { m_out.insert(m_out.end(), p, p + n); }
  void aurora(uint16_t sub) {
    put8(GX_AURORA);
    put16(sub);
  }

  void bp(uint32_t v, const uint8_t* raw);
  void cp(uint8_t reg, uint32_t v, const uint8_t* raw);
  void draw(const uint8_t* p, uint32_t size);
  void indexedXf(const uint8_t* p);
  void endFrame();

  // Vertex arrays (CP array numbering: 0 POS, 1 NRM, 2-3 CLR, 4-11 TEX, 12-15 XF arrays).
  struct Array {
    uint32_t base = 0, stride = 0;
    uint64_t emittedPtr = 0;
    uint32_t emittedSize = 0;
  } m_arrays[16];
  void ensureArray(int i, uint32_t needed);

  // Texture maps.
  struct TexMap {
    uint32_t mode0 = 0, mode1 = 0, image0 = 0, image3 = 0, tlut = 0;
    bool dirty = false;
  } m_tex[8];
  void flushTextures();

  // TLUT loads by TMEM offset, and their Aurora slots.
  uint32_t m_tlutSrc = 0;  // BP 0x64
  struct TlutLoad {
    uint32_t phys = 0, entries = 0;
  };
  std::unordered_map<uint32_t, TlutLoad> m_tlutLoads;
  uint32_t m_tlutSlotOffset[20];
  int m_tlutSlotNext = 0;
  int tlutSlot(uint32_t tmemOffset);

  // EFB copy registers.
  uint32_t m_copySrc = 0, m_copySize = 0, m_copyDest = 0;

  bool m_dumpTextures = false;

  // Per-frame content hashes (texture/TLUT versions).
  std::unordered_map<uint64_t, uint32_t> m_hashes;
  uint32_t contentVersion(uint32_t phys, uint32_t size);
};

Bridge g_bridge;

uint32_t Bridge::contentVersion(uint32_t phys, uint32_t size) {
  uint64_t key = (uint64_t)phys << 32 | size;
  auto it = m_hashes.find(key);
  if (it != m_hashes.end()) return it->second;
  if ((phys & guest::kMemMask) + size > guest::kMem1Size) size = guest::kMem1Size - (phys & guest::kMemMask);
  uint32_t v = (uint32_t)XXH3_64bits(hostPhys(phys), size) | 1;
  m_hashes.emplace(key, v);
  return v;
}

int Bridge::tlutSlot(uint32_t tmemOffset) {
  for (int i = 0; i < 20; i++)
    if (m_tlutSlotOffset[i] == tmemOffset + 1) return i;
  int slot = m_tlutSlotNext;
  m_tlutSlotNext = (m_tlutSlotNext + 1) % 20;
  m_tlutSlotOffset[slot] = tmemOffset + 1;
  return slot;
}

void Bridge::ensureArray(int i, uint32_t needed) {
  Array& a = m_arrays[i];
  uint64_t ptr = (uint64_t)(uintptr_t)hostPhys(a.base);
  if (ptr == a.emittedPtr && needed <= a.emittedSize) return;
  uint32_t size = (needed + 255) & ~255u;
  if (ptr == a.emittedPtr && size < a.emittedSize) size = a.emittedSize;
  uint32_t limit = guest::kMem1Size - (a.base & guest::kMemMask);
  if (size > limit) size = limit;
  aurora(GX_AURORA_LOAD_ARRAYBASE + i);
  put64(ptr);
  put32(size);
  put8(0);  // big-endian data
  a.emittedPtr = ptr;
  a.emittedSize = size;
}

void Bridge::flushTextures() {
  for (int id = 0; id < 8; id++) {
    TexMap& t = m_tex[id];
    if (!t.dirty) continue;
    t.dirty = false;
    uint32_t width = field(t.image0, 10, 0) + 1, height = field(t.image0, 10, 10) + 1;
    uint32_t format = field(t.image0, 4, 20);
    uint32_t phys = field(t.image3, 24, 0) << 5;
    uint32_t maxLod = field(t.mode1, 8, 8);
    bool mips = maxLod > 0;
    uint32_t bytes = GXGetTexBufferSize((u16)width, (u16)height, format, mips, (u8)(maxLod / 16 + 1));
    uint32_t tlut = 0;
    if (format == GX_TF_C4 || format == GX_TF_C8 || format == GX_TF_C14X2) {
      uint32_t tmem = field(t.tlut, 10, 0);
      uint32_t tlutFmt = field(t.tlut, 2, 10);
      int slot = tlutSlot(tmem);
      auto it = m_tlutLoads.find(tmem);
      if (it != m_tlutLoads.end()) {
        aurora(GX_AURORA_LOAD_TLUT);
        put8(slot);
        put64((uint64_t)(uintptr_t)hostPhys(it->second.phys));
        put32(tlutFmt);
        put16(it->second.entries);
        put32(it->second.phys | 0x80000000u);
        put32(contentVersion(it->second.phys, it->second.entries * 2));
      }
      tlut = (uint32_t)slot;
    }
    if (m_dumpTextures && width > 1) {
      char name[96];
      snprintf(name, sizeof(name), "build/tex_%08X_%ux%u_f%u.bin", phys, width, height, format);
      if (FILE* f = fopen(name, "wb")) {
        fwrite(hostPhys(phys), 1, bytes, f);
        fclose(f);
      }
    }
    aurora(GX_AURORA_LOAD_TEXOBJ);
    put8(id);
    put64((uint64_t)(uintptr_t)hostPhys(phys));
    put32(width);
    put32(height);
    put32(format);
    put32(tlut);
    put8(mips ? 1 : 0);
    put32((uint32_t)XXH3_64bits_withSeed(&phys, 4, (uint64_t)width << 32 | height << 4 | format));
    put32(contentVersion(phys, bytes));
  }
}

void Bridge::bp(uint32_t v, const uint8_t* raw) {
  uint32_t reg = v >> 24;
  // Texture map registers: 0x80-0x9B (maps 0-3) and 0xA0-0xBB (maps 4-7).
  if ((reg >= 0x80 && reg < 0x9C) || (reg >= 0xA0 && reg < 0xBC)) {
    uint32_t idx = reg - 0x80;
    uint32_t kind = (idx & 0x1F) / 4;
    int map = (int)(idx & 3) + (idx >= 0x20 ? 4 : 0);
    TexMap& t = m_tex[map];
    switch (kind) {
    case 0: t.mode0 = v; break;
    case 1: t.mode1 = v; break;
    case 2: t.image0 = v; t.dirty = true; break;
    case 5: t.image3 = v; t.dirty = true; break;
    case 6: t.tlut = v; t.dirty = true; break;
    }
    putRaw(raw, 5);
    return;
  }
  switch (reg) {
  case 0x64: m_tlutSrc = v; return;  // TLUT load source (Aurora gets LOAD_TLUT instead)
  case 0x65: {
    uint32_t tmem = field(v, 10, 0);
    m_tlutLoads[tmem] = {field(m_tlutSrc, 21, 0) << 5, field(v, 11, 10) * 16};
    for (TexMap& t : m_tex) t.dirty = true;
    return;
  }
  case 0x59: {
    // Scissor box offset: hardware subtracts it (the SDK writes (x + 342) / 2); Aurora's
    // encoding adds a signed offset, so negate both 10-bit halves.
    uint32_t x = (0u - field(v, 10, 0)) & 0x3FF, y = (0u - field(v, 10, 10)) & 0x3FF;
    uint32_t nv = 0x59000000u | y << 10 | x;
    const uint8_t fixed[5] = {0x61, (uint8_t)(nv >> 24), (uint8_t)(nv >> 16), (uint8_t)(nv >> 8), (uint8_t)nv};
    putRaw(fixed, 5);
    return;
  }
  case 0x49: m_copySrc = v; break;
  case 0x4A: m_copySize = v; break;
  case 0x4B: m_copyDest = field(v, 24, 0) << 5; break;
  case 0x52: {
    if (v & (1u << 14)) {  // display copy: end of the guest frame
      endFrame();
      return;
    }
    uint32_t left = field(m_copySrc, 10, 0), top = field(m_copySrc, 10, 10);
    uint32_t w = field(m_copySize, 10, 0) + 1, h = field(m_copySize, 10, 10) + 1;
    bool mip = v & (1u << 9);
    uint32_t pe = field(v, 3, 4) | (field(v, 1, 3) << 3);
    bool intensity = v & (1u << 15);
    uint32_t fmt;
    if (intensity && pe < 4) fmt = pe;                    // I4, I8, IA4, IA8
    else if (pe >= 4 && pe <= 6 && !intensity) fmt = pe;  // RGB565, RGB5A3, RGBA8
    else fmt = 0x20 | pe;                                 // GX_CTF_* copy formats
    aurora(GX_AURORA_LOAD_COPY_SRC);
    put32(left);
    put32(top);
    put32(w);
    put32(h);
    aurora(GX_AURORA_LOAD_COPY_DST);
    put32(mip ? w / 2 : w);
    put32(mip ? h / 2 : h);
    put32(fmt);
    put8(mip ? 1 : 0);
    aurora(GX_AURORA_LOAD_COPY_DEST);
    put64((uint64_t)(uintptr_t)hostPhys(m_copyDest));
    break;
  }
  }
  putRaw(raw, 5);
}

void Bridge::cp(uint8_t reg, uint32_t v, const uint8_t* raw) {
  if (reg >= 0xA0 && reg < 0xB0) {  // array base: Aurora takes host pointers instead
    m_arrays[reg & 15].base = v & 0x03FFFFFF;
    return;
  }
  if (reg >= 0xB0 && reg < 0xC0) m_arrays[reg & 15].stride = v & 0xFF;
  putRaw(raw, 6);
}

void Bridge::draw(const uint8_t* p, uint32_t size) {
  flushTextures();
  uint32_t vat = p[0] & 7;
  uint32_t count = (uint32_t)p[1] << 8 | p[2];
  gxfifo::Attr attrs[gxfifo::kNumAttrs];
  gxfifo::vertexLayout(vat, attrs);
  uint32_t maxIndex[12] = {};
  bool used[12] = {};
  bool anyIndexed = false;
  for (int i = gxfifo::kAttrPos; i < gxfifo::kNumAttrs; i++) anyIndexed |= attrs[i].type >= 2;
  if (anyIndexed) {
    const uint8_t* v = p + 3;
    for (uint32_t n = 0; n < count; n++) {
      for (int i = 0; i < gxfifo::kNumAttrs; i++) {
        const gxfifo::Attr& at = attrs[i];
        if (at.type >= 2) {
          int arr = i - gxfifo::kAttrPos;
          used[arr] = true;
          for (uint32_t k = 0; k < at.indices; k++) {
            uint32_t idx = at.type == 2 ? v[k] : (uint32_t)v[2 * k] << 8 | v[2 * k + 1];
            if (idx > maxIndex[arr]) maxIndex[arr] = idx;
          }
        }
        v += at.size;
      }
    }
    for (int arr = 0; arr < 12; arr++) {
      if (!used[arr]) continue;
      const gxfifo::Attr& at = attrs[arr + gxfifo::kAttrPos];
      (void)at;
      uint32_t stride = m_arrays[arr].stride;
      uint32_t elem = stride ? stride : 4;
      ensureArray(arr, maxIndex[arr] * stride + elem);
    }
  }
  putRaw(p, size);
}

void Bridge::indexedXf(const uint8_t* p) {
  int arr = 12 + (p[0] - 0x20) / 8;
  uint32_t index = (uint32_t)p[1] << 8 | p[2];
  uint32_t len = (p[3] >> 4) + 1;
  ensureArray(arr, index * m_arrays[arr].stride + len * 4);
  putRaw(p, 5);
}

// BLITZ_GX_DROP=bp43,xf1018,cp30,...: drop register loads (bridge debugging).
static bool dropped(const uint8_t* p) {
  static const char* env = getenv("BLITZ_GX_DROP");
  if (!env) return false;
  char key[16];
  if (p[0] == 0x61) snprintf(key, sizeof(key), "bp%02x", p[1]);
  else if (p[0] == 0x08) snprintf(key, sizeof(key), "cp%02x", p[1]);
  else if (p[0] == 0x10) snprintf(key, sizeof(key), "xf%04x", (unsigned)(be32(p + 1) & 0xFFFF));
  else return false;
  return strstr(env, key) != nullptr;
}

void Bridge::command(const uint8_t* p, uint32_t size) {
  uint8_t op = p[0];
  if (dropped(p)) return;
  switch (op) {
  case 0x61: bp(be32(p + 1), p); return;
  case 0x08: cp(p[1], be32(p + 2), p); return;
  case 0x20:
  case 0x28:
  case 0x30:
  case 0x38: indexedXf(p); return;
  case 0x44: return;  // not understood by Aurora; no visible effect
  default:
    if (op >= 0x80) draw(p, size);
    else putRaw(p, size);
  }
}

void Bridge::endFrame() {
  static uint32_t frameNo = 0;
  static const char* dump = getenv("BLITZ_GX_DUMP");  // <frame>: save the translated stream
  if (dump && frameNo == (uint32_t)atoi(dump)) {
    char name[64];
    snprintf(name, sizeof(name), "build/gxframe_%u.bin", frameNo);
    if (FILE* f = fopen(name, "wb")) {
      fwrite(m_out.data(), 1, m_out.size(), f);
      fclose(f);
      LOG_INFO("wrote %s (%zu bytes)", name, m_out.size());
    }
  }
  frameNo++;
  static auto lastEnd = std::chrono::steady_clock::now();
  static double sumWait = 0, sumGuest = 0;
  auto t0 = std::chrono::steady_clock::now();
  sumGuest += std::chrono::duration<double, std::milli>(t0 - lastEnd).count();
  m_dumpTextures = dump && frameNo == (uint32_t)atoi(dump);
  std::unique_lock lk(mutex);
  // Lockstep: wait for Aurora to finish the previous frame before handing over this one.
  cv.wait(lk, [&] { return consumed || shutdown; });
  ready.swap(m_out);
  m_out.clear();
  hasReady = true;
  consumed = false;
  cv.notify_all();
  // ...and until it has consumed this one: it reads guest memory while processing.
  cv.wait(lk, [&] { return consumed || shutdown; });
  lastEnd = std::chrono::steady_clock::now();
  sumWait += std::chrono::duration<double, std::milli>(lastEnd - t0).count();
  static const bool perf = getenv("BLITZ_PERF") != nullptr;
  if (perf && frameNo % 100 == 0) {
    LOG_INFO("gx frames: guest %.1f ms, handoff wait %.1f ms per frame", sumGuest / 100, sumWait / 100);
    sumWait = sumGuest = 0;
  }
  m_hashes.clear();
  if (getenv("BLITZ_DRAWS_BLOCKING")) {
    aurora(GX_AURORA_SET_DRAWS_BLOCKING);
    put8(1);
  }
  // Aurora starts each frame with fresh caches.
  for (Array& a : m_arrays) a.emittedPtr = 0, a.emittedSize = 0;
  for (TexMap& t : m_tex) t.dirty = true;
}

}  // namespace

void init() { gxfifo::setSink(&g_bridge); }

bool takeFrame(std::vector<uint8_t>& out, int timeoutMs) {
  std::unique_lock lk(g_bridge.mutex);
  if (!g_bridge.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [] { return g_bridge.hasReady; })) return false;
  out.swap(g_bridge.ready);
  g_bridge.hasReady = false;
  return true;
}

void frameConsumed() {
  std::lock_guard lk(g_bridge.mutex);
  g_bridge.consumed = true;
  g_bridge.cv.notify_all();
}

void shutdown() {
  std::lock_guard lk(g_bridge.mutex);
  g_bridge.shutdown = true;
  g_bridge.cv.notify_all();
}

}  // namespace gxbridge
