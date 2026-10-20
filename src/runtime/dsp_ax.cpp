#include "dsp_ax.h"

#include "hw.h"
#include "log.h"
#include "memory.h"

#include <cstring>

namespace ax {

namespace {

constexpr int kFrame = 160;    // samples per 5 ms frame at 32 kHz
constexpr int kSubframes = 5;  // parameter updates happen once per 1 ms (32 samples)

// Parameter block byte offsets (MusyX _PB, same layout as AX).
enum : uint32_t {
  PB_NEXT = 0x00, PB_SRC_SELECT = 0x08, PB_COEF_SELECT = 0x0A, PB_MIXER_CTRL = 0x0C, PB_STATE = 0x0E,
  PB_LOOP_TYPE = 0x10,
  PB_MIX = 0x12,  // 9 (volume, delta) pairs, see kBusOrder
  PB_UPD_NUM = 0x44, PB_UPD_DATA = 0x4E,
  PB_DPOP = 0x52,  // L, AL, BL, R, AR, BR, S, AS, BS
  PB_VE_VOL = 0x64, PB_VE_DELTA = 0x66,
  PB_LOOP_FLAG = 0x6E, PB_FORMAT = 0x70, PB_LOOP_ADDR = 0x72, PB_END_ADDR = 0x76, PB_CUR_ADDR = 0x7A,
  PB_ADPCM_COEFS = 0x7E, PB_ADPCM_GAIN = 0x9E, PB_ADPCM_PS = 0xA0, PB_ADPCM_YN1 = 0xA2, PB_ADPCM_YN2 = 0xA4,
  PB_SRC_RATIO = 0xA6, PB_SRC_FRAC = 0xAA, PB_SRC_LAST = 0xAC,
  PB_LOOP_PS = 0xB4, PB_LOOP_YN1 = 0xB6, PB_LOOP_YN2 = 0xB8,
  PB_SIZE = 0xBC,
};

enum Bus { L, R, S, AL, AR, AS, BL, BR, BS, kBuses };

// Order of the (volume, delta) pairs in _PBMIX.
constexpr Bus kMixOrder[9] = {L, R, AL, AR, BL, BR, BS, S, AS};
// Order of the depop words in _PBDPOP.
constexpr Bus kDpopOrder[9] = {L, AL, BL, R, AR, BR, S, AS, BS};

int32_t g_bus[kBuses][kFrame];  // the studio being processed
int32_t g_out[3][kFrame];       // master output accumulator (L, R, S)
uint32_t g_pb = 0;

int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

uint32_t rd32pair(uint32_t a) { return (uint32_t)guest::rd16(a) << 16 | guest::rd16(a + 2); }
void wr32pair(uint32_t a, uint32_t v) {
  guest::wr16(a, (uint16_t)(v >> 16));
  guest::wr16(a + 2, (uint16_t)v);
}

uint8_t aram8(uint32_t byteAddr) { return hw::g_aram[byteAddr & (hw::kAramSize - 1)]; }

// Host copy of one parameter block while it's being rendered.
struct Voice {
  uint16_t w[PB_SIZE / 2];
  uint16_t& at(uint32_t off) { return w[off / 2]; }
  uint32_t at32(uint32_t off) { return (uint32_t)w[off / 2] << 16 | w[off / 2 + 1]; }
  void set32(uint32_t off, uint32_t v) {
    w[off / 2] = (uint16_t)(v >> 16);
    w[off / 2 + 1] = (uint16_t)v;
  }
};

void loadPb(Voice& v, uint32_t addr) {
  for (uint32_t i = 0; i < PB_SIZE / 2; i++) v.w[i] = guest::rd16(addr + 2 * i);
}
void storePb(const Voice& v, uint32_t addr) {
  for (uint32_t i = 0; i < PB_SIZE / 2; i++) guest::wr16(addr + 2 * i, v.w[i]);
}

// Reads the next source sample, advancing the address with loop/end handling.
// Returns false when the voice has ended (and the sample is silence).
struct Source {
  Voice& v;
  uint32_t cur, end, loop;
  uint16_t format, ps;
  int16_t yn1, yn2;
  bool stopped = false;

  explicit Source(Voice& voice) : v(voice) {
    cur = v.at32(PB_CUR_ADDR);
    end = v.at32(PB_END_ADDR);
    loop = v.at32(PB_LOOP_ADDR);
    format = v.at(PB_FORMAT);
    ps = v.at(PB_ADPCM_PS);
    yn1 = (int16_t)v.at(PB_ADPCM_YN1);
    yn2 = (int16_t)v.at(PB_ADPCM_YN2);
  }

  void save() {
    v.set32(PB_CUR_ADDR, cur);
    v.at(PB_ADPCM_PS) = ps;
    v.at(PB_ADPCM_YN1) = (uint16_t)yn1;
    v.at(PB_ADPCM_YN2) = (uint16_t)yn2;
  }

  int16_t decode() {
    switch (format) {
    case 0x00: {  // DSP ADPCM: addresses in nibbles, 2-nibble header per 16-nibble frame
      if ((cur & 15) == 0) {
        ps = aram8(cur >> 1);
        cur += 2;
      }
      uint8_t b = aram8(cur >> 1);
      int nib = (cur & 1) ? (b & 15) : (b >> 4);
      if (nib >= 8) nib -= 16;
      int coef = (ps >> 4) & 7;
      int32_t c1 = (int16_t)v.at(PB_ADPCM_COEFS + coef * 4), c2 = (int16_t)v.at(PB_ADPCM_COEFS + coef * 4 + 2);
      int32_t val = ((nib << (ps & 15)) << 11) + c1 * yn1 + c2 * yn2;
      int16_t s = clamp16((val + 1024) >> 11);
      yn2 = yn1;
      yn1 = s;
      return s;
    }
    case 0x0A: {  // PCM16: addresses in samples
      uint32_t a = cur * 2;
      return (int16_t)(aram8(a) << 8 | aram8(a + 1));
    }
    case 0x19:  // PCM8: addresses in bytes
      return (int16_t)((int8_t)aram8(cur) * 256);
    default: return 0;
    }
  }

  int16_t next() {
    if (stopped) return 0;
    int16_t s = decode();
    if (cur == end) {
      if (v.at(PB_LOOP_FLAG)) {
        cur = loop;
        if (format == 0x00) {
          if (v.at(PB_LOOP_TYPE) == 0) {
            ps = v.at(PB_LOOP_PS);
            yn1 = (int16_t)v.at(PB_LOOP_YN1);
            yn2 = (int16_t)v.at(PB_LOOP_YN2);
          } else {
            ps = aram8((cur & ~15u) >> 1);  // streams keep their history; take the frame header
          }
        }
      } else {
        stopped = true;
      }
    } else {
      cur++;
    }
    return s;
  }
};

void applyUpdates(Voice& v, int subframe) {
  uint32_t data = v.at32(PB_UPD_DATA);
  uint32_t skip = 0;
  for (int k = 0; k < subframe; k++) skip += v.at(PB_UPD_NUM + 2 * k);
  uint32_t n = v.at(PB_UPD_NUM + 2 * subframe);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t e = data + 4 * (skip + i);
    uint16_t word = guest::rd16(e), value = guest::rd16(e + 2);
    if (word < PB_SIZE / 2) v.w[word] = value;
  }
}

uint32_t g_playing = 0;  // voices rendered in the current list (debug stats)

void renderVoice(uint32_t pbAddr) {
  Voice v;
  loadPb(v, pbAddr);
  if (v.at(PB_STATE) != 1) return;
  g_playing++;

  Source src(v);
  uint32_t ratio = v.at32(PB_SRC_RATIO);
  uint32_t frac = v.at(PB_SRC_FRAC);
  int16_t hist[4];
  for (int i = 0; i < 4; i++) hist[i] = (int16_t)v.at(PB_SRC_LAST + 2 * i);
  uint16_t srcSelect = v.at(PB_SRC_SELECT);

  int32_t last[kBuses] = {};
  for (int sub = 0; sub < kSubframes; sub++) {
    applyUpdates(v, sub);
    if (sub > 0 && v.at(PB_STATE) != 1) break;
    if (sub > 0) ratio = v.at32(PB_SRC_RATIO);
    for (int n = sub * 32; n < sub * 32 + 32; n++) {
      int32_t s;
      if (srcSelect == 2) {
        s = src.next();
      } else {
        // 16.16 position: consume input samples into the history, then interpolate.
        frac += ratio;
        while (frac >= 0x10000) {
          frac -= 0x10000;
          hist[0] = hist[1];
          hist[1] = hist[2];
          hist[2] = hist[3];
          hist[3] = src.next();
        }
        float t = frac / 65536.0f;
        if (srcSelect == 1) {
          s = (int32_t)(hist[2] + (hist[3] - hist[2]) * t);
        } else {
          // Catmull-Rom between hist[1] and hist[2] (one sample of latency) stands in for
          // the microcode's 4-tap polyphase filter.
          float p0 = hist[0], p1 = hist[1], p2 = hist[2], p3 = hist[3];
          float y = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
          s = clamp16((int32_t)y);
        }
      }
      // Envelope.
      int32_t ve = v.at(PB_VE_VOL);
      s = (s * ve) >> 15;
      ve += (int16_t)v.at(PB_VE_DELTA);
      v.at(PB_VE_VOL) = (uint16_t)(ve < 0 ? 0 : ve > 0x7FFF ? 0x7FFF : ve);
      // Mix into the buses with per-sample volume ramps.
      for (int m = 0; m < 9; m++) {
        uint16_t& vol = v.at(PB_MIX + 4 * m);
        int16_t delta = (int16_t)v.at(PB_MIX + 4 * m + 2);
        if (vol || delta) {
          int32_t out = (s * (int32_t)vol) >> 15;
          g_bus[kMixOrder[m]][n] += out;
          last[kMixOrder[m]] = out;
          int32_t nv = (int32_t)vol + delta;
          vol = (uint16_t)(nv < 0 ? 0 : nv > 0xFFFF ? 0xFFFF : nv);
        }
      }
    }
  }
  if (src.stopped) v.at(PB_STATE) = 0;
  src.save();
  v.at(PB_SRC_FRAC) = (uint16_t)frac;
  for (int i = 0; i < 4; i++) v.at(PB_SRC_LAST + 2 * i) = (uint16_t)hist[i];
  for (int d = 0; d < 9; d++) v.at(PB_DPOP + 2 * d) = (uint16_t)clamp16(last[kDpopOrder[d]]);
  // The CPU owns the update list and the voice links; write back everything else.
  Voice mem;
  loadPb(mem, pbAddr);
  for (uint32_t off = PB_SRC_SELECT; off < PB_SIZE; off += 2)
    if (off < PB_UPD_NUM || off > PB_UPD_DATA + 2) mem.at(off) = v.at(off);
  storePb(mem, pbAddr);
}

void processVoices() {
  uint32_t pb = g_pb;
  for (int guard = 0; pb && guard < 256; guard++) {
    renderVoice(pb & 0x01FFFFFF | 0x80000000);
    pb = rd32pair((pb & 0x01FFFFFF | 0x80000000) + PB_NEXT);
  }
}

// Bus block in memory: L, R, S planes of 160 big-endian s32.
void readLrs(uint32_t addr, int32_t out[3][kFrame]) {
  for (int c = 0; c < 3; c++)
    for (int n = 0; n < kFrame; n++) out[c][n] = (int32_t)guest::rd32(addr + 4 * (c * kFrame + n));
}
void writeLrs(uint32_t addr, int32_t* l, int32_t* r, int32_t* s) {
  int32_t* planes[3] = {l, r, s};
  for (int c = 0; c < 3; c++)
    for (int n = 0; n < kFrame; n++) guest::wr32(addr + 4 * (c * kFrame + n), (uint32_t)planes[c][n]);
}

void setup(uint32_t spb) {
  // 9 depop ramps (start s32, delta s16) seed the buses: L R S AL AR AS BL BR BS.
  for (int b = 0; b < kBuses; b++) {
    uint32_t e = spb + 6 * b;
    int32_t start = (int32_t)rd32pair(e);
    int16_t delta = (int16_t)guest::rd16(e + 4);
    for (int n = 0; n < kFrame; n++) g_bus[b][n] = start + delta * n;
  }
}

void auxSend(uint32_t outAddr, uint32_t inAddr, Bus l, Bus r, Bus s) {
  if (outAddr) writeLrs(outAddr, g_bus[l], g_bus[r], g_bus[s]);
  if (inAddr) {
    int32_t in[3][kFrame];
    readLrs(inAddr, in);
    for (int n = 0; n < kFrame; n++) {
      g_bus[L][n] += in[0][n];
      g_bus[R][n] += in[1][n];
      g_bus[S][n] += in[2][n];
    }
  }
}

void output(uint32_t surround, uint32_t dest) {
  (void)surround;
  for (int n = 0; n < kFrame; n++) {
    // Fold surround into the stereo pair (matrix-encoded, Dolby-style: out of phase).
    int32_t s = g_out[2][n] * 181 / 256;
    int16_t l = clamp16(g_out[0][n] - s), r = clamp16(g_out[1][n] + s);
    // AI DMA frames are right channel first.
    guest::wr16(dest + 4 * n, (uint16_t)r);
    guest::wr16(dest + 4 * n + 2, (uint16_t)l);
  }
}

uint32_t phys(uint32_t a) { return (a & 0x01FFFFFF) | 0x80000000; }

}  // namespace

void processCommandList(uint32_t list) {
  static uint32_t lists = 0, voices = 0;
  if (++lists % 400 == 0) {
    LOG_DEBUG("AX: %u lists, %.1f voices per list", lists, voices / 400.0);
    voices = 0;
  }
  struct Count {
    ~Count() {
      voices += g_playing;
      g_playing = 0;
    }
  } count;
  uint32_t p = phys(list);
  for (int guard = 0; guard < 4096; guard++) {
    uint16_t cmd = guest::rd16(p);
    auto arg32 = [&](int i) { return rd32pair(p + 2 + 4 * i); };
    switch (cmd) {
    case 0:  // SETUP spb
      setup(phys(arg32(0)));
      p += 6;
      break;
    case 1: {  // DL_AND_VOL_MIX addr, vol, volA, volB: route another studio's output in
      int32_t in[3][kFrame];
      readLrs(phys(arg32(0)), in);
      int32_t vol = guest::rd16(p + 6), volA = guest::rd16(p + 8), volB = guest::rd16(p + 10);
      for (int c = 0; c < 3; c++)
        for (int n = 0; n < kFrame; n++) {
          g_bus[L + c][n] += (in[c][n] * vol) >> 15;
          g_bus[AL + c][n] += (in[c][n] * volA) >> 15;
          g_bus[BL + c][n] += (in[c][n] * volB) >> 15;
        }
      p += 12;
      break;
    }
    case 2:  // PB_ADDR
      g_pb = arg32(0);
      p += 6;
      break;
    case 3:  // PROCESS
      processVoices();
      p += 2;
      break;
    case 4:  // MIX_AUXA out, in
      auxSend(arg32(0) ? phys(arg32(0)) : 0, arg32(1) ? phys(arg32(1)) : 0, AL, AR, AS);
      p += 10;
      break;
    case 5:   // MIX_AUXB out, in
    case 16:  // MIX_AUXB_LR
      auxSend(arg32(0) ? phys(arg32(0)) : 0, arg32(1) ? phys(arg32(1)) : 0, BL, BR, BS);
      p += 10;
      break;
    case 6:  // UPLOAD_LRS addr
      writeLrs(phys(arg32(0)), g_bus[L], g_bus[R], g_bus[S]);
      p += 6;
      break;
    case 9: {  // add a master studio's uploaded output to the final mix
      int32_t in[3][kFrame];
      readLrs(phys(arg32(0)), in);
      for (int c = 0; c < 3; c++)
        for (int n = 0; n < kFrame; n++) g_out[c][n] += in[c][n];
      p += 6;
      break;
    }
    case 13:  // MORE addr (+ size): continue at the next list chunk
      p = phys(arg32(0));
      break;
    case 14:  // OUTPUT surround, dest
      output(phys(arg32(0)), phys(arg32(1)));
      p += 10;
      break;
    case 15:  // END
      return;
    case 17:  // SET_OPPOSITE_LR: start of the final mix
      memset(g_out, 0, sizeof(g_out));
      p += 6;
      break;
    case 18:  // compressor table setup: not applied
      p += 10;
      break;
    default:
      LOG_ERROR("AX: unknown command %u in list %08X", cmd, list);
      return;
    }
  }
  LOG_ERROR("AX: command list %08X did not end", list);
}

}  // namespace ax
