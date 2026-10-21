// Guest thread scheduling and interrupt delivery.
//
// Every guest OSThread runs on its own host thread; only the thread holding the
// guest CPU executes guest code, so guest code never runs concurrently. The SDK's
// thread functions (OSCreateThread, OSSleepThread, ...) are HLE'd on top of this
// (hle_thread.cpp). Interrupts are queued by host devices and taken at safe points
// (loop back-edges, HLE calls) while MSR[EE] is set, or by the idle thread when no
// guest thread is runnable.
#pragma once

#include "ctx.h"

#include <functional>
#include <string>

namespace sched {

constexpr uint32_t kMsrEE = 0x8000;

struct Thread;

void runMainThread(uint32_t entry);

// Queue an interrupt-context callback (runs on the guest CPU with interrupts off).
void postIrq(std::function<void(guest::Ctx&)> fn);

// Call a guest function from host code on the current guest CPU, with a scratch copy
// of the context whose stack frame lies below the interrupted one. Integer arguments
// go in r3.. .
uint32_t callGuest(guest::Ctx& c, uint32_t fn, std::initializer_list<uint32_t> args = {});

// --- thread API used by the OS HLE --------------------------------------------
Thread* current();
Thread* find(uint32_t guestThread);  // nullptr if the runtime doesn't know it
uint32_t guestAddr(Thread* t);
Thread* create(uint32_t guestThread, uint32_t entry, uint32_t param, uint32_t stackTop, int priority, bool detached);
int suspend(Thread* t);              // returns previous suspend count
int resume(Thread* t);
void sleepOn(guest::Ctx& c, uint32_t queue);      // blocks the calling thread until woken
void wakeup(guest::Ctx& c, uint32_t queue);       // readies every thread sleeping on queue
[[noreturn]] void exitCurrent(guest::Ctx& c, uint32_t value);
void cancel(guest::Ctx& c, Thread* t);
void disableScheduler();
void enableScheduler(guest::Ctx& c);
int schedulerDisableCount();
void yield(guest::Ctx& c);           // reschedule: hand the CPU to a higher-priority thread
void setPriority(Thread* t, int priority);

// Host waits that block on "hardware" release the guest CPU meanwhile.
void dumpGuestStacks();  // diagnostics: back-chain walk of every guest thread
// Diagnostics: lr and back-chain return addresses of the calling guest thread's own context (inside
// callGuest, e.g. an interrupt handler, that is the interrupted code's stack).
std::string currentBacktrace();

void releaseCpu(guest::Ctx& c);
void reacquireCpu(guest::Ctx& c);

}  // namespace sched
