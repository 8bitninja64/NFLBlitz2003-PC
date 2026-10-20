// Low-level hardware register models (EXI, VI).
#pragma once

#include <atomic>
#include <cstdint>

namespace hw {

constexpr uint32_t kGetInterruptHandler = 0x801A92D8;  // __OSGetInterruptHandler (USA)

constexpr uint32_t kAramSize = 0x01000000;  // 16 MB
extern uint8_t* g_aram;

// Suspends the VI retrace and AI DMA interrupts while the app holds the guest (pause menu).
extern std::atomic<bool> g_paused;

void init();   // register MMIO devices (before boot)
void start();  // start device timers (VI retrace)
uint32_t viFramebufferAddress();  // XFB the VI scans out (physical)

}  // namespace hw
