// OS library HLE.
#include "hle.h"
#include "log.h"

// Boots the DSP with the IPL's init microcode; audio is handled on the host.
HLE(__OSInitAudioSystem) { (void)c; }
