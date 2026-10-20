// Translates the game's hardware GX command stream (from gx_fifo) into Aurora's FIFO
// format and hands complete frames to the Aurora thread.
//
// Raw BP/CP/XF loads and draws pass through unchanged. Commands that carry guest
// physical addresses become Aurora's extended commands with host pointers: texture
// binds (LOAD_TEXOBJ), TLUT loads (LOAD_TLUT), vertex array bases (LOAD_ARRAYBASE,
// sized from the indices each draw uses) and EFB texture copies. A display copy ends
// the guest frame; the guest waits until Aurora has consumed it (lockstep), so Aurora
// may read guest memory while processing.
#pragma once

#include <cstdint>
#include <vector>

namespace gxbridge {

void init();  // installs the gx_fifo sink

// Aurora thread: wait up to timeoutMs for a finished guest frame, then replay it and
// call frameConsumed() so the guest continues.
bool takeFrame(std::vector<uint8_t>& out, int timeoutMs);
void frameConsumed();
void shutdown();  // releases a guest waiting on a frame handoff

}  // namespace gxbridge
