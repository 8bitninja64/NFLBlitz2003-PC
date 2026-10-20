// Logging and call tracing.
#pragma once

#include <cstdint>

enum class LogLevel { Debug, Info, Warn, Error, Fatal };

void logf(LogLevel level, const char* fmt, ...);
void setLogLevel(LogLevel level);
bool logEnabled(LogLevel level);
void dumpCallTrace();                    // this host thread's recent guest functions
void dumpAllTraces();
void registerTraceRing(const char* name);  // call on each host thread that runs guest code
void startWatchdog();                    // BLITZ_WATCHDOG=<secs>: periodic trace dumps

#define LOG_DEBUG(...) \
  do { \
    if (logEnabled(LogLevel::Debug)) logf(LogLevel::Debug, __VA_ARGS__); \
  } while (0)
#define LOG_INFO(...) logf(LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) logf(LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) logf(LogLevel::Error, __VA_ARGS__)
