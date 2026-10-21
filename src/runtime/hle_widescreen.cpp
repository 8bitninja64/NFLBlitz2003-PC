// Widescreen support that depends on the game (widescreen.h).
//
// Hor+: Bridge::xf multiplies the X terms of the game's perspective projections by (4/3)/aspect, so in
// a match the 3D view is aspect*3/4 times wider than the 4:3 view the game set up. The game culls against
// its own 4:3 RenderWare frustum, so at the sides of a 16:9 screen players and their shadows (801E0C8C),
// crowd billboards (801D24C8), stadium and field pieces (800F3A7C) and world sectors (801EF7DC) were
// dropped: players vanished when the camera swung after a tackle, the crowd popped in and out.
#include "widescreen.h"

#include "hle.h"
#include "hw.h"
#include "sched.h"
#include "settings.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

namespace widescreen {

// Blitz Proc (80033C1C) stores the "Game structure" at 0x80283898 while a match is loaded (0 in the
// front end, attract and loading screens); its +0xC is the one-hot match state driven by Game Proc
// 80046A48. The game's own in-match test (80060E88) is ptr != 0 && state != 0x80000 (halftime show).
// The end-of-quarter coaching tips screen runs in state 0x200000 with the per-frame match tick
// (0x802DCA08) cleared; pausing is impossible in that state, so a zero tick there means the tips.
// The pause menu and play select stay "in match": they are drawn over the (frozen) 3D field.
bool inMatch() {
  uint32_t game = guest::rd32(0x80283898);
  if (!game) return false;
  uint32_t state = guest::rd32(game + 0x0C);
  if (state == 0x00080000) return false;
  if (state == 0x00200000 && guest::rd32(0x802DCA08) == 0) return false;
  return true;
}

bool inQuarterBreak() {
  uint32_t game = guest::rd32(0x80283898);
  return game && guest::rd32(game + 0x0C) == 0x00200000;
}

}  // namespace widescreen

namespace {

// RenderWare camera (this build): +0x68 viewWindow, +0x98 frustum planes (6 x 0x14: normal, distance,
// 3 normal-sign bytes), +0x110 frustum bounding box, +0x128 frustum corners. 80201AEC builds the
// corners and planes from viewWindow (only the side planes 2 and 4 depend on viewWindow.x); the camera
// sync 80203194 then derives the bounding box from the corners. The view matrix and the projection the
// game sends to GX come from recipViewWindow (+0x70) and never see a change to viewWindow.
constexpr uint32_t kCamViewWindowX = 0x68;
constexpr uint32_t kCamPlanes = 0x98;
constexpr uint32_t kPlanesWords = 6 * 0x14 / 4;
constexpr uint32_t kBuildClipPlanes = 0x80201AEC;
constexpr uint32_t kFrustumTestSphere = 0x80203570;  // RwCameraFrustumTestSphere

using Planes = std::array<uint32_t, kPlanesWords>;
std::unordered_map<uint32_t, Planes> g_planes4x3;  // camera -> the game's own planes (for game logic)

Planes readPlanes(uint32_t cam) {
  Planes p;
  for (uint32_t i = 0; i < kPlanesWords; i++) p[i] = guest::rd32(cam + kCamPlanes + 4 * i);
  return p;
}
void writePlanes(uint32_t cam, const Planes& p) {
  for (uint32_t i = 0; i < kPlanesWords; i++) guest::wr32(cam + kCamPlanes + 4 * i, p[i]);
}

// How many times wider than the game's 4:3 view the bridge draws perspective scenes right now
// (Bridge::xf: X terms times (4/3)/aspect unless the frame is a stretched front-end menu). The
// bridge latches menu mode per frame; a one-frame difference at a menu/match switch is harmless.
float horPlusWidth() {
  const settings::Settings& st = settings::get();
  if (!st.widescreen || (st.stretchMenus && !widescreen::inMatch())) return 1.0f;
  return settings::g_displayAspect.load() * 0.75f;
}

// Runs guest function fn (its pre-hook made a no-op through `nested`) with interrupts held off, so no
// other guest code sees the camera in between. fn must be pure computation (no waits).
void callNested(guest::Ctx& c, uint32_t fn, bool& nested) {
  uint32_t ee = c.msr & sched::kMsrEE;
  c.msr &= ~sched::kMsrEE;
  nested = true;
  guest::callIndirect(c, fn);
  nested = false;
  c.msr |= ee;  // pending interrupts are taken at the caller's next back-edge
}

}  // namespace

// 80201AEC(camera): builds the frustum corners and planes (from CameraSync, whenever the camera frame
// moved). Builds the game's own planes first and keeps them for game logic, then builds the culling
// frustum of the Hor+ view (viewWindow.x widened only while the builder runs), which every renderer
// that culls then uses. BLITZ_NO_WIDECULL=1 keeps the game's 4:3 culling; BLITZ_WIDECULL_AB=1 does
// so on odd frames only (neighbouring frame shots then differ only in culling).
HLE(WideCullFrustum) {
  static bool nested = false;
  if (nested) return;  // the inner calls: r0 stays 0, the original body runs
  static const bool off = getenv("BLITZ_NO_WIDECULL") != nullptr;
  static const bool ab = getenv("BLITZ_WIDECULL_AB") != nullptr;  // 4:3 culling on odd frames only
  uint32_t cam = c.r[3];
  float width = off || (ab && (hw::g_framesBuilt.load() & 1)) ? 1.0f : horPlusWidth();
  if (!(width > 1.0f)) {
    g_planes4x3.erase(cam);
    return;
  }
  callNested(c, kBuildClipPlanes, nested);
  g_planes4x3[cam] = readPlanes(cam);
  uint32_t vwx = guest::rd32(cam + kCamViewWindowX);
  guest::wr32(cam + kCamViewWindowX, guest::f32Bits(guest::asF32(vwx) * width));
  c.r[3] = cam;
  callNested(c, kBuildClipPlanes, nested);
  guest::wr32(cam + kCamViewWindowX, vwx);
  c.r[0] = 1;  // done
}

// 80203570 RwCameraFrustumTestSphere(camera, sphere): rendering callers (stadium atomics 800F3A7C, the
// jumbotron feed 8003944C) use the Hor+ planes; the two gameplay callers (off-screen pass target flag
// in 8002EF38, off-screen catch-up speed in 8006A464) get the game's own 4:3 planes, so the game plays
// as on hardware.
HLE(FrustumTestForLogic) {
  static bool nested = false;
  if (nested) return;
  if (c.lr != 0x8002FA54 && c.lr != 0x8006A724) return;
  auto it = g_planes4x3.find(c.r[3]);
  if (it == g_planes4x3.end()) return;
  uint32_t cam = c.r[3];
  Planes wide = readPlanes(cam);
  writePlanes(cam, it->second);
  callNested(c, kFrustumTestSphere, nested);
  writePlanes(cam, wide);
  c.r[0] = 1;  // done: r3 holds the test's result
}
