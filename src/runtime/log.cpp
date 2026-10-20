#include "log.h"

#include "ctx.h"
#include "gx_fifo.h"
#include "sched.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

static LogLevel g_level = LogLevel::Info;
static std::mutex g_logMutex;

void setLogLevel(LogLevel level) { g_level = level; }
bool logEnabled(LogLevel level) { return level >= g_level; }

void logf(LogLevel level, const char* fmt, ...) {
  if (level < g_level) return;
  static const char* names[] = {"debug", "info", "warn", "error", "FATAL"};
  static const auto start = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard lk(g_logMutex);
  fprintf(stderr, "[%7lld %s] %s\n", (long long)ms, names[(int)level], buf);
  fflush(stderr);
}

namespace guest {
thread_local TraceRing t_trace;
}

static std::mutex g_ringMutex;
static std::vector<std::pair<const char*, guest::TraceRing*>> g_rings;

// Generated code records into its own module's thread-local ring; the loader tells us how
// to reach it from each host thread.
static guest::TraceRing* (*g_ringSource)() = nullptr;
void setTraceRingSource(guest::TraceRing* (*source)()) { g_ringSource = source; }
static guest::TraceRing* threadRing() { return g_ringSource ? g_ringSource() : &guest::t_trace; }

void registerTraceRing(const char* name) {
  std::lock_guard lk(g_ringMutex);
  g_rings.emplace_back(name, threadRing());
}

void unregisterTraceRing() {
  std::lock_guard lk(g_ringMutex);
  for (auto it = g_rings.begin(); it != g_rings.end(); ++it)
    if (it->second == threadRing()) {
      g_rings.erase(it);
      break;
    }
}

static void printRing(const char* name, const guest::TraceRing& r) {
  char line[64 * 10 + 64];
  int n = snprintf(line, sizeof(line), "trace %s (loop %08X; oldest first):", name, r.loop);
  uint32_t pos = r.pos;
  for (uint32_t k = pos > 64 ? pos - 64 : 0; k < pos && n < (int)sizeof(line) - 12; k++)
    n += snprintf(line + n, sizeof(line) - n, " %08X", r.addr[k & 63]);
  logf(LogLevel::Error, "%s", line);
}

void dumpCallTrace() { printRing("current", *threadRing()); }

void dumpAllTraces() {
  std::lock_guard lk(g_ringMutex);
  for (auto& [name, ring] : g_rings) printRing(name, *ring);
}

#ifdef _WIN32
#include <windows.h>

static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  HMODULE mod = nullptr;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCSTR>(er->ExceptionAddress), &mod);
  char modName[MAX_PATH] = "?";
  if (mod) GetModuleFileNameA(mod, modName, sizeof(modName));
  logf(LogLevel::Fatal, "host exception %08lX at %p (%s+%llX)%s%llX", er->ExceptionCode, er->ExceptionAddress, modName,
       (unsigned long long)((const char*)er->ExceptionAddress - (const char*)mod),
       er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? " accessing " : " info ",
       er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
  dumpCallTrace();
  dumpAllTraces();
  sched::dumpGuestStacks();
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void installCrashHandler() {
#ifdef _WIN32
  SetUnhandledExceptionFilter(crashFilter);
  // Leave room to report a stack overflow on the faulting thread.
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
#endif
}

void startWatchdog() {
  const char* s = getenv("BLITZ_WATCHDOG");
  if (!s) return;
  int secs = atoi(s);
  if (secs <= 0) secs = 3;
  std::thread([secs] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(secs));
      dumpAllTraces();
      sched::dumpGuestStacks();
      if (const char* peek = getenv("BLITZ_PEEK")) {
        // BLITZ_PEEK=addr[,addr...]: 32-bit guest words to print with each dump.
        char line[1024];
        int n = snprintf(line, sizeof(line), "peek:");
        for (const char* p = peek; *p && n < (int)sizeof(line) - 24;) {
          uint32_t a = (uint32_t)strtoul(p, const_cast<char**>(&p), 16);
          n += snprintf(line + n, sizeof(line) - n, " %08X=%08X", a, guest::rd32(a));
          while (*p == ',' || *p == ' ') p++;
        }
        logf(LogLevel::Error, "%s", line);
      }
      auto st = gxfifo::stats();
      logf(LogLevel::Error, "gx: %llu bytes, %llu cmds, %llu draws, %llu BP, %llu XF, %llu CP, %llu DLs, %llu display copies",
           (unsigned long long)st.bytes, (unsigned long long)st.commands, (unsigned long long)st.draws,
           (unsigned long long)st.bpWrites, (unsigned long long)st.xfWrites, (unsigned long long)st.cpWrites,
           (unsigned long long)st.displayLists, (unsigned long long)st.displayCopies);
    }
  }).detach();
}
