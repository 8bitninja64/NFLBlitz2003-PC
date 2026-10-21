// Memory card HLE on Aurora's CARD library (Dolphin-compatible GCI folders). Operations
// complete immediately on the host; async callbacks run as interrupts.
#include "hle.h"
#include "log.h"
#include "guest_sched.h"

#include <aurora/card.h>
#include <dolphin/card.h>

extern "C" BOOL CARDProbe(s32 chan);  // defined by Aurora's CARD library, not in its header

#include <filesystem>
#include <mutex>

using hle::arg;
// Result logging (-v) for every CARD call.
#define ret(c, v) cardRet(c, __func__, (uint32_t)(v))
static void cardRet(guest::Ctx& c, const char* fn, uint32_t v) {
  LOG_DEBUG("%s -> %d", fn, (int32_t)v);
  (hle::ret)(c, v);
}

namespace {

bool g_cardInit = false;

void ensureInit() {
  if (g_cardInit) return;
  g_cardInit = true;
  // A card is "inserted" in slot A: Aurora treats a missing GCI folder as an empty slot.
  char path[1024];
  if (aurora_card_get_path("GO3E", AURORA_CARD_GCI_DIRECTORY, 0, path, sizeof(path))) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    LOG_INFO("memory card A: %s", path);
  }
  CARDInit("GO3E", "5D");
}

void callback(uint32_t cb, int32_t chan, int32_t result) {
  if (!cb) return;
  sched::postIrq([cb, chan, result](guest::Ctx& c) { sched::callGuest(c, cb, {(uint32_t)chan, (uint32_t)result}); });
}

// CARDFileInfo: chan, fileNo, offset, length (s32), iBlock (u16).
CARDFileInfo fileInfoFromGuest(uint32_t a) {
  CARDFileInfo fi{};
  fi.chan = (s32)guest::rd32(a);
  fi.fileNo = (s32)guest::rd32(a + 4);
  fi.offset = (s32)guest::rd32(a + 8);
  fi.length = (s32)guest::rd32(a + 12);
  fi.iBlock = guest::rd16(a + 16);
  return fi;
}

void fileInfoToGuest(uint32_t a, const CARDFileInfo& fi) {
  guest::wr32(a, (uint32_t)fi.chan);
  guest::wr32(a + 4, (uint32_t)fi.fileNo);
  guest::wr32(a + 8, (uint32_t)fi.offset);
  guest::wr32(a + 12, (uint32_t)fi.length);
  guest::wr16(a + 16, fi.iBlock);
}

// CARDStat has the same layout in host and guest; only the byte order differs.
void statToGuest(uint32_t a, const CARDStat& st) {
  memcpy(guest::ptr(a), st.fileName, CARD_FILENAME_MAX);
  guest::wr32(a + 0x20, st.length);
  guest::wr32(a + 0x24, st.time);
  memcpy(guest::ptr(a + 0x28), st.gameName, 4);
  memcpy(guest::ptr(a + 0x2C), st.company, 2);
  guest::wr8(a + 0x2E, st.bannerFormat);
  guest::wr32(a + 0x30, st.iconAddr);
  guest::wr16(a + 0x34, st.iconFormat);
  guest::wr16(a + 0x36, st.iconSpeed);
  guest::wr32(a + 0x38, st.commentAddr);
  guest::wr32(a + 0x3C, st.offsetBanner);
  guest::wr32(a + 0x40, st.offsetBannerTlut);
  for (int i = 0; i < CARD_ICON_MAX; i++) guest::wr32(a + 0x44 + 4 * i, st.offsetIcon[i]);
  guest::wr32(a + 0x64, st.offsetIconTlut);
  guest::wr32(a + 0x68, st.offsetData);
}

CARDStat statFromGuest(uint32_t a) {
  CARDStat st{};
  memcpy(st.fileName, guest::ptr(a), CARD_FILENAME_MAX);
  st.length = guest::rd32(a + 0x20);
  st.time = guest::rd32(a + 0x24);
  memcpy(st.gameName, guest::ptr(a + 0x28), 4);
  memcpy(st.company, guest::ptr(a + 0x2C), 2);
  st.bannerFormat = guest::rd8(a + 0x2E);
  st.iconAddr = guest::rd32(a + 0x30);
  st.iconFormat = guest::rd16(a + 0x34);
  st.iconSpeed = guest::rd16(a + 0x36);
  st.commentAddr = guest::rd32(a + 0x38);
  return st;
}

bool present(s32 chan) { return chan == 0 && CARDProbe(chan); }

}  // namespace

HLE(CARDInit) {
  (void)c;
  ensureInit();
}

HLE(CARDGetResultCode) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  ret(c, present(chan) ? (uint32_t)CARDGetResultCode(chan) : (uint32_t)CARD_RESULT_NOCARD);
}

HLE(CARDProbeEx) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  uint32_t memSizeP = arg(c, 1), sectorSizeP = arg(c, 2);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  s32 memSize = 0, sectorSize = 0;
  s32 r = CARDProbeEx(chan, &memSize, &sectorSize);
  LOG_DEBUG("CARDProbeEx(%d): memSize %d sectorSize %d", chan, memSize, sectorSize);
  if (memSizeP) guest::wr32(memSizeP, (uint32_t)memSize);
  if (sectorSizeP) guest::wr32(sectorSizeP, (uint32_t)sectorSize);
  ret(c, (uint32_t)r);
}

HLE(CARDMount) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  ret(c, present(chan) ? (uint32_t)CARDMount(chan, nullptr, nullptr) : (uint32_t)CARD_RESULT_NOCARD);
}

HLE(CARDUnmount) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  // Aurora doesn't track mounts (its CARDUnmount reports NOCARD); unmounting a present card succeeds.
  if (present(chan)) CARDUnmount(chan);
  ret(c, present(chan) ? (uint32_t)CARD_RESULT_READY : (uint32_t)CARD_RESULT_NOCARD);
}

HLE(CARDCheck) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  ret(c, present(chan) ? (uint32_t)CARDCheck(chan) : (uint32_t)CARD_RESULT_NOCARD);
}

HLE(CARDCheckAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  s32 r = present(chan) ? CARDCheck(chan) : CARD_RESULT_NOCARD;
  if (r == CARD_RESULT_READY) callback(arg(c, 1), chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDFormatAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  s32 r = present(chan) ? CARDFormat(chan) : CARD_RESULT_NOCARD;
  if (r == CARD_RESULT_READY) callback(arg(c, 1), chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDFreeBlocks) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  uint32_t bytesP = arg(c, 1), filesP = arg(c, 2);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  s32 bytes = 0, files = 0;
  s32 r = CARDFreeBlocks(chan, &bytes, &files);
  if (bytesP) guest::wr32(bytesP, (uint32_t)bytes);
  if (filesP) guest::wr32(filesP, (uint32_t)files);
  ret(c, (uint32_t)r);
}

HLE(CARDGetSerialNo) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  uint32_t out = arg(c, 1);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  u64 serial = 0;
  s32 r = CARDGetSerialNo(chan, &serial);
  guest::wr64(out, serial);
  ret(c, (uint32_t)r);
}

HLE(CARDOpen) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  const char* name = guest::str(arg(c, 1));
  uint32_t fiP = arg(c, 2);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  CARDFileInfo fi{};
  s32 r = CARDOpen(chan, name, &fi);
  if (r == CARD_RESULT_READY) fileInfoToGuest(fiP, fi);
  LOG_DEBUG("CARDOpen '%s' -> %d", name, r);
  ret(c, (uint32_t)r);
}

HLE(CARDClose) {
  CARDFileInfo fi = fileInfoFromGuest(arg(c, 0));
  ret(c, (uint32_t)CARDClose(&fi));
}

// s32 CARDCreateAsync(s32 chan, const char* name, u32 size, CARDFileInfo*, CARDCallback)
HLE(CARDCreateAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  const char* name = guest::str(arg(c, 1));
  uint32_t size = arg(c, 2), fiP = arg(c, 3), cb = arg(c, 4);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  CARDFileInfo fi{};
  s32 r = CARDCreate(chan, name, size, &fi);
  if (r == CARD_RESULT_READY) fileInfoToGuest(fiP, fi);
  LOG_INFO("memory card: create '%s' (%u bytes) -> %d", name, size, r);
  if (r == CARD_RESULT_READY) callback(cb, chan, r);
  ret(c, (uint32_t)r);
}

// s32 CARDReadAsync(CARDFileInfo*, void* buf, s32 length, s32 offset, CARDCallback)
HLE(CARDReadAsync) {
  CARDFileInfo fi = fileInfoFromGuest(arg(c, 0));
  uint32_t buf = arg(c, 1), cb = arg(c, 4);
  s32 len = (s32)arg(c, 2), off = (s32)arg(c, 3);
  s32 r = CARDRead(&fi, guest::ptr(buf), len, off);
  if (r == CARD_RESULT_READY) callback(cb, fi.chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDWrite) {
  CARDFileInfo fi = fileInfoFromGuest(arg(c, 0));
  s32 r = CARDWrite(&fi, guest::ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3));
  ret(c, (uint32_t)r);
}

HLE(CARDWriteAsync) {
  CARDFileInfo fi = fileInfoFromGuest(arg(c, 0));
  uint32_t cb = arg(c, 4);
  s32 r = CARDWrite(&fi, guest::ptr(arg(c, 1)), (s32)arg(c, 2), (s32)arg(c, 3));
  if (r == CARD_RESULT_READY) callback(cb, fi.chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDFastDeleteAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0), fileNo = (s32)arg(c, 1);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  s32 r = CARDFastDelete(chan, fileNo);
  if (r == CARD_RESULT_READY) callback(arg(c, 2), chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDGetStatus) {
  ensureInit();
  s32 chan = (s32)arg(c, 0), fileNo = (s32)arg(c, 1);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  CARDStat st{};
  s32 r = CARDGetStatus(chan, fileNo, &st);
  if (r == CARD_RESULT_READY) statToGuest(arg(c, 2), st);
  ret(c, (uint32_t)r);
}

HLE(CARDSetStatus) {
  ensureInit();
  s32 chan = (s32)arg(c, 0), fileNo = (s32)arg(c, 1);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  CARDStat st = statFromGuest(arg(c, 2));
  ret(c, (uint32_t)CARDSetStatus(chan, fileNo, &st));
}

HLE(CARDSetStatusAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0), fileNo = (s32)arg(c, 1);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  CARDStat st = statFromGuest(arg(c, 2));
  s32 r = CARDSetStatus(chan, fileNo, &st);
  if (r == CARD_RESULT_READY) callback(arg(c, 3), chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDRenameAsync) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  s32 r = CARDRename(chan, guest::str(arg(c, 1)), guest::str(arg(c, 2)));
  if (r == CARD_RESULT_READY) callback(arg(c, 3), chan, r);
  ret(c, (uint32_t)r);
}

HLE(CARDGetSectorSize) {
  ensureInit();
  s32 chan = (s32)arg(c, 0);
  if (!present(chan)) return ret(c, (uint32_t)CARD_RESULT_NOCARD);
  u32 size = 0;
  s32 r = CARDGetSectorSize(chan, &size);
  guest::wr32(arg(c, 1), size);
  ret(c, (uint32_t)r);
}
