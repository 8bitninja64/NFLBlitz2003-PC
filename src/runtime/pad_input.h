// Controller state shared between the host input layer (window app) and the PAD HLE.
#pragma once

#include <cstdint>

namespace padinput {

constexpr int kPorts = 4;

// Mirrors the SDK's PADStatus fields (host byte order).
struct Pad {
  uint16_t button = 0;
  int8_t stickX = 0, stickY = 0, substickX = 0, substickY = 0;
  uint8_t triggerL = 0, triggerR = 0, analogA = 0, analogB = 0;
  int8_t err = -1;  // PAD_ERR_NO_CONTROLLER until a device reports in
};

// Host input thread: publish the latest sampled state.
void publish(const Pad pads[kPorts]);
// Guest side: current state, with scripted input (BLITZ_INPUT) applied on top.
void read(Pad pads[kPorts]);

// Rumble requests from the guest; the host applies them (PAD_MOTOR_STOP/RUMBLE/STOP_HARD).
void setMotor(int port, uint32_t cmd);
bool takeMotor(int port, uint32_t* cmd);  // true if changed since last call

// Frame clock for scripted input (VI retraces).
void tick();
// The game is setting up a players' match (not the attract demo): scripted m+ steps start with it.
void playersMatchSetup();

}  // namespace padinput
