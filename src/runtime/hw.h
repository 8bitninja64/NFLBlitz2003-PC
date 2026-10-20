// Low-level hardware register models (EXI, VI).
#pragma once

#include <cstdint>

namespace hw {

constexpr uint32_t kGetInterruptHandler = 0x801A92D8;  // __OSGetInterruptHandler (USA)

constexpr uint32_t kAramSize = 0x01000000;  // 16 MB
extern uint8_t* g_aram;

void init();   // register MMIO devices (before boot)
void start();  // start device timers (VI retrace)
uint32_t viFramebufferAddress();  // XFB the VI scans out (physical)

}  // namespace hw
