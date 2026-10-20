// PAD HLE: the game reads controllers through PADRead; state comes from pad_input
// (host devices via Aurora, or scripted input). SI stays unemulated.
#include "hle.h"
#include "log.h"
#include "pad_input.h"

using hle::arg;
using hle::ret;

HLE(PADInit) { ret(c, 1); }
HLE(PADReset) { ret(c, 1); }
HLE(PADRecalibrate) { ret(c, 1); }
HLE(PADSetSpec) { (void)c; }

// u32 PADRead(PADStatus status[4]) -- guest PADStatus is 12 bytes, big-endian button.
HLE(PADRead) {
  uint32_t dst = arg(c, 0);
  padinput::Pad pads[padinput::kPorts];
  padinput::read(pads);
  uint32_t connected = 0;
  for (int i = 0; i < padinput::kPorts; i++) {
    const padinput::Pad& p = pads[i];
    uint32_t a = dst + 12 * i;
    guest::wr16(a + 0, p.button);
    guest::wr8(a + 2, (uint8_t)p.stickX);
    guest::wr8(a + 3, (uint8_t)p.stickY);
    guest::wr8(a + 4, (uint8_t)p.substickX);
    guest::wr8(a + 5, (uint8_t)p.substickY);
    guest::wr8(a + 6, p.triggerL);
    guest::wr8(a + 7, p.triggerR);
    guest::wr8(a + 8, p.analogA);
    guest::wr8(a + 9, p.analogB);
    guest::wr8(a + 10, (uint8_t)p.err);
    guest::wr8(a + 11, 0);
    if (p.err == 0) connected |= 0x80000000u >> i;
  }
  ret(c, connected);
}

HLE(PADControlMotor) { padinput::setMotor((int)arg(c, 0), arg(c, 1)); }

HLE(PADControlAllMotors) {
  uint32_t cmds = arg(c, 0);
  for (int i = 0; i < padinput::kPorts; i++) padinput::setMotor(i, guest::rd32(cmds + 4 * i));
}

// The game drives PADRead itself; a sampling callback (SI polling) is never invoked.
HLE(PADSetSamplingCallback) { ret(c, 0); }
