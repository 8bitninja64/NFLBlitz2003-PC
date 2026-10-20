// DVD read HLE: reads come from the user's disc image on a worker thread with a
// modelled latency, then complete on the guest CPU like the DI interrupt would.
// DVDOpen / path lookup stay recompiled (they walk the FST that boot.cpp loads).
#include "disc.h"
#include "hle.h"
#include "log.h"
#include "sched.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using hle::arg;
using hle::ret;

namespace {

// DVDCommandBlock
constexpr uint32_t kCbCommand = 0x08, kCbState = 0x0C, kCbOffset = 0x10, kCbLength = 0x14, kCbAddr = 0x18,
                   kCbTransferred = 0x20, kCbCallback = 0x28;
// DVDFileInfo
constexpr uint32_t kFiStart = 0x30, kFiCallback = 0x38;

constexpr int kStateEnd = 0, kStateBusy = 1, kStateCanceled = 10;

struct Request {
  uint32_t block;
  uint32_t dst;
  uint32_t length;
  uint64_t discOffset;
  uint32_t callback;   // guest callback(s32 result, block)
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Request> g_queue;
bool g_busy = false;
bool g_started = false;

void worker() {
  std::vector<uint8_t> buf;
  for (;;) {
    Request r;
    {
      std::unique_lock lk(g_mutex);
      g_cv.wait(lk, [] { return !g_queue.empty(); });
      r = g_queue.front();
      g_queue.pop_front();
      g_busy = true;
    }
    buf.resize(r.length);
    bool ok = g_disc.read(r.discOffset, buf.data(), r.length);
    // Console-ish timing: seek + ~3 MB/s transfer, capped so loads stay quick.
    auto us = 300 + (uint64_t)r.length / 16;
    if (us > 20000) us = 20000;
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    sched::postIrq([r, ok, data = std::move(buf)](guest::Ctx& c) {
      if (guest::rd32(r.block + kCbState) == kStateCanceled) return;
      if (ok) memcpy(guest::ptr(r.dst), data.data(), r.length);
      else LOG_ERROR("DVD read failed: offset %llx length %x", (unsigned long long)r.discOffset, r.length);
      guest::wr32(r.block + kCbTransferred, ok ? r.length : 0);
      guest::wr32(r.block + kCbState, ok ? kStateEnd : (uint32_t)-1);
      if (r.callback) sched::callGuest(c, r.callback, {ok ? r.length : (uint32_t)-1, r.block});
    });
    buf = {};
    std::lock_guard lk(g_mutex);
    g_busy = !g_queue.empty();
  }
}

void submit(const Request& r) {
  if (r.length > 0x01800000) LOG_ERROR("DVD read of %u bytes at %llx (block %08X)", r.length, (unsigned long long)r.discOffset, r.block);
  guest::wr32(r.block + kCbState, kStateBusy);
  guest::wr32(r.block + kCbCommand, 1);  // read
  guest::wr32(r.block + kCbAddr, r.dst);
  guest::wr32(r.block + kCbLength, r.length);
  guest::wr32(r.block + kCbTransferred, 0);
  std::lock_guard lk(g_mutex);
  if (!g_started) {
    g_started = true;
    std::thread(worker).detach();
  }
  g_queue.push_back(r);
  g_cv.notify_one();
}

}  // namespace

// BOOL DVDReadAsyncPrio(DVDFileInfo*, void* addr, s32 length, s32 offset, DVDCallback, s32 prio)
HLE(DVDReadAsyncPrio) {
  uint32_t fi = arg(c, 0), dst = arg(c, 1), len = arg(c, 2), off = arg(c, 3), cb = arg(c, 4);
  guest::wr32(fi + kFiCallback, cb);
  guest::wr32(fi + kCbOffset, off);
  guest::wr32(fi + kCbCallback, cb);
  LOG_DEBUG("DVDReadAsyncPrio fi=%08X dst=%08X len=%X off=%X", fi, dst, len, off);
  submit({fi, dst, len, (uint64_t)guest::rd32(fi + kFiStart) + off, cb});
  ret(c, 1);
}

// BOOL DVDReadAbsAsyncPrio(DVDCommandBlock*, void* addr, s32 length, s32 offset, DVDCBCallback, s32 prio)
HLE(DVDReadAbsAsyncPrio) {
  uint32_t block = arg(c, 0), dst = arg(c, 1), len = arg(c, 2), off = arg(c, 3), cb = arg(c, 4);
  guest::wr32(block + kCbOffset, off);
  guest::wr32(block + kCbCallback, cb);
  submit({block, dst, len, off, cb});
  ret(c, 1);
}

HLE(DVDGetCommandBlockStatus) { ret(c, guest::rd32(arg(c, 0) + kCbState)); }

HLE(DVDGetDriveStatus) {
  std::lock_guard lk(g_mutex);
  ret(c, (g_busy || !g_queue.empty()) ? kStateBusy : kStateEnd);
}

// BOOL DVDCancelAsync(DVDCommandBlock*, DVDCBCallback)
HLE(DVDCancelAsync) {
  uint32_t block = arg(c, 0), cb = arg(c, 1);
  {
    std::lock_guard lk(g_mutex);
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
      if (it->block == block) {
        g_queue.erase(it);
        break;
      }
  }
  if (guest::rd32(block + kCbState) == kStateBusy) guest::wr32(block + kCbState, kStateCanceled);
  if (cb) sched::callGuest(c, cb, {0, block});
  ret(c, 1);
}

HLE(DVDCancel) {
  uint32_t block = arg(c, 0);
  std::lock_guard lk(g_mutex);
  for (auto it = g_queue.begin(); it != g_queue.end(); ++it)
    if (it->block == block) {
      g_queue.erase(it);
      break;
    }
  if (guest::rd32(block + kCbState) == kStateBusy) guest::wr32(block + kCbState, kStateCanceled);
  ret(c, 0);
}

HLE(DVDCheckDisk) { ret(c, 1); }
