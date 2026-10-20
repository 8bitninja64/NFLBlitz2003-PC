// GX FIFO path: the write-gather pipe (0xCC008000) writes into guest RAM at the PI
// FIFO write pointer, as on hardware, and while the graphics processor's FIFO reads
// are enabled the stream is also parsed as GP commands (gx_fifo.cpp). The parser
// completes draw-sync tokens / DrawDone like an instant GPU and hands each command to
// a sink (the Aurora bridge).
#pragma once

#include <cstdint>

namespace gxfifo {

struct Sink {
  // Called per complete GP command (raw big-endian bytes as on the wire). Display
  // lists are expanded inline, so CALL_DL never reaches the sink.
  virtual void command(const uint8_t* data, uint32_t size) = 0;
  virtual ~Sink() = default;
};

void init();  // registers CP/PE/PI MMIO and the write-gather sink
void setSink(Sink* sink);

struct Stats {
  uint64_t commands, draws, bpWrites, xfWrites, cpWrites, displayLists, displayCopies, bytes;
};
Stats stats();

// Vertex layout for the given VAT index from the current CP state (in stream order).
// Attributes in vertex order: PNMTXIDX, TEX0-7MTXIDX, POS, NRM, CLR0, CLR1, TEX0-7.
constexpr int kNumAttrs = 21, kAttrPos = 9, kAttrNrm = 10, kAttrClr0 = 11, kAttrTex0 = 13;
struct Attr {
  uint32_t type;     // 0 none, 1 direct, 2 index8, 3 index16
  uint32_t size;     // bytes in the vertex
  uint32_t indices;  // indices per vertex (NBT normals with NORMALINDEX3: 3)
};
void vertexLayout(uint32_t vat, Attr out[kNumAttrs]);
uint32_t vertexSize(uint32_t vat);

}  // namespace gxfifo
