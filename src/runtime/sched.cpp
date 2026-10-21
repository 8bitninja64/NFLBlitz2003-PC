// Guest thread scheduler: one host thread per guest thread, a single "guest CPU"
// token (g_running), priority scheduling at the same points the SDK would switch
// (sleep, wakeup, resume, suspend, exit, interrupt return), and interrupt delivery.
#include "sched.h"

#include "boot.h"
#include "func_table.h"
#include "hw.h"
#include "log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace sched {

enum class State { Ready, Waiting, Moribund };

struct Thread {
  const char* name = "thread";
  uint32_t addr = 0;  // guest OSThread*
  int priority = 16;
  int suspendCount = 0;
  State state = State::Ready;
  uint32_t waitQueue = 0;
  bool started = false;
  bool cancelled = false;
  bool detached = false;
  uint32_t entry = 0, param = 0, stackTop = 0;
  guest::Ctx* ctx = nullptr;  // live context of the host thread (set once running)
  std::condition_variable cv;
};

struct ThreadExit {};

constexpr uint32_t kHostWaitQueue = 0xFFFFFFFFu;

// OSThread field offsets (Dolphin SDK).
constexpr uint32_t kThreadState = 0x2C8;    // u16: 1 ready, 2 running, 4 waiting, 8 moribund
constexpr uint32_t kThreadAttr = 0x2CA;     // u16: 1 detached
constexpr uint32_t kThreadSuspend = 0x2CC;  // s32
constexpr uint32_t kThreadPriority = 0x2D0;
constexpr uint32_t kThreadBase = 0x2D4;
constexpr uint32_t kThreadVal = 0x2D8;
constexpr uint32_t kCurrentThread = 0x800000E4;

static std::mutex g_m;
static std::condition_variable g_idleCv;
static Thread* g_running = nullptr;
static std::vector<Thread*> g_threads;
static std::deque<std::function<void(guest::Ctx&)>> g_irqs;
static std::atomic<bool> g_irqQueued{false};  // g_irqs is not empty (g_irqPending may be held up by BLITZ_WATCHW)
static const bool g_holdIrqFlag = getenv("BLITZ_WATCHW") != nullptr;  // keep checkIrq polling the watch
static int g_schedDisable = 0;
static Thread g_idle;
static Thread* g_main = nullptr;
static thread_local Thread* t_self = nullptr;
static thread_local int t_irqDepth = 0;

static bool runnable(const Thread* t) { return t->state == State::Ready && t->suspendCount <= 0 && !t->cancelled; }

static Thread* pickNext(Thread* self) {
  Thread* best = (self && self != &g_idle && runnable(self)) ? self : nullptr;
  for (Thread* t : g_threads)
    if (runnable(t) && (!best || t->priority < best->priority)) best = t;
  return best;
}

static void syncGuest(Thread* t) {
  if (!t->addr) return;
  uint16_t st = t->state == State::Moribund ? 8 : t->state == State::Waiting ? 4 : (t == g_running ? 2 : 1);
  guest::wr16(t->addr + kThreadState, st);
  guest::wr32(t->addr + kThreadSuspend, (uint32_t)t->suspendCount);
  guest::wr32(t->addr + kThreadPriority, (uint32_t)t->priority);
}

// Gives the CPU to `next` (or the idle loop) without waiting. Lock held.
static void handOff(Thread* next) {
  Thread* prev = g_running;
  g_running = next;
  if (prev && prev != &g_idle) syncGuest(prev);
  if (next) {
    guest::wr32(kCurrentThread, next->addr);
    syncGuest(next);
    next->cv.notify_one();
  } else {
    g_idleCv.notify_one();
  }
}

static void waitForCpu(std::unique_lock<std::mutex>& lk, Thread* self) {
  self->cv.wait(lk, [&] { return g_running == self || self->cancelled; });
  if (self->cancelled) {
    if (g_running == self) handOff(pickNext(nullptr));
    throw ThreadExit{};
  }
}

// Switch away from self if another thread should run. Lock held, self holds the CPU.
static void reschedule(std::unique_lock<std::mutex>& lk, Thread* self) {
  if (self == &g_idle || t_irqDepth > 0) return;  // the idle loop / interrupt exit decides
  if (g_schedDisable > 0 && runnable(self)) return;
  Thread* next = pickNext(self);
  if (next == self) return;
  handOff(next);
  waitForCpu(lk, self);
}

Thread* current() {
  Thread* t = t_self;
  if (t == g_main && g_main && !g_main->addr) {
    // __OSThreadInit (recompiled) has set up the default thread by now.
    g_main->addr = guest::rd32(kCurrentThread);
    if (g_main->addr) g_main->priority = (int)guest::rd32(g_main->addr + kThreadPriority);
  }
  return t;
}

Thread* find(uint32_t guestThread) {
  current();
  std::lock_guard lk(g_m);
  for (Thread* t : g_threads)
    if (t->addr == guestThread) return t;
  return nullptr;
}

uint32_t guestAddr(Thread* t) { return t ? t->addr : 0; }

// --- interrupts --------------------------------------------------------------
void postIrq(std::function<void(guest::Ctx&)> fn) {
  std::lock_guard lk(g_m);
  g_irqs.push_back(std::move(fn));
  g_irqQueued = true;
  guest::g_irqPending = 1;
  if (!g_running) g_idleCv.notify_one();
}

static void deliver(guest::Ctx& c) {
  for (;;) {
    std::function<void(guest::Ctx&)> fn;
    {
      std::lock_guard lk(g_m);
      if (g_irqs.empty()) {
        g_irqQueued = false;
        guest::g_irqPending = g_holdIrqFlag ? 1 : 0;
        return;
      }
      fn = std::move(g_irqs.front());
      g_irqs.pop_front();
    }
    uint32_t msr = c.msr;
    c.msr &= ~kMsrEE;
    t_irqDepth++;
    fn(c);
    t_irqDepth--;
    c.msr = msr;
  }
}

uint32_t callGuest(guest::Ctx& c, uint32_t fn, std::initializer_list<uint32_t> args) {
  guest::Ctx tmp = c;
  tmp.r[1] = (c.r[1] - 0x400) & ~15u;
  guest::wr32(tmp.r[1], c.r[1]);  // back chain
  int i = 3;
  for (uint32_t a : args) tmp.r[i++] = a;
  tmp.lr = 0;
  guest::callIndirect(tmp, fn);
  return tmp.r[3];
}

// --- thread lifecycle --------------------------------------------------------
static void threadMain(Thread* t) {
  t_self = t;
  registerTraceRing(t->name);
  guest::Ctx c{};
  t->ctx = &c;
  try {
    {
      std::unique_lock lk(g_m);
      waitForCpu(lk, t);
    }
    c.r[1] = (t->stackTop - 8) & ~7u;
    guest::wr32(c.r[1], 0);
    guest::wr32(c.r[1] + 4, 0);
    c.r[2] = g_main->ctx->r[2];
    c.r[13] = g_main->ctx->r[13];
    c.r[3] = t->param;
    c.msr = kMsrEE;
    for (int k = 0; k < 8; k++) c.gqr[k] = g_main->ctx->gqr[k];
    c.hid2 = g_main->ctx->hid2;
    c.lr = 0;
    guest::callIndirect(c, t->entry);
    exitCurrent(c, c.r[3]);
  } catch (ThreadExit&) {
  }
  t->ctx = nullptr;
  unregisterTraceRing();
  LOG_INFO("guest thread %08X ended", t->addr);
}

Thread* create(uint32_t guestThread, uint32_t entry, uint32_t param, uint32_t stackTop, int priority, bool detached) {
  current();
  auto* t = new Thread;
  t->name = "guest";
  t->addr = guestThread;
  t->entry = entry;
  t->param = param;
  t->stackTop = stackTop;
  t->priority = priority;
  t->suspendCount = 1;
  t->detached = detached;
  std::lock_guard lk(g_m);
  // Games re-create threads in the same OSThread after exiting/cancelling them: the old
  // record must stop answering to that address.
  for (auto it = g_threads.begin(); it != g_threads.end();) {
    Thread* old = *it;
    if (old->addr == guestThread && old != g_main) {
      if (old->state != State::Moribund)
        LOG_WARN("OSCreateThread %08X: thread still alive, replacing it", guestThread);
      old->state = State::Moribund;
      old->cancelled = true;
      old->cv.notify_one();
      it = g_threads.erase(it);  // the Thread object is leaked: its host thread may still unwind
    } else {
      ++it;
    }
  }
  g_threads.push_back(t);
  syncGuest(t);
  return t;
}

int suspend(Thread* t) {
  std::unique_lock lk(g_m);
  int prev = t->suspendCount++;
  syncGuest(t);
  if (t == t_self) reschedule(lk, t);
  return prev;
}

int resume(Thread* t) {
  std::unique_lock lk(g_m);
  int prev = t->suspendCount;
  if (t->suspendCount > 0) t->suspendCount--;
  if (t->suspendCount == 0 && !t->started && t->state != State::Moribund) {
    t->started = true;
    std::thread(threadMain, t).detach();
  }
  syncGuest(t);
  reschedule(lk, t_self);
  return prev;
}

void sleepOn(guest::Ctx& c, uint32_t queue) {
  (void)c;
  Thread* self = current();
  std::unique_lock lk(g_m);
  self->state = State::Waiting;
  self->waitQueue = queue;
  syncGuest(self);
  while (self->state == State::Waiting) {
    Thread* next = pickNext(self);
    handOff(next);
    waitForCpu(lk, self);
  }
}

void wakeup(guest::Ctx& c, uint32_t queue) {
  (void)c;
  Thread* self = current();
  std::unique_lock lk(g_m);
  for (Thread* t : g_threads) {
    if (t->state == State::Waiting && t->waitQueue == queue) {
      t->state = State::Ready;
      t->waitQueue = 0;
      syncGuest(t);
    }
  }
  reschedule(lk, self);
}

void exitCurrent(guest::Ctx& c, uint32_t value) {
  (void)c;
  Thread* self = current();
  {
    std::unique_lock lk(g_m);
    self->state = State::Moribund;
    if (self->addr) guest::wr32(self->addr + kThreadVal, value);
    syncGuest(self);
    handOff(pickNext(nullptr));
  }
  if (self == g_main) {
    LOG_INFO("main guest thread exited (%08X)", value);
    // Keep the process alive for other threads.
    std::unique_lock lk(g_m);
    self->cv.wait(lk, [] { return false; });
  }
  throw ThreadExit{};
}

void cancel(guest::Ctx& c, Thread* t) {
  LOG_INFO("OSCancelThread %08X", t->addr);
  if (t == current()) exitCurrent(c, 0);
  std::lock_guard lk(g_m);
  t->state = State::Moribund;
  t->cancelled = true;
  syncGuest(t);
  t->cv.notify_one();
}

void disableScheduler() {
  std::lock_guard lk(g_m);
  g_schedDisable++;
}

void enableScheduler(guest::Ctx& c) {
  (void)c;
  std::unique_lock lk(g_m);
  if (g_schedDisable > 0) g_schedDisable--;
}

int schedulerDisableCount() { return g_schedDisable; }

void yield(guest::Ctx& c) {
  (void)c;
  std::unique_lock lk(g_m);
  reschedule(lk, current());
}

void setPriority(Thread* t, int priority) {
  std::unique_lock lk(g_m);
  t->priority = priority;
  syncGuest(t);
  reschedule(lk, t_self);
}

void releaseCpu(guest::Ctx& c) {
  (void)c;
  Thread* self = current();
  std::unique_lock lk(g_m);
  self->state = State::Waiting;
  self->waitQueue = kHostWaitQueue;
  handOff(pickNext(self));
}

void reacquireCpu(guest::Ctx& c) {
  (void)c;
  Thread* self = current();
  std::unique_lock lk(g_m);
  self->state = State::Ready;
  self->waitQueue = 0;
  if (!g_running) {
    handOff(self);
  } else {
    // Ask the running thread to reschedule at its next safe point.
    g_irqs.push_back([](guest::Ctx&) {});
    g_irqQueued = true;
    guest::g_irqPending = 1;
  }
  waitForCpu(lk, self);
}

// Debug: guest call stacks from the PowerPC back chain (racy reads; diagnostics only).
void dumpGuestStacks() {
  std::vector<Thread*> ts;
  bool locked = false;
  for (int i = 0; i < 100 && !(locked = g_m.try_lock()); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!locked) {
    logf(LogLevel::Error, "scheduler lock held for over 1 s (deadlock?); running %08X", g_running ? g_running->addr : 0);
    return;
  }
  ts = g_threads;
  g_m.unlock();
  for (Thread* t : ts) {
    if (!t->ctx) continue;
    char line[512];
    int n = snprintf(line, sizeof(line), "stack %08X%s prio %d state %d susp %d queue %08X lr %08X:", t->addr,
                     t == g_running ? " (running)" : "", t->priority, (int)t->state, t->suspendCount, t->waitQueue,
                     t->ctx->lr);
    uint32_t sp = t->ctx->r[1];
    for (int depth = 0; depth < 24 && sp >= 0x80000000u && sp < 0x81800000u && n < (int)sizeof(line) - 12; depth++) {
      uint32_t back = guest::rd32(sp);
      if (back < 0x80000000u || back >= 0x81800000u || back <= sp) break;
      n += snprintf(line + n, sizeof(line) - n, " %08X", guest::rd32(back + 4));
      sp = back;
    }
    logf(LogLevel::Error, "%s", line);
  }
}

std::string currentBacktrace() {
  Thread* t = t_self;
  if (!t || !t->ctx) return "?";
  char line[512];
  int n = snprintf(line, sizeof(line), "lr %08X:", t->ctx->lr);
  uint32_t sp = t->ctx->r[1];
  for (int depth = 0; depth < 24 && sp >= 0x80000000u && sp < 0x81800000u && n < (int)sizeof(line) - 12; depth++) {
    uint32_t back = guest::rd32(sp);
    if (back < 0x80000000u || back >= 0x81800000u || back <= sp) break;
    n += snprintf(line + n, sizeof(line) - n, " %08X", guest::rd32(back + 4));
    sp = back;
  }
  return line;
}

// --- idle loop and main thread ---------------------------------------------------
static void idleMain() {
  t_self = &g_idle;
  registerTraceRing("idle");
  guest::Ctx c{};
  g_idle.ctx = &c;
  uint32_t stack = guest::allocReserved(0x10000);
  for (;;) {
    std::unique_lock lk(g_m);
    g_idleCv.wait(lk, [] { return !g_running && (!g_irqs.empty() || pickNext(nullptr)); });
    if (Thread* n = pickNext(nullptr)) {
      handOff(n);
      continue;
    }
    g_running = &g_idle;
    lk.unlock();
    c = *g_main->ctx;  // r2/r13/GQRs of the game
    c.r[1] = stack + 0x10000 - 64;
    c.msr = 0;
    deliver(c);
    lk.lock();
    handOff(pickNext(nullptr));
  }
}

void runMainThread(uint32_t entry) {
  static guest::Ctx c{};
  g_main = new Thread;
  g_main->name = "main";
  g_main->started = true;
  g_main->ctx = &c;
  t_self = g_main;
  registerTraceRing("main");
  startWatchdog();
  g_threads.push_back(g_main);
  g_running = g_main;
  std::thread(idleMain).detach();
  guest::Fn fn = guest::lookupFunc(entry);
  if (!fn) {
    LOG_ERROR("entry point %08X was not recompiled", entry);
    return;
  }
  try {
    fn(c);
    LOG_INFO("main thread returned (r3=%08X)", c.r[3]);
  } catch (ThreadExit&) {
  }
}

}  // namespace sched

namespace guest {

// BLITZ_WATCHW=addr[+words],...: log guest words when they change, with the guest call stack and
// recent functions, checked at every loop back-edge (debugging; slow: keeps the IRQ flag raised).
static const char* const g_watchW = getenv("BLITZ_WATCHW");

static void pollWriteWatch(Ctx& c) {
  static uint32_t last[64];
  static int logged = 0;
  static bool primed = false;  // the first poll records the current values
  int k = 0;
  for (const char* q = g_watchW; q && *q && k < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : nullptr) {
    char* end = nullptr;
    uint32_t a = (uint32_t)strtoul(q, &end, 16), n = *end == '+' ? (uint32_t)strtoul(end + 1, nullptr, 10) : 1;
    for (uint32_t w = 0; w < n && k < 64; w++, k++) {
      uint32_t v = guest::rd32(a + 4 * w);
      if (primed && v != last[k] && logged < 200) {
        logged++;
        LOG_INFO("watchw %08X: %08X -> %08X frame %u at %s", a + 4 * w, last[k], v, hw::g_framesBuilt.load(), sched::currentBacktrace().c_str());
        dumpCallTrace();
      }
      last[k] = v;
    }
  }
  primed = true;
  (void)c;
}

void checkIrq(Ctx& c) {
  if (g_watchW) {
    pollWriteWatch(c);
    g_irqPending = 1;
    if (!sched::g_irqQueued) return;
  }
  if (!(c.msr & sched::kMsrEE) || sched::t_irqDepth > 0) return;
  sched::deliver(c);
  std::unique_lock lk(sched::g_m);
  sched::reschedule(lk, sched::current());
}

void writeMsr(Ctx& c, uint32_t v) {
  uint32_t old = c.msr;
  c.msr = v;
  if (!(old & sched::kMsrEE) && (v & sched::kMsrEE) && g_irqPending) checkIrq(c);
}

}  // namespace guest
