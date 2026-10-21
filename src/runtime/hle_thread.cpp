// OSThread / scheduler / alarm HLE on top of the host scheduler (sched.cpp).
#include "boot.h"
#include "hle.h"
#include "log.h"
#include "guest_sched.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

using hle::arg;
using hle::ret;

static sched::Thread* threadArg(guest::Ctx& c, uint32_t addr, const char* fn) {
  sched::Thread* t = sched::find(addr);
  if (!t) guest::trap(c, addr, fn);
  return t;
}

// BOOL OSCreateThread(OSThread*, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority, u16 attr)
HLE(OSCreateThread) {
  uint32_t thread = arg(c, 0), func = arg(c, 1), param = arg(c, 2), stack = arg(c, 3), size = arg(c, 4);
  int prio = (int)arg(c, 5);
  uint32_t attr = arg(c, 6) & 0xFFFF;
  if (prio < 0 || prio > 31) return ret(c, 0);
  memset(guest::ptr(thread), 0, 0x318);
  guest::wr16(thread + 0x2CA, (uint16_t)attr);
  guest::wr32(thread + 0x2D4, (uint32_t)prio);
  guest::wr32(thread + 0x304, stack);
  guest::wr32(thread + 0x308, stack - size);
  guest::wr32(stack - size, 0xDEADBABE);
  sched::create(thread, func, param, stack, prio, attr & 1);
  LOG_DEBUG("OSCreateThread %08X func=%08X prio=%d stack=%08X", thread, func, prio, stack);
  ret(c, 1);
}

HLE(OSResumeThread) { ret(c, (uint32_t)sched::resume(threadArg(c, arg(c, 0), "OSResumeThread: unknown thread"))); }
HLE(OSSuspendThread) { ret(c, (uint32_t)sched::suspend(threadArg(c, arg(c, 0), "OSSuspendThread: unknown thread"))); }
HLE(OSSleepThread) { sched::sleepOn(c, arg(c, 0)); }
HLE(OSWakeupThread) { sched::wakeup(c, arg(c, 0)); }
HLE(OSExitThread) { sched::exitCurrent(c, arg(c, 0)); }
HLE(OSCancelThread) { sched::cancel(c, threadArg(c, arg(c, 0), "OSCancelThread: unknown thread")); }
HLE(__OSReschedule) { sched::yield(c); }

HLE(OSDisableScheduler) {
  int prev = sched::schedulerDisableCount();
  sched::disableScheduler();
  ret(c, (uint32_t)prev);
}

HLE(OSEnableScheduler) {
  int prev = sched::schedulerDisableCount();
  sched::enableScheduler(c);
  ret(c, (uint32_t)prev);
}

HLE(SelectThread) { guest::trap(c, 0x801AB72C, "SelectThread reached (scheduler is HLE)"); }

// --- alarms --------------------------------------------------------------------
// OSAlarm: handler 0x00, tag 0x04, fire 0x08 (u64), prev 0x10, next 0x14, period 0x18 (u64), start 0x20 (u64)
namespace {

struct Alarm {
  uint64_t fire;
  uint64_t period;
  uint64_t start;
  uint32_t handler;
};

std::mutex g_alarmMutex;
std::condition_variable g_alarmCv;
std::map<uint32_t, Alarm> g_alarms;  // keyed by guest OSAlarm*
uint32_t g_alarmContext = 0;         // scratch OSContext passed to handlers
bool g_alarmThreadStarted = false;

uint64_t ticksToNs(uint64_t t) { return t * 2000ull / 81ull; }

void alarmThread() {
  using namespace std::chrono;
  std::unique_lock lk(g_alarmMutex);
  for (;;) {
    if (g_alarms.empty()) {
      g_alarmCv.wait(lk);
      continue;
    }
    auto next = g_alarms.begin();
    for (auto it = g_alarms.begin(); it != g_alarms.end(); ++it)
      if (it->second.fire < next->second.fire) next = it;
    uint64_t now = guest::timeBase();
    if (next->second.fire > now) {
      g_alarmCv.wait_for(lk, nanoseconds(ticksToNs(next->second.fire - now) / guest::timeWarp()));
      continue;
    }
    uint32_t alarm = next->first;
    uint32_t handler = next->second.handler;
    if (next->second.period) {
      uint64_t p = next->second.period;
      uint64_t f = next->second.fire;
      f += ((now - f) / p + 1) * p;
      next->second.fire = f;
    } else {
      g_alarms.erase(next);
    }
    bool periodic = next->second.period != 0;
    sched::postIrq([alarm, handler, periodic](guest::Ctx& c) {
      // Skip if the alarm was cancelled or re-armed with another handler meanwhile.
      if (guest::rd32(alarm) != handler) return;
      if (!periodic) guest::wr32(alarm, 0);
      sched::callGuest(c, handler, {alarm, g_alarmContext});
    });
  }
}

void armAlarm(uint32_t alarm, const Alarm& a) {
  std::lock_guard lk(g_alarmMutex);
  if (!g_alarmThreadStarted) {
    g_alarmThreadStarted = true;
    g_alarmContext = guest::allocReserved(0x2C8);
    std::thread(alarmThread).detach();
  }
  guest::wr32(alarm + 0x00, a.handler);
  guest::wr64(alarm + 0x08, a.fire);
  guest::wr64(alarm + 0x18, a.period);
  guest::wr64(alarm + 0x20, a.start);
  g_alarms[alarm] = a;
  g_alarmCv.notify_one();
}

uint64_t arg64(guest::Ctx& c, int reg) { return ((uint64_t)c.r[reg] << 32) | c.r[reg + 1]; }

}  // namespace

// void OSSetAlarm(OSAlarm*, OSTime tick, OSAlarmHandler)  -- tick in r5:r6
HLE(OSSetAlarm) {
  uint64_t now = guest::timeBase();
  armAlarm(c.r[3], {now + arg64(c, 5), 0, 0, c.r[7]});
}

// void OSSetPeriodicAlarm(OSAlarm*, OSTime start, OSTime period, OSAlarmHandler)
HLE(OSSetPeriodicAlarm) {
  uint64_t start = arg64(c, 5), period = arg64(c, 7);
  uint64_t now = guest::timeBase();
  uint64_t fire = start;
  if (period && fire <= now) fire += ((now - fire) / period + 1) * period;
  armAlarm(c.r[3], {fire, period, start, c.r[9]});
}

HLE(OSCancelAlarm) {
  std::lock_guard lk(g_alarmMutex);
  g_alarms.erase(c.r[3]);
  guest::wr32(c.r[3] + 0x00, 0);
}
