// Low-level hardware register models for SDK code that stays recompiled rather than
// HLE'd: EXI (transfers complete instantly, devices answer zeros) and VI (registers +
// the vertical retrace interrupt, which runs the game's own __VIRetraceHandler).
#include "hw.h"

#include "audio_out.h"
#include "boot.h"
#include "dsp_ax.h"
#include "log.h"
#include "memory.h"
#include "pad_input.h"
#include "guest_sched.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace hw {

std::atomic<bool> g_paused{false};

// --- EXI -----------------------------------------------------------------------
static uint32_t g_exi[3][5];  // per channel: CSR, MAR, LENGTH, CR, DATA

static uint32_t exiRead(uint32_t off, int size) {
  (void)size;
  uint32_t rel = off - 0x6800;
  uint32_t ch = rel / 0x14, reg = (rel % 0x14) / 4;
  if (ch >= 3) return 0;
  return g_exi[ch][reg];
}

static void exiWrite(uint32_t off, uint32_t v, int size) {
  (void)size;
  uint32_t rel = off - 0x6800;
  uint32_t ch = rel / 0x14, reg = (rel % 0x14) / 4;
  if (ch >= 3) return;
  if (reg == 3 && (v & 1)) {
    // Transfer: immediate reads return zeros; DMA reads clear the target buffer.
    bool dma = v & 2;
    uint32_t rw = (v >> 2) & 3;
    if (dma && rw == 0) memset(guest::ptr(0x80000000 | g_exi[ch][1]), 0, g_exi[ch][2]);
    if (!dma && rw == 0) g_exi[ch][4] = 0;
    v &= ~1u;
    g_exi[ch][0] |= 0x8;  // TCINT
  }
  if (reg == 0) {
    // Writing 1 to interrupt status bits clears them.
    uint32_t ack = v & 0x80A;
    g_exi[ch][0] = (v & ~0x80Au) | (g_exi[ch][0] & 0x80A & ~ack);
    return;
  }
  g_exi[ch][reg] = v;
}

static const guest::MmioDevice kExi = {exiRead, exiWrite};

// --- VI ------------------------------------------------------------------------
// Registers are 16-bit, stored big-endian at their offsets.
static uint8_t g_vi[0x100];
static std::atomic<uint32_t> g_viLine{0};
constexpr int kInterruptVI = 24;  // __OS_INTERRUPT_PI_VI

static uint16_t viReg(uint32_t off) { return (uint16_t)(g_vi[off] << 8 | g_vi[off + 1]); }
static void setViReg(uint32_t off, uint16_t v) {
  g_vi[off] = (uint8_t)(v >> 8);
  g_vi[off + 1] = (uint8_t)v;
}

static uint32_t viRead(uint32_t off, int size) {
  off &= 0xFF;
  if (off == 0x2C || off == 0x2E) {
    // VCT/HCT: advance so polling loops make progress.
    uint32_t l = g_viLine.fetch_add(1) % 525;
    setViReg(0x2C, (uint16_t)(l + 1));
    setViReg(0x2E, 1);
  }
  if (size == 4) return (uint32_t)viReg(off) << 16 | viReg(off + 2);
  if (size == 2) return viReg(off);
  return g_vi[off];
}

static void viWrite(uint32_t off, uint32_t v, int size) {
  off &= 0xFF;
  if (size == 4) {
    setViReg(off, (uint16_t)(v >> 16));
    setViReg(off + 2, (uint16_t)v);
  } else if (size == 2) {
    setViReg(off, (uint16_t)v);
  } else {
    g_vi[off] = (uint8_t)v;
  }
}

static const guest::MmioDevice kVi = {viRead, viWrite};

uint32_t viFramebufferAddress() {
  // TFBL (0x1C): top field base, address >> 5 when POFF (bit 28) is set.
  uint32_t v = (uint32_t)viReg(0x1C) << 16 | viReg(0x1E);
  uint32_t addr = v & 0x00FFFFFF;
  if (v & (1u << 28)) addr <<= 5;
  return addr;
}

// BLITZ_XFB_DUMP=<retrace>[,<count>]: write the scanned-out XFB (YUYV) as PPM files.
static void maybeDumpXfb(uint32_t retrace) {
  static const char* env = getenv("BLITZ_XFB_DUMP");
  if (!env) return;
  static uint32_t first = (uint32_t)atoi(env);
  static uint32_t count = strchr(env, ',') ? (uint32_t)atoi(strchr(env, ',') + 1) : 1;
  if (retrace < first || retrace >= first + count) return;
  uint32_t addr = viFramebufferAddress();
  const uint32_t w = 640, h = 480;
  char name[64];
  snprintf(name, sizeof(name), "build/xfb_%05u.ppm", retrace);
  FILE* f = fopen(name, "wb");
  if (!f) return;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  const uint8_t* src = guest::g_mem + (addr & guest::kMemMask);
  auto clamp = [](int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x += 2) {
      const uint8_t* p = src + (y * w + x) * 2;
      int y0 = p[0] - 16, u = p[1] - 128, y1 = p[2] - 16, v = p[3] - 128;
      for (int yy : {y0, y1}) {
        uint8_t rgb[3] = {clamp((298 * yy + 409 * v + 128) >> 8), clamp((298 * yy - 100 * u - 208 * v + 128) >> 8),
                          clamp((298 * yy + 516 * u + 128) >> 8)};
        fwrite(rgb, 1, 3, f);
      }
    }
  }
  fclose(f);
  LOG_INFO("dumped XFB %08X at retrace %u to %s", addr, retrace, name);
}

std::atomic<uint32_t> g_framesBuilt{0};
static uint32_t g_fastBootUntil = 0;

void startFastBoot(uint32_t untilFrame, uint32_t factor) {
  g_fastBootUntil = untilFrame;
  guest::setTimeWarp(factor);
}

static void viRetraceThread() {
  using namespace std::chrono;
  const auto period = nanoseconds(1001000000ll / 60);  // 59.94 Hz
  auto next = steady_clock::now() + period;
  uint32_t context = guest::allocReserved(0x2C8);
  for (;;) {
    std::this_thread::sleep_until(next);
    static uint32_t retrace = 0;
    // Fast boot: the timed boot screens (legal, licenses, memory card) run at g_turboFactor x
    // speed until the retrace count reaches g_turboUntil.
    uint32_t turbo = guest::timeWarp();
    if (turbo > 1 && g_framesBuilt.load() >= g_fastBootUntil) {
      guest::setTimeWarp(1);
      turbo = 1;
      LOG_INFO("fast boot done");
    }
    next += period / turbo;
    if (turbo > 1 && steady_clock::now() > next + milliseconds(50)) next = steady_clock::now();
    if (g_paused.load()) continue;  // the guest is held at a frame handoff (menu open)
    sched::postIrq([context](guest::Ctx& c) {
      maybeDumpXfb(retrace++);
      // BLITZ_WATCH=addr[+words],...: log guest words when they change (checked once per retrace).
      static const char* watch = getenv("BLITZ_WATCH");
      if (watch) {
        static uint32_t last[256];
        int k = 0;
        for (const char* q = watch; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) {
          char* end = nullptr;
          uint32_t a = (uint32_t)strtoul(q, &end, 16), n = *end == '+' ? (uint32_t)strtoul(end + 1, nullptr, 10) : 1;
          for (uint32_t w = 0; w < n && k < 256; w++, k++) {
            uint32_t v = guest::rd32(a + 4 * w);
            if (v != last[k]) LOG_INFO("watch %08X: %08X -> %08X at retrace %u", a + 4 * w, last[k], v, retrace);
            last[k] = v;
          }
        }
      }
      padinput::tick();
      uint32_t handler = sched::callGuest(c, kGetInterruptHandler, {kInterruptVI});
      if (!handler) return;
      setViReg(0x30, viReg(0x30) | 0x8000);  // DI0: display interrupt 0 fired
      sched::callGuest(c, handler, {kInterruptVI, context});
    });
  }
}

// --- DSP interface: mailboxes, control/status, ARAM DMA ------------------------------
// The DSP itself is not emulated (audio is handled on the host); ARAM DMA is real so
// game data staged in ARAM survives the round trip.
uint8_t* g_aram = nullptr;
static uint16_t g_dsp[0x40];  // 16-bit registers by (off & 0x7F) / 2
constexpr int kInterruptDspAram = 6;  // __OS_INTERRUPT_DSP_ARAM

static uint32_t dsp32(uint32_t off) { return (uint32_t)g_dsp[off / 2] << 16 | g_dsp[off / 2 + 1]; }

static void aramDma() {
  uint32_t mm = dsp32(0x20) & 0x01FFFFE0;
  uint32_t ar = dsp32(0x24) & (kAramSize - 1) & ~31u;
  uint32_t cnt = dsp32(0x28);
  bool toMain = cnt & 0x80000000u;
  uint32_t len = cnt & 0x7FFFFFE0u;
  if (ar + len > kAramSize) len = kAramSize - ar;
  if (toMain) memcpy(guest::ptr(0x80000000 | mm), g_aram + ar, len);
  else memcpy(g_aram + ar, guest::ptr(0x80000000 | mm), len);
  g_dsp[0x0A / 2] |= 0x20;  // ARINT
  g_dsp[0x28 / 2] = 0;
  g_dsp[0x2A / 2] = 0;
  sched::postIrq([](guest::Ctx& c) {
    uint32_t handler = sched::callGuest(c, kGetInterruptHandler, {kInterruptDspAram});
    if (handler) sched::callGuest(c, handler, {kInterruptDspAram, 0});
  });
}

// Stand-in DSP: consumes every mail at once and answers the SDK task-boot handshake.
// The MusyX microcode protocol is handled later by the host audio mixer.
static std::mutex g_mailMutex;
static std::deque<uint32_t> g_mailFromDsp;
static uint32_t g_lastMailToDsp = 0;
static int g_mailLogBudget = 64;
constexpr int kInterruptDsp = 7;  // __OS_INTERRUPT_DSP_DSP

static void raiseDspInterrupt() {
  sched::postIrq([](guest::Ctx& c) {
    g_dsp[0x0A / 2] |= 0x80;  // DSPINT
    uint32_t handler = sched::callGuest(c, kGetInterruptHandler, {kInterruptDsp});
    if (handler) sched::callGuest(c, handler, {kInterruptDsp, 0});
  });
}

void dspSendMail(uint32_t mail) {
  {
    std::lock_guard lk(g_mailMutex);
    g_mailFromDsp.push_back(mail);
  }
  raiseDspInterrupt();
}

static bool g_axReady = false;    // the audio microcode task has been booted
static bool g_axListNext = false;  // 0xBABE<size> received, list address follows

static void mailToDsp(uint32_t mail) {
  if (g_axReady) {
    // AX protocol: 0xBABE<size>, then the command list address. Process it, then report
    // the task resumed (MusyX's "DSP is done" callback).
    if ((mail >> 16) == 0xBABE) {
      g_axListNext = true;
      return;
    }
    if (g_axListNext) {
      g_axListNext = false;
      ax::processCommandList(mail);
      dspSendMail(0xDCD10001u);
      return;
    }
  }
  if (g_mailLogBudget > 0) {
    g_mailLogBudget--;
    LOG_INFO("mail to DSP %08X", mail);
  }
  // Task boot: 0x80F3D001 <entry> is the last mail; the microcode then reports DSP_INIT.
  if (g_lastMailToDsp == 0x80F3D001u) {
    dspSendMail(0xDCD10000u);
    g_axReady = true;
  }
  g_lastMailToDsp = mail;
}

// --- AI DMA: plays the game's output buffers in real time ------------------------------
// Each time a buffer starts, the AI DMA interrupt fires and its handler programs the next one.
// The guest services interrupts late at times (e.g. while it waits for the renderer), so the
// next buffer is latched only once that handler has run, as it always has on hardware;
// latching early would replay the previous 5 ms block and drop the new one. The host queue is
// kept deep enough (~40 ms) to ride out those waits.
constexpr int kInterruptAiDma = 5;  // __OS_INTERRUPT_DSP_AI
static std::atomic<bool> g_aiDmaRunning{false};
static std::atomic<uint64_t> g_aiIrqDone{0};

static void aiDmaThread() {
  using namespace std::chrono;
  std::vector<int16_t> host;
  auto next = steady_clock::now();
  uint64_t posted = 0;
  for (;;) {
    if (!(g_dsp[0x36 / 2] & 0x8000) || g_paused.load()) {
      std::this_thread::sleep_for(milliseconds(1));
      next = steady_clock::now();
      continue;
    }
    // Wait (bounded) for the handler of the previous block's interrupt.
    auto deadline = steady_clock::now() + milliseconds(100);
    while (g_aiIrqDone.load() < posted && steady_clock::now() < deadline && !g_paused.load())
      std::this_thread::sleep_for(microseconds(200));
    uint32_t addr = ((uint32_t)g_dsp[0x30 / 2] << 16 | g_dsp[0x32 / 2]) & 0x01FFFFE0;
    uint32_t bytes = (g_dsp[0x36 / 2] & 0x7FFF) * 32;
    if (!bytes) bytes = 32;
    uint32_t frames = bytes / 4;
    host.resize(frames * 2);
    const uint8_t* src = guest::g_mem + addr;
    for (uint32_t i = 0; i < frames; i++) {
      // Guest frames are big-endian, right channel first.
      host[2 * i + 1] = (int16_t)(src[4 * i] << 8 | src[4 * i + 1]);
      host[2 * i] = (int16_t)(src[4 * i + 2] << 8 | src[4 * i + 3]);
    }
    // The block has started: its interrupt lets the game prepare the next one.
    posted++;
    sched::postIrq([](guest::Ctx& c) {
      g_dsp[0x0A / 2] |= 0x08;  // AIDINT
      uint32_t handler = sched::callGuest(c, kGetInterruptHandler, {kInterruptAiDma});
      if (handler) sched::callGuest(c, handler, {kInterruptAiDma, 0});
      g_aiIrqDone.fetch_add(1);
    });
    audioout::capture(host.data(), frames);
    const audioout::Sink* sink = audioout::sink();
    if (sink) {
      // Real-time pacing comes from the device draining the queue.
      uint32_t target = sink->rate / 25;  // 40 ms
      while (sink->queuedFrames() > target && !g_paused.load()) std::this_thread::sleep_for(microseconds(500));
      sink->submit(host.data(), frames);
    } else {
      next += nanoseconds((int64_t)frames * 1000000000ll / 32000);
      std::this_thread::sleep_until(next);
    }
  }
}

static void startAiDma() {
  bool expected = false;
  if (g_aiDmaRunning.compare_exchange_strong(expected, true)) std::thread(aiDmaThread).detach();
}

static uint32_t dspRead(uint32_t off, int size) {
  off &= 0x7F;
  switch (off) {
  case 0x00: return (size == 4 ? dsp32(0) : g_dsp[0]) & ~0x80000000u & (size == 4 ? 0xFFFFFFFFu : 0x7FFFu);
  case 0x04:
  case 0x06: {
    std::lock_guard lk(g_mailMutex);
    uint32_t m = g_mailFromDsp.empty() ? 0 : g_mailFromDsp.front();
    bool pop = (size == 4) || off == 0x06;
    if (pop && !g_mailFromDsp.empty()) g_mailFromDsp.pop_front();
    if (size == 4) return m;
    return off == 0x04 ? (m >> 16) : (m & 0xFFFF);
  }
  case 0x16: return 1;  // AR_MODE: ARAM initialized
  }
  if (size == 4) return dsp32(off);
  return g_dsp[off / 2];
}

static void dspWrite(uint32_t off, uint32_t v, int size) {
  off &= 0x7F;
  if (size == 4) {
    g_dsp[off / 2] = (uint16_t)(v >> 16);
    g_dsp[off / 2 + 1] = (uint16_t)v;
  } else if (off == 0x0A) {
    // CSR: interrupt bits (AIDINT 0x08, ARINT 0x20, DSPINT 0x80) are write-1-to-clear;
    // RESET (0x01) completes immediately.
    if (v & 0x01) {
      // Reset: the DSP ROM greets the CPU once it's up.
      std::lock_guard lk(g_mailMutex);
      g_mailFromDsp.assign(1, 0x8071FEEDu);
    }
    uint16_t keep = g_dsp[off / 2] & 0xA8 & ~(uint16_t)v;
    g_dsp[off / 2] = (uint16_t)((v & ~0xA9u) | keep);
    return;
  } else {
    g_dsp[off / 2] = (uint16_t)v;
  }
  if (off == 0x28 || off == 0x2A) aramDma();  // writing the count starts the DMA
  if (off == 0x36 && (g_dsp[0x36 / 2] & 0x8000)) startAiDma();
  if (off == 0x02 || (off == 0x00 && size == 4)) mailToDsp(dsp32(0));
}

static const guest::MmioDevice kDsp = {dspRead, dspWrite};

// --- AI: streaming control + sample counter ------------------------------------------
// AICR: PSTAT 0x01 (play), AFR 0x02 (48 kHz), AIINTMSK 0x04, AIINT 0x08, AIINTVLD 0x10,
// SCRESET 0x20, DSPR 0x40 (DSP rate 32 kHz when clear).
static uint32_t g_aiCr = 0, g_aiVr = 0, g_aiIt = 0;
static uint64_t g_aiStartTb = 0;
static uint32_t g_aiBaseCount = 0;

static uint32_t aiCount() {
  if (!(g_aiCr & 1)) return g_aiBaseCount;
  uint32_t rate = (g_aiCr & 2) ? 48000 : 32000;
  return g_aiBaseCount + (uint32_t)((guest::timeBase() - g_aiStartTb) * rate / 40500000ull);
}

static uint32_t aiRead(uint32_t off, int size) {
  (void)size;
  switch (off & 0x1F) {
  case 0x00: return g_aiCr;
  case 0x04: return g_aiVr;
  case 0x08: return aiCount();
  case 0x0C: return g_aiIt;
  }
  return 0;
}

static void aiWrite(uint32_t off, uint32_t v, int size) {
  (void)size;
  switch (off & 0x1F) {
  case 0x00: {
    uint32_t count = aiCount();
    if (v & 0x20) count = 0;  // SCRESET
    g_aiBaseCount = count;
    g_aiStartTb = guest::timeBase();
    g_aiCr = (v & ~0x28u) | (g_aiCr & 0x08 & ~(v & 0x08));
    break;
  }
  case 0x04: g_aiVr = v; break;
  case 0x0C: g_aiIt = v; break;
  }
}

static const guest::MmioDevice kAi = {aiRead, aiWrite};

void init() {
  guest::registerMmio(0xCC006C00, 0x400, &kAi);
  g_aram = static_cast<uint8_t*>(calloc(1, kAramSize));
  g_mailFromDsp.assign(1, 0x8071FEEDu);  // DSP ROM is up (the IPL's audio init is HLE'd away)
  guest::registerMmio(0xCC006800, 0x400, &kExi);
  guest::registerMmio(0xCC002000, 0x400, &kVi);
  guest::registerMmio(0xCC005000, 0x400, &kDsp);
}

void start() { std::thread(viRetraceThread).detach(); }

}  // namespace hw
