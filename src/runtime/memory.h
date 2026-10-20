// Guest memory setup and hardware register dispatch.
#pragma once

#include "ctx.h"

namespace guest {

struct MmioDevice {
  uint32_t (*read)(uint32_t off, int size);          // off = ea & 0xFFFF
  void (*write)(uint32_t off, uint32_t v, int size);
};
using MmioHandler = const MmioDevice*;
using FifoSink = void (*)(uint32_t v, int size);  // write-gather pipe (0xCC008000)

void initMemory();
void registerMmio(uint32_t base, uint32_t size, MmioHandler handler);
void setFifoSink(FifoSink sink);

// Typed guest pointer helpers for host code.
inline uint8_t* ptr(uint32_t ea) { return hostp(ea); }
inline const char* str(uint32_t ea) { return ea ? reinterpret_cast<const char*>(hostp(ea)) : nullptr; }

}  // namespace guest
