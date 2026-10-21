#include "gx_bridge.h"

#include "gx_fifo.h"
#include "hw.h"
#include "log.h"
#include "memory.h"
#include "sched.h"
#include "settings.h"
#include "widescreen.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXTexture.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gxbridge {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t field(uint32_t v, int bits, int shift) { return (v >> shift) & ((1u << bits) - 1); }

uint8_t* hostPhys(uint32_t phys) { return guest::g_mem + (phys & guest::kMemMask); }

// Copies of guest memory for the frame being built. Aurora reads vertex arrays and TLUTs when it
// processes the frame, after the game may have reused that memory (it does: skinned vertices
// and palettes change between draws), while the hardware reads them when the GP executes the
// command. Blocks stay valid until Aurora has consumed the frame.
class Arena {
public:
  uint8_t* copy(const uint8_t* src, size_t size) {
    size_t aligned = (size + 31) & ~size_t(31);
    if (m_block >= m_blocks.size() || m_used + aligned > m_blocks[m_block].size) {
      if (m_block < m_blocks.size()) m_block++;
      while (m_block < m_blocks.size() && m_blocks[m_block].size < aligned) m_block++;
      if (m_block >= m_blocks.size()) {
        size_t n = aligned > kBlock ? aligned : kBlock;
        m_blocks.push_back({std::make_unique<uint8_t[]>(n), n});
        m_block = m_blocks.size() - 1;
      }
      m_used = 0;
    }
    uint8_t* p = m_blocks[m_block].data.get() + m_used;
    memcpy(p, src, size);
    m_used += aligned;
    return p;
  }
  void reset() { m_block = 0, m_used = 0; }

private:
  static constexpr size_t kBlock = 8 << 20;
  struct Block {
    std::unique_ptr<uint8_t[]> data;
    size_t size;
  };
  std::vector<Block> m_blocks;
  size_t m_block = 0, m_used = 0;
};

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
  void xf(const uint8_t* p, uint32_t size);
  uint32_t m_projType = 0;  // last XF 0x1026 (0 perspective, 1 orthographic)
  // Widescreen with centered 2D: full-screen 2D draws (fades, flashes, clears) still cover the
  // whole screen. Needs the guest's position matrices and unpatched projection.
  uint32_t m_xfMtx[0x100] = {};  // XF matrix memory 0x000-0x0FF (position/texture matrices)
  uint32_t m_proj[7] = {};       // XF 0x1020-0x1026 as the game wrote them
  uint32_t m_mtxIdxA = 0;        // XF 0x1018
  bool m_frameHas3d = false;     // a perspective draw happened earlier in this frame
  bool m_menuMode = false;       // this frame is a front-end menu to stretch (latched per frame)
  // Menu art keeps out of the top/bottom ~12 lines (TV overscan); this zoom fills those bars.
  static constexpr float kMenuZoom = 1.05f;
  bool isFullScreen2d(const uint8_t* p, uint32_t count, const gxfifo::Attr* attrs, uint32_t vat);
  void putProjX(float scale);
  void cp(uint8_t reg, uint32_t v, const uint8_t* raw);
  void draw(const uint8_t* p, uint32_t size);
  void indexedXf(const uint8_t* p);
  void endFrame();

  // Vertex arrays (CP array numbering: 0 POS, 1 NRM, 2-3 CLR, 4-11 TEX, 12-15 XF arrays).
  struct Array {
    uint32_t base = 0, stride = 0;
    uint32_t emittedBase = 0, emittedSize = 0;  // the snapshot Aurora was last given
    const uint8_t* snapshot = nullptr;
  } m_arrays[16];
  Arena m_arena;
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
    std::vector<uint8_t> tmem;  // copied at load time, as the hardware does (TMEM keeps it)
    uint32_t version = 0;
  };
  std::unordered_map<uint32_t, TlutLoad> m_tlutLoads;
  uint32_t m_tlutSlotOffset[20];
  struct TlutCopy {  // this frame's arena copy per Aurora slot (one pointer while unchanged)
    const uint8_t* data = nullptr;
    uint32_t tmem = 0, version = 0;
  } m_tlutCopies[20];
  int m_tlutSlotNext = 0;
  int tlutSlot(uint32_t tmemOffset);

  // EFB copy registers.
  uint32_t m_copySrc = 0, m_copySize = 0, m_copyDest = 0;
  // Aurora keeps EFB copies on the GPU, keyed by destination, until told to drop them; guest RAM
  // at the destination is untouched. When the game later puts something else there (the helmet
  // sphere map landed on an old copy of the team-select background), RAM no longer matches what
  // it held at copy time and the copy is stale, as it would be on hardware.
  struct CopyRecord {
    uint32_t size;
    uint64_t ramHash;
  };
  std::unordered_map<uint32_t, CopyRecord> m_copies;  // by destination (physical)
  uint64_t ramHash(uint32_t phys, uint32_t size) {
    phys &= guest::kMemMask;
    if (phys >= guest::kMem1Size) return 0;
    if (phys + size > guest::kMem1Size) size = guest::kMem1Size - phys;
    return XXH3_64bits(hostPhys(phys), size);
  }

  bool m_dumpTextures = false;
  uint64_t m_arrayBytes = 0, m_arrayLoads = 0;  // per frame
  uint64_t m_invalidates = 0, m_invalidatedBytes = 0;
  uint32_t m_tevC0 = 0, m_tevA0 = 0;  // BP C0/C1 (BLITZ_GX_PAINT)
  uint32_t m_bpShadow[256] = {};       // last value of every BP register (debug overrides restore it)
  uint32_t m_drawInFrame = 0;
  uint32_t m_genMode = 0, m_bp40 = 0, m_bp41 = 0, m_bpF3 = 0, m_bp42 = 0, m_bp43 = 0;
  std::unordered_map<uint64_t, uint32_t> m_blendHist;
  std::deque<std::pair<uint32_t, std::vector<uint8_t>>> m_ring;  // BLITZ_GX_RING
  // BLITZ_GX_HAZARD: memory a draw referenced that changed before Aurora reads it.
  struct Use {
    uint64_t hash;
    int kind;
  };
  std::unordered_map<uint64_t, Use> m_uses;  // (host offset << 24 | size) -> last hash
  uint32_t m_hazardMid[18] = {}, m_hazardLate[18] = {};
  void noteUse(int kind, uint32_t phys, uint32_t size);

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

// kind: 0-11 vertex arrays, 12-15 indexed XF arrays, 16 texture, 17 TLUT.
void Bridge::noteUse(int kind, uint32_t phys, uint32_t size) {
  phys &= guest::kMemMask;
  if (phys >= guest::kMem1Size || size == 0) return;
  if (phys + size > guest::kMem1Size) size = guest::kMem1Size - phys;
  uint64_t key = (uint64_t)phys << 24 | (size & 0xFFFFFF);
  uint64_t h = XXH3_64bits(hostPhys(phys), size);
  auto [it, inserted] = m_uses.try_emplace(key, Use{h, kind});
  if (!inserted && it->second.hash != h) {
    m_hazardMid[kind]++;
    it->second.hash = h;
  }
}

void Bridge::ensureArray(int i, uint32_t needed) {
  Array& a = m_arrays[i];
  uint32_t phys = a.base & guest::kMemMask;
  uint32_t limit = phys < guest::kMem1Size ? guest::kMem1Size - phys : 0;
  if (needed > limit) needed = limit;
  // Reuse the last snapshot while it covers the range and the game has not changed the data.
  if (a.snapshot && a.emittedBase == a.base && needed <= a.emittedSize &&
      memcmp(a.snapshot, hostPhys(phys), needed) == 0)
    return;
  if (needed > 0x01000000) LOG_ERROR("GX: vertex array %d needs %u bytes (base %08X stride %u)", i, needed, a.base, a.stride);
  uint32_t size = (needed + 255) & ~255u;
  if (a.emittedBase == a.base && size < a.emittedSize) size = a.emittedSize;
  if (size > limit) size = limit;
  m_arrayBytes += size;
  m_arrayLoads++;
  a.snapshot = m_arena.copy(hostPhys(phys), size);
  a.emittedBase = a.base;
  a.emittedSize = size;
  aurora(GX_AURORA_LOAD_ARRAYBASE + i);
  put64((uint64_t)(uintptr_t)a.snapshot);
  put32(size);
  put8(0);  // big-endian data
}

void Bridge::flushTextures() {
  static const bool noTex = getenv("BLITZ_GX_NOTEX") != nullptr;  // debugging
  if (noTex) return;
  static const bool hazard = getenv("BLITZ_GX_HAZARD") != nullptr;
  for (int id = 0; id < 8; id++) {
    TexMap& t = m_tex[id];
    if (!t.dirty) continue;
    t.dirty = false;
    uint32_t width = field(t.image0, 10, 0) + 1, height = field(t.image0, 10, 10) + 1;
    uint32_t format = field(t.image0, 4, 20);
    uint32_t phys = field(t.image3, 24, 0) << 5;
    static const bool copyLog = getenv("BLITZ_GX_COPYLOG") != nullptr;
    if (copyLog && m_copies.count(phys & guest::kMemMask)) {
      static int logged = 0;
      if (logged++ < 50)
        LOG_INFO("GX bind of copy address %08X (%ux%u fmt %u): RAM %s", phys, width, height, format,
                 ramHash(phys, m_copies[phys & guest::kMemMask].size) == m_copies[phys & guest::kMemMask].ramHash ? "unchanged" : "changed");
    }
    if (auto copy = m_copies.find(phys & guest::kMemMask); copy != m_copies.end() &&
                                                        ramHash(phys, copy->second.size) != copy->second.ramHash) {
      aurora(GX_AURORA_DESTROY_COPY_TEX);
      put64((uint64_t)(uintptr_t)hostPhys(phys));
      m_copies.erase(copy);
    }
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
        TlutCopy& copy = m_tlutCopies[slot];
        if (!copy.data || copy.tmem != tmem || copy.version != it->second.version)
          copy = {m_arena.copy(it->second.tmem.data(), it->second.tmem.size()), tmem, it->second.version};
        put64((uint64_t)(uintptr_t)copy.data);
        put32(tlutFmt);
        put16(it->second.entries);
        put32(it->second.phys | 0x80000000u);
        put32(it->second.version);
        if (hazard) noteUse(17, it->second.phys, it->second.entries * 2);
        if (m_dumpTextures) {
          char name[96];
          snprintf(name, sizeof(name), "build/tlut_%08X_%u_f%u.bin", it->second.phys, it->second.entries, tlutFmt);
          if (FILE* f = fopen(name, "wb")) {
            fwrite(it->second.tmem.data(), 1, it->second.tmem.size(), f);
            fclose(f);
          }
        }
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
    if (hazard) noteUse(16, phys, bytes);
  }
}

void Bridge::bp(uint32_t v, const uint8_t* raw) {
  uint32_t reg = v >> 24;
  if (reg != 0xFE) m_bpShadow[reg] = v & 0xFFFFFF;
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
  // BLITZ_GX_BPREPL=<reg>:<value>:<replacement>[,...]: substitute BP register values (debugging).
  static const char* bpRepl = getenv("BLITZ_GX_BPREPL");
  if (bpRepl) {
    for (const char* q = bpRepl; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) {
      unsigned r = 0, from = 0, to = 0;
      if (sscanf(q, "%x:%x:%x", &r, &from, &to) == 3 && r == reg && (v & 0xFFFFFF) == from) {
        uint32_t nv = reg << 24 | (to & 0xFFFFFF);
        const uint8_t fixed[5] = {0x61, (uint8_t)(nv >> 24), (uint8_t)(nv >> 16), (uint8_t)(nv >> 8), (uint8_t)nv};
        return bp(nv, fixed);
      }
    }
  }
  // BLITZ_GX_LOGBP=f6,fe,...: log writes to these BP registers (first 200).
  static const char* logBp = getenv("BLITZ_GX_LOGBP");
  if (logBp) {
    static int logged = 0;
    char key[8];
    snprintf(key, sizeof(key), "%02x", reg);
    if (logged < 200 && strstr(logBp, key)) {
      logged++;
      LOG_INFO("GX BP %02X = %06X", reg, v & 0xFFFFFF);
    }
  }
  switch (reg) {
  case 0x64: m_tlutSrc = v; return;  // TLUT load source (Aurora gets LOAD_TLUT instead)
  case 0x65: {
    uint32_t tmem = field(v, 10, 0);
    uint32_t phys = (field(m_tlutSrc, 21, 0) << 5) & guest::kMemMask;
    uint32_t entries = field(v, 11, 10) * 16;
    if (phys + entries * 2 > guest::kMem1Size) entries = (guest::kMem1Size - phys) / 2;
    TlutLoad& load = m_tlutLoads[tmem];
    load.phys = phys;
    load.entries = entries;
    load.tmem.assign(hostPhys(phys), hostPhys(phys) + entries * 2);
    load.version = (uint32_t)XXH3_64bits(load.tmem.data(), load.tmem.size()) | 1;
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
  case 0x4F:
  case 0x50:
    // The game clears to white; with 2D screens centered in a wider frame the cleared
    // margins would show, so clear them to black instead (the alpha byte is kept).
    if (settings::get().widescreen && settings::get().hud == settings::Hud2D::Centered) {
      uint32_t nv = reg == 0x4F ? (v & 0xFFFFFF00u) : (v & 0xFF000000u);
      const uint8_t fixed[5] = {0x61, (uint8_t)(nv >> 24), (uint8_t)(nv >> 16), (uint8_t)(nv >> 8), (uint8_t)nv};
      putRaw(fixed, 5);
      return;
    }
    break;
  case 0x00: m_genMode = v & 0xFFFFFF; break;
  case 0xC0: m_tevC0 = v & 0xFFFFFF; break;
  case 0xC1: m_tevA0 = v & 0xFFFFFF; break;
  case 0x40: m_bp40 = v & 0xFFFFFF; break;
  case 0x41: m_bp41 = v & 0xFFFFFF; break;
  case 0xF3: m_bpF3 = v & 0xFFFFFF; break;
  case 0x42: m_bp42 = v & 0xFFFFFF; break;
  case 0x43: m_bp43 = v & 0xFFFFFF; break;
  case 0x49: m_copySrc = v; break;
  case 0x4A: m_copySize = v; break;
  case 0x4B: m_copyDest = field(v, 24, 0) << 5; break;
  case 0x52: {
    if (v & (1u << 14)) {  // display copy: end of the guest frame
      endFrame();
      return;
    }
    static const bool noCopy = getenv("BLITZ_GX_NOCOPY") != nullptr;  // debugging
    if (noCopy) return;
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
    {
      uint32_t cw = mip ? w / 2 : w, ch = mip ? h / 2 : h;
      uint32_t size = (fmt & 0x20) || fmt == 6 ? cw * ch * 4 : cw * ch * 2;  // upper bound per format
      m_copies[m_copyDest & guest::kMemMask] = {size, ramHash(m_copyDest, size)};
      static const bool copyLog = getenv("BLITZ_GX_COPYLOG") != nullptr;
      if (copyLog) LOG_INFO("GX copy -> %08X %ux%u fmt %X (%u bytes)", m_copyDest, cw, ch, fmt, size);
    }
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
  // Too few vertices for the primitive draws nothing on hardware; Aurora would underflow its
  // primitive count (a 0-vertex strip allocated an 8 GB index buffer).
  {
    uint32_t n = (uint32_t)p[1] << 8 | p[2];
    uint32_t prim = (p[0] >> 3) & 7;  // 0 quads, 1 quads2, 2 tris, 3 strip, 4 fan, 5 lines, 6 linestrip, 7 points
    static const uint8_t kMin[8] = {4, 4, 3, 3, 3, 2, 2, 1};
    if (n < kMin[prim]) {
      static const bool dropLog = getenv("BLITZ_GX_DROPLOG") != nullptr;
      static int logged = 0;
      if (dropLog && n > 0 && logged++ < 50) LOG_INFO("GX dropped draw: prim %u with %u vertices", prim, n);
      return;
    }
  }
  // BLITZ_GX_SKIPBLEND=<hex>: drop draws whose blend mode (BP 0x41 low 16 bits) matches (debugging).
  static const long skipBlend = getenv("BLITZ_GX_SKIPBLEND") ? strtol(getenv("BLITZ_GX_SKIPBLEND"), nullptr, 16) : -1;
  if (skipBlend >= 0 && (long)(m_bp41 & 0xFFFF) == skipBlend) return;
  // BLITZ_GX_SKIPFMT=<fmt>[,...]: drop draws whose texture map 0 has this format (debugging).
  static const char* skipFmt = getenv("BLITZ_GX_SKIPFMT");
  if (skipFmt) {
    char key[8];
    snprintf(key, sizeof(key), ",%u,", field(m_tex[0].image0, 4, 20));
    std::string list = std::string(",") + skipFmt + ",";
    if (list.find(key) != std::string::npos) return;
  }
  flushTextures();
  // BLITZ_BLEND_LOG: per-frame histogram of (blend mode BP 0x41, alpha compare BP 0xF3, pixel format BP 0x43).
  static const bool blendLog = getenv("BLITZ_BLEND_LOG") != nullptr;
  if (blendLog) m_blendHist[(uint64_t)(m_bp41 & 0xFFFF) << 32 | (m_bpF3 & 0xFFFFFF)]++;
  uint32_t vat = p[0] & 7;
  uint32_t count = (uint32_t)p[1] << 8 | p[2];
  static const bool hazard = getenv("BLITZ_GX_HAZARD") != nullptr;
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
      static const bool nrmStats = getenv("BLITZ_GX_NRMSTATS") != nullptr;  // debugging
      if (nrmStats && arr == 1 && stride >= 12) {
        static int logged = 0;
        if (logged < 40) {
          logged++;
          float mn = 1e9f, mx = 0;
          for (uint32_t k = 0; k <= maxIndex[1] && k < 64; k++) {
            const uint8_t* e = hostPhys(m_arrays[1].base + k * stride);
            float c[3];
            for (int j = 0; j < 3; j++) {
              uint32_t bits = be32(e + 4 * j);
              memcpy(&c[j], &bits, 4);
            }
            float len = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            mn = len < mn ? len : mn, mx = len > mx ? len : mx;
          }
          LOG_INFO("GX normals: draw of %u verts, array %08X stride %u, |n| %.3f..%.3f", count, m_arrays[1].base, stride, mn, mx);
        }
      }
      if (hazard) noteUse(arr, m_arrays[arr].base, maxIndex[arr] * stride + elem);
    }
  }
  // BLITZ_GX_POSLOG=<stages>: log indexed positions of draws with this many TEV stages (debugging).
  static const int posLogStages = getenv("BLITZ_GX_POSLOG") ? atoi(getenv("BLITZ_GX_POSLOG")) : 0;
  static const uint32_t posLogCount = getenv("BLITZ_GX_POSLOG_N") ? (uint32_t)atoi(getenv("BLITZ_GX_POSLOG_N")) : 0;
  if (posLogStages && (int)field(m_genMode, 4, 10) + 1 >= posLogStages && attrs[gxfifo::kAttrPos].type >= 2 &&
      (!posLogCount || posLogCount == count)) {
    static int logged = 0;
    if (logged < 60) {
      logged++;
      const Array& pa = m_arrays[0];
      uint32_t offset = 0;
      for (int i = 0; i < gxfifo::kAttrPos; i++) offset += attrs[i].size;
      std::string line;
      const uint8_t* v = p + 3 + offset;
      uint32_t stride = 0;
      for (int i = 0; i < gxfifo::kNumAttrs; i++) stride += attrs[i].size;
      for (uint32_t n = 0; n < count && n < 3; n++, v += stride) {
        uint32_t idx = attrs[gxfifo::kAttrPos].type == 2 ? v[0] : (uint32_t)v[0] << 8 | v[1];
        const uint8_t* e = hostPhys(pa.base + idx * pa.stride);
        float c[3];
        for (int j = 0; j < 3; j++) {
          uint32_t bits = be32(e + 4 * j);
          memcpy(&c[j], &bits, 4);
        }
        char b[96];
        snprintf(b, sizeof(b), " [%u]=(%.2f %.2f %.2f)", idx, c[0], c[1], c[2]);
        line += b;
      }
      float m3;
      memcpy(&m3, &m_xfMtx[(m_mtxIdxA & 63) * 4 + 3], 4);
      // GX depth range of the draw: eye z from the position matrix, then the projection.
      auto f32 = [](uint32_t b) {
        float f;
        memcpy(&f, &b, 4);
        return f;
      };
      const uint32_t* m = &m_xfMtx[(m_mtxIdxA & 63) * 4];
      float p4 = f32(m_proj[4]), p5 = f32(m_proj[5]), dmin = 9, dmax = -9;
      float p0 = f32(m_proj[0]), p1 = f32(m_proj[1]), p2 = f32(m_proj[2]), p3 = f32(m_proj[3]);
      float xmin = 9, xmax = -9, ymin = 9, ymax = -9;
      const uint8_t* vv = p + 3 + offset;
      for (uint32_t n = 0; n < count; n++, vv += stride) {
        uint32_t idx = attrs[gxfifo::kAttrPos].type == 2 ? vv[0] : (uint32_t)vv[0] << 8 | vv[1];
        const uint8_t* e = hostPhys(pa.base + idx * pa.stride);
        float x = f32(be32(e)), y = f32(be32(e + 4)), z = f32(be32(e + 8));
        float ez = f32(m[8]) * x + f32(m[9]) * y + f32(m[10]) * z + f32(m[11]);
        float d = m_projType ? 1 + p4 * ez + p5 : 1 + (p4 * ez + p5) / -ez;
        float ex = f32(m[0]) * x + f32(m[1]) * y + f32(m[2]) * z + f32(m[3]);
        float ey = f32(m[4]) * x + f32(m[5]) * y + f32(m[6]) * z + f32(m[7]);
        float nx = m_projType ? p0 * ex + p1 : (p0 * ex + p1 * ez) / -ez;
        float ny = m_projType ? p2 * ey + p3 : (p2 * ey + p3 * ez) / -ez;
        xmin = nx < xmin ? nx : xmin, xmax = nx > xmax ? nx : xmax, ymin = ny < ymin ? ny : ymin, ymax = ny > ymax ? ny : ymax;
        dmin = d < dmin ? d : dmin, dmax = d > dmax ? d : dmax;
      }
      LOG_INFO("pos stages=%u n=%u base %08X stride %u mtx %u tx %.1f depth %.5f..%.5f ndc x %.2f..%.2f y %.2f..%.2f:%s",
               field(m_genMode, 4, 10) + 1, count, pa.base, pa.stride, m_mtxIdxA & 63, m3, dmin, dmax, xmin, xmax, ymin,
               ymax, line.c_str());
    }
  }
  const settings::Settings& st = settings::get();
  bool stretch = st.widescreen && !m_menuMode && st.hud == settings::Hud2D::Centered && m_projType == 1 &&
                 isFullScreen2d(p, count, attrs, vat);
  if (m_projType == 0) m_frameHas3d = true;
  static const bool forceDirty = getenv("BLITZ_GX_FORCEDIRTY") != nullptr;  // experiment
  if (forceDirty) {
    put8(0x61);
    put32(0x41000000u | ((m_bp41 ^ 0x4) & 0xFFFFFF));
    put8(0x61);
    put32(0x41000000u | m_bp41);
  }
  // BLITZ_GX_DRAWSTACK=<vertex count>[,...]: log the guest call stack of draws with these vertex counts
  // (the parser runs inside the game's FIFO writes, so it is the code that issued the draw).
  static const char* drawStack = getenv("BLITZ_GX_DRAWSTACK");
  if (drawStack) {
    static int logged = 0;
    char keyN[16];
    snprintf(keyN, sizeof(keyN), ",%u,", count);
    if (logged < 60 && (std::string(",") + drawStack + ",").find(keyN) != std::string::npos) {
      logged++;
      LOG_INFO("GX draw of %u verts: %s", count, sched::currentBacktrace().c_str());
    }
  }
  // BLITZ_GX_PAINT=<draw index>|n<vertex count>[,...]: draw these draws (per frame) solid white, opaque
  // (debugging; draw order varies from run to run, vertex counts do not).
  static const char* paint = getenv("BLITZ_GX_PAINT");
  uint32_t drawIndex = m_drawInFrame++;
  if (paint) {
    char key[16], keyN[16];
    snprintf(key, sizeof(key), ",%u,", drawIndex);
    snprintf(keyN, sizeof(keyN), ",n%u,", count);
    char keyS[16];
    snprintf(keyS, sizeof(keyS), ",s%u,", field(m_genMode, 4, 10) + 1);  // s<TEV stages>
    std::string list = std::string(",") + paint + ",";
    if (list.find(key) != std::string::npos || list.find(keyN) != std::string::npos ||
        list.find(keyS) != std::string::npos) {
      auto bpw = [&](uint32_t val) {
        put8(0x61);
        put32(val);
      };
      bpw(0x00000000u | (m_genMode & ~(0xFu << 10)));  // one TEV stage
      bpw(0xC0000000u | 0x08FFFCu);                       // color = 1
      bpw(0xC1000000u | 0x08FFE0u);                       // alpha = konst (1)
      bpw(0x41000000u | 0x00011Du);                       // replace
      putRaw(p, size);
      bpw(0x00000000u | m_genMode);
      bpw(0xC0000000u | m_tevC0);
      bpw(0xC1000000u | m_tevA0);
      bpw(0x41000000u | m_bp41);
      return;
    }
  }
  // BLITZ_GX_DRAWBP=<draw index>|s<TEV stages>:<reg>=<value>[;<reg>=<value>...]: BP overrides for one draw,
  // or for every draw with that many TEV stages (debugging).
  static const char* drawBp = getenv("BLITZ_GX_DRAWBP");
  if (drawBp && (drawBp[0] == 's' ? (uint32_t)atoi(drawBp + 1) == field(m_genMode, 4, 10) + 1
                                  : (uint32_t)atoi(drawBp) == drawIndex)) {
    std::vector<uint32_t> regs;
    for (const char* q = strchr(drawBp, ':'); q && *q; q = strchr(q, ';')) {
      q++;
      unsigned r = 0, val = 0;
      if (sscanf(q, "%x=%x", &r, &val) == 2) {
        put8(0x61);
        put32(r << 24 | (val & 0xFFFFFF));
        regs.push_back(r);
      }
    }
    putRaw(p, size);
    for (uint32_t r : regs) {  // restore what the game set
      uint32_t val = r == 0x00 ? m_genMode : r == 0x41 ? m_bp41 : r == 0xC0 ? m_tevC0 : r == 0xC1 ? m_tevA0 : m_bpShadow[r];
      put8(0x61);
      put32(r << 24 | val);
    }
    return;
  }
  if (!stretch) {
    putRaw(p, size);
    return;
  }
  putProjX(1.0f);
  putRaw(p, size);
  putProjX((4.0f / 3.0f) / settings::g_displayAspect.load());
}

// A 2D draw over the whole 640-wide screen whose job is to cover it: a fade/flash over the 3D
// scene, or a clear quad (depth test "always" with depth writes). Pure 2D screens stay 4:3.
bool Bridge::isFullScreen2d(const uint8_t* p, uint32_t count, const gxfifo::Attr* attrs, uint32_t vat) {
  const gxfifo::Attr& pos = attrs[gxfifo::kAttrPos];
  uint32_t vatA = gxfifo::vatReg(0, vat);
  if (pos.type != 1 || field(vatA, 3, 1) != 4 || attrs[0].type) return false;  // direct F32, one matrix
  bool clearLike = (m_bp40 & 0x1F) == 0x1F;  // enabled, ALWAYS, update
  if (!m_frameHas3d && !clearLike) return false;
  uint32_t offset = 0, stride = 0;
  for (int i = 0; i < gxfifo::kNumAttrs; i++) {
    if (i < gxfifo::kAttrPos) offset += attrs[i].size;
    stride += attrs[i].size;
  }
  auto f32 = [](uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
  };
  const uint32_t* m = &m_xfMtx[(m_mtxIdxA & 63) * 4];
  float p0 = f32(m_proj[0]), p1 = f32(m_proj[1]);
  int comps = field(vatA, 1, 0) ? 3 : 2;
  float lo = 1e9f, hi = -1e9f;
  const uint8_t* v = p + 3 + offset;
  for (uint32_t n = 0; n < count; n++, v += stride) {
    float x = f32(be32(v)), y = f32(be32(v + 4)), z = comps == 3 ? f32(be32(v + 8)) : 0.0f;
    float ex = f32(m[0]) * x + f32(m[1]) * y + f32(m[2]) * z + f32(m[3]);
    float cx = p0 * ex + p1;
    lo = cx < lo ? cx : lo;
    hi = cx > hi ? cx : hi;
  }
  static const bool dbg = getenv("BLITZ_GX_FSDEBUG") != nullptr;
  if (dbg && count >= 4 && !clearLike && hi - lo > 1.5f && (m_bp41 & 0xFFFF) != 0x04BD) {
    static int logged = 0;
    if (logged++ < 300)
      LOG_INFO("2D draw n=%u blend %04X 3d=%d clear=%d mtx %u x %.3f..%.3f p0 %f p1 %f m0 %f m3 %f", count, m_bp41 & 0xFFFF,
               m_frameHas3d, clearLike, m_mtxIdxA & 63, lo, hi, p0, p1, f32(m[0]), f32(m[3]));
  }
  return lo <= -0.98f && hi >= 0.98f;
}

// Re-emits the projection with its horizontal terms scaled by `scale` (1 = the game's own).
// Aurora only accepts the whole 7-register block.
void Bridge::putProjX(float scale) {
  put8(0x10);
  put32(0x00061020);
  for (int i = 0; i < 7; i++) {
    uint32_t bits = m_proj[i];
    if (i < 2) {
      float f;
      memcpy(&f, &bits, 4);
      f *= scale;
      memcpy(&bits, &f, 4);
    }
    put32(bits);
  }
}

void Bridge::indexedXf(const uint8_t* p) {
  int arr = 12 + (p[0] - 0x20) / 8;
  uint32_t index = (uint32_t)p[1] << 8 | p[2];
  uint32_t len = (p[3] >> 4) + 1;
  ensureArray(arr, index * m_arrays[arr].stride + len * 4);
  static const bool hazard = getenv("BLITZ_GX_HAZARD") != nullptr;
  if (hazard) noteUse(arr, m_arrays[arr].base + index * m_arrays[arr].stride, len * 4);
  uint32_t addr = (uint32_t)(p[3] & 0x0F) << 8 | p[4];
  const uint8_t* src = hostPhys(m_arrays[arr].base + index * m_arrays[arr].stride);
  for (uint32_t k = 0; k < len && addr + k < 0x100; k++) m_xfMtx[addr + k] = be32(src + 4 * k);
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

// Widescreen: the content framebuffer has the window's aspect (Aurora STRETCH policy). In a match
// the game's 4:3 projections are narrowed horizontally by (4/3) / aspect: perspective (3D) sees
// more to the sides (Hor+) and orthographic (2D) ends up centered at 4:3, or stays stretched if
// the user prefers. In the front-end menus (stretch_menus) the whole 4:3 frame is stretched to the
// window instead and zoomed vertically over the margins the menu art leaves for TV overscan; 2D
// and 3D stay aligned (team select draws 3D players inside 2D frames).
void Bridge::xf(const uint8_t* p, uint32_t size) {
  uint32_t header = be32(p + 1);
  uint32_t first = header & 0xFFFF, count = (header >> 16) + 1;
  // BLITZ_GX_XFAND=<addr>:<mask>[,...]: AND XF register values with a mask (debugging).
  static const char* xfAnd = getenv("BLITZ_GX_XFAND");
  if (xfAnd) {
    static std::vector<uint8_t> patched;
    patched.assign(p, p + size);
    for (const char* q = xfAnd; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) {
      unsigned addr = 0, mask = 0;
      if (sscanf(q, "%x:%x", &addr, &mask) != 2 || addr < first || addr >= first + count) continue;
      uint8_t* w = patched.data() + 5 + 4 * (addr - first);
      uint32_t val = be32(w) & mask;
      w[0] = (uint8_t)(val >> 24), w[1] = (uint8_t)(val >> 16), w[2] = (uint8_t)(val >> 8), w[3] = (uint8_t)val;
    }
    p = patched.data();
  }
  for (uint32_t k = 0; k < count && 5 + 4 * k + 4 <= size; k++) {
    uint32_t addr = first + k, val = be32(p + 5 + 4 * k);
    if (addr < 0x100) m_xfMtx[addr] = val;
    else if (addr >= 0x1020 && addr <= 0x1026) m_proj[addr - 0x1020] = val;
    else if (addr == 0x1018) m_mtxIdxA = val;
  }
  const settings::Settings& st = settings::get();
  bool touchesProj = first <= 0x1026 && first + count > 0x1020;
  if (!touchesProj || !st.widescreen) {
    if (touchesProj && first + count > 0x1026) m_projType = be32(p + 5 + 4 * (0x1026 - first));
    putRaw(p, size);
    return;
  }
  if (first <= 0x1026 && first + count > 0x1026) m_projType = be32(p + 5 + 4 * (0x1026 - first));
  float kx = (4.0f / 3.0f) / settings::g_displayAspect.load(), ky = 1.0f;
  if (m_menuMode) kx = 1.0f, ky = kMenuZoom;
  else if (m_projType == 1 && st.hud == settings::Hud2D::Stretched) kx = 1.0f;
  uint8_t buf[5 + 4 * 64];
  if (size > sizeof(buf) || (kx == 1.0f && ky == 1.0f)) {
    putRaw(p, size);
    return;
  }
  memcpy(buf, p, size);
  for (uint32_t reg : {0x1020u, 0x1021u, 0x1022u, 0x1023u}) {
    if (reg < first || reg >= first + count) continue;
    uint8_t* w = buf + 5 + 4 * (reg - first);
    uint32_t bits = be32(w);
    float f;
    memcpy(&f, &bits, 4);
    f *= reg < 0x1022u ? kx : ky;
    memcpy(&bits, &f, 4);
    w[0] = (uint8_t)(bits >> 24);
    w[1] = (uint8_t)(bits >> 16);
    w[2] = (uint8_t)(bits >> 8);
    w[3] = (uint8_t)bits;
  }
  putRaw(buf, size);
}

void Bridge::command(const uint8_t* p, uint32_t size) {
  uint8_t op = p[0];
  if (dropped(p)) return;
  switch (op) {
  case 0x10: xf(p, size); return;
  case 0x61: bp(be32(p + 1), p); return;
  case 0x08: cp(p[1], be32(p + 2), p); return;
  case 0x20:
  case 0x28:
  case 0x30:
  case 0x38: indexedXf(p); return;
  case 0x44: return;  // not understood by Aurora; no visible effect
  case 0x48:
    m_invalidates++;
    for (const Array& a : m_arrays) m_invalidatedBytes += a.emittedSize;
    putRaw(p, size);
    return;
  default:
    if (op >= 0x80) draw(p, size);
    else putRaw(p, size);
  }
}

void Bridge::endFrame() {
  if (m_arrayBytes > 0x02000000) {
    static int reported = 0;
    if (reported++ < 20)
      LOG_ERROR("GX: frame uploads %llu MB of vertex arrays in %llu loads", (unsigned long long)(m_arrayBytes >> 20),
                (unsigned long long)m_arrayLoads);
  }
  if (m_invalidatedBytes > 0x02000000) {
    static int reported2 = 0;
    if (reported2++ < 20)
      LOG_ERROR("GX: %llu vertex cache invalidations, %llu MB of arrays to re-upload",
                (unsigned long long)m_invalidates, (unsigned long long)(m_invalidatedBytes >> 20));
  }
  m_arrayBytes = m_arrayLoads = 0;
  m_invalidates = m_invalidatedBytes = 0;
  if (m_out.size() > 0x04000000) LOG_ERROR("GX: frame stream is %zu bytes", m_out.size());
  static uint32_t frameNo = 0;
  // BLITZ_GX_DUMP=<frame>[,<count>[,<step>]]: save translated streams (+ textures of the first).
  static const char* dump = getenv("BLITZ_GX_DUMP");
  static uint32_t dumpFirst = 0, dumpCount = 1, dumpStep = 1;
  if (dump && !dumpFirst && dump[0]) {
    int a = 0, b = 1, c = 1;
    sscanf(dump, "%d,%d,%d", &a, &b, &c);
    dumpFirst = (uint32_t)a;
    dumpCount = b > 0 ? (uint32_t)b : 1;
    dumpStep = c > 0 ? (uint32_t)c : 1;
  }
  auto dumpThis = [](uint32_t n) {
    return dump && n >= dumpFirst && (n - dumpFirst) % dumpStep == 0 && (n - dumpFirst) / dumpStep < dumpCount;
  };
  if (dumpThis(frameNo)) {
    char name[64];
    snprintf(name, sizeof(name), "build/gxframe_%u.bin", frameNo);
    if (FILE* f = fopen(name, "wb")) {
      fwrite(m_out.data(), 1, m_out.size(), f);
      fclose(f);
      LOG_INFO("wrote %s (%zu bytes)", name, m_out.size());
    }
    // ...and guest MEM1 at the end of the frame (offline geometry checks).
    snprintf(name, sizeof(name), "build/gxmem_%u.bin", frameNo);
    if (FILE* f = fopen(name, "wb")) {
      fwrite(hostPhys(0), 1, guest::kMem1Size, f);
      fclose(f);
    }
  }
  // BLITZ_GX_RING=<n>: keep the last n streams; creating build/gxdump.trigger writes them out
  // (build/ring/gxframe_N.bin) and dumps the next frame's textures. Catches rare effects.
  static const int ringSize = getenv("BLITZ_GX_RING") ? atoi(getenv("BLITZ_GX_RING")) : 0;
  bool ringTriggered = false;
  if (ringSize > 0) {
    m_ring.emplace_back(frameNo, m_out);
    if ((int)m_ring.size() > ringSize) m_ring.pop_front();
    if (frameNo % 10 == 0 && std::filesystem::exists("build/gxdump.trigger")) {
      std::error_code ec;
      std::filesystem::remove("build/gxdump.trigger", ec);
      std::filesystem::create_directories("build/ring", ec);
      for (auto& [n, data] : m_ring) {
        char name[64];
        snprintf(name, sizeof(name), "build/ring/gxframe_%u.bin", n);
        if (FILE* f = fopen(name, "wb")) {
          fwrite(data.data(), 1, data.size(), f);
          fclose(f);
        }
      }
      LOG_INFO("wrote GX ring: frames %u..%u", m_ring.front().first, m_ring.back().first);
      ringTriggered = true;
    }
  }
  if (!m_uses.empty()) {
    for (auto& [key, use] : m_uses)
      if (XXH3_64bits(hostPhys((uint32_t)(key >> 24)), (size_t)(key & 0xFFFFFF)) != use.hash) m_hazardLate[use.kind]++;
    m_uses.clear();
    std::string line;
    for (int k = 0; k < 18; k++)
      if (m_hazardMid[k] || m_hazardLate[k]) line += " " + std::to_string(k) + ":" + std::to_string(m_hazardMid[k]) + "/" + std::to_string(m_hazardLate[k]);
    if (!line.empty()) LOG_INFO("GX hazards frame %u (kind:changed between uses/after last use):%s", frameNo, line.c_str());
    for (int k = 0; k < 18; k++) m_hazardMid[k] = m_hazardLate[k] = 0;
  }
  if (!m_blendHist.empty()) {
    std::string line;
    for (auto& [k, n] : m_blendHist) {
      char b[64];
      snprintf(b, sizeof(b), " cm0=%04X ac=%06X x%u", (unsigned)(k >> 32), (unsigned)(k & 0xFFFFFF), n);
      line += b;
    }
    LOG_INFO("blend frame %u pe=%06X cm1=%06X:%s", frameNo, m_bp43, m_bp42, line.c_str());
    m_blendHist.clear();
  }
  frameNo++;
  hw::g_framesBuilt.store(frameNo);
  static auto lastEnd = std::chrono::steady_clock::now();
  static double sumWait = 0, sumGuest = 0;
  auto t0 = std::chrono::steady_clock::now();
  sumGuest += std::chrono::duration<double, std::milli>(t0 - lastEnd).count();
  m_dumpTextures = (dump && frameNo == dumpFirst) || ringTriggered;
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
  // Draws wait for their pipeline to compile. With Aurora's default (skip a draw until its pipeline
  // is ready) the jersey number/collar panels of players never appeared. BLITZ_DRAWS_ASYNC=1 restores it.
  static const bool async = getenv("BLITZ_DRAWS_ASYNC") != nullptr;
  if (!async) {
    aurora(GX_AURORA_SET_DRAWS_BLOCKING);
    put8(1);
  }
  // Aurora starts each frame with fresh caches.
  // Aurora has consumed the frame: its snapshots can be reused.
  m_arena.reset();
  m_drawInFrame = 0;
  // The quarter-break loading screen (no 3D) runs while the game still reports the match: a frame
  // without 3D there is a menu too.
  const settings::Settings& st = settings::get();
  m_menuMode = st.widescreen && st.stretchMenus &&
               (!widescreen::inMatch() || (widescreen::inQuarterBreak() && !m_frameHas3d));
  m_frameHas3d = false;
  for (TlutCopy& c : m_tlutCopies) c.data = nullptr;
  for (Array& a : m_arrays) a.snapshot = nullptr, a.emittedSize = 0;
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
