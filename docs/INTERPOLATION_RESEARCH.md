# Above 60 fps: host-side frame interpolation (feasibility, 2026-10-22)

The game advances a fixed step per frame (RenderWare's triple-buffer swap on the VI retrace, no
elapsed-time scaling), so a faster VI only speeds gameplay up. The only way to show more than 60
distinct images per second is to synthesise in-between frames on the host from two consecutive
translated GX streams. This note measures how well two streams line up.

## Method

- `BLITZ_GX_DUMP=<first>,<count>` writes each translated stream (build/gxframe_N.bin), guest MEM1 at
  the end of the frame (gxmem_N.bin) and now **build/gxarrays_N.txt**: one line per vertex-array load
  in stream order (array, guest base, size, XXH3 of the snapshot). The stream carries only the host
  arena pointer, so array contents could not be compared without this. Dump mode only; no change
  when the variable is unset.
- `tools/gxpair.py <a.bin> <b.bin>` / `--range <first> <last>` matches the draws of frame B to frame A
  in three tiers (all keyed on full TEV/blend/z/vertex-format state, bound textures, primitive and
  vertex count):
  1. identical index/inline data and array contents → **same** (matrices equal) or **xf** (only
     position/normal/texture matrices or projection differ),
  2. identical index data, different array contents → **skin** (CPU-rebuilt vertices),
  3. different inline vertex data → **geom** (direct-vertex draws rebuilt by the CPU),
  4. otherwise **new**.
  It also counts how many matched draws keep their relative order (longest increasing subsequence)
  and, with gxmem, checks ambiguous pairs (several draws with the same key) by view-space centroid.
- Captures: soak script, frames 5400-5699 (pre-snap, the snap, a run, the tackle, "NO GAIN"
  banner, camera pan and zoom toward the tackle) and a second run 5500-5559 (players running pre-play
  and after the snap) with guest memory for the position check.

## Results

300 consecutive pairs (5400-5699):

| class | draws | vertices |
|---|---|---|
| same | 1.6% | 1.6% |
| xf (matrices/projection only) | 30.8% | 28.6% |
| skin (same indices, rebuilt arrays) | 66.1% | 68.1% |
| geom (inline vertices rebuilt) | 1.3% | 1.2% |
| new (no counterpart) | 0.2% | 0.5% |

- **99.8% of draws pair 1:1**. 416 draws per frame pre-snap, falling to ~334 as the camera zooms in
  and players leave the frustum. Only 0 to 2 draws are "new" in most frames. The outliers are
  5508 (46 new: players switching mesh LOD as the camera closes in) and 5695 (112 new: the same, at
  the tackle close-up).
- **Draw order is stable**: in 286 of 299 pairs every matched draw keeps its relative order. In the
  rest, up to 20% of draws move (5508, 5569, 5627, 5652, 5676, 5695: player draw lists reordered,
  probably sorted by distance). Nearly everything is blend 0x4BD (src-alpha/inv-src-alpha, z LEQUAL +
  update) or the player shadow pass 0x2FD (dst-colour/inv-dst-alpha, z test without update). Opaque
  pixels don't care about order, so reordering only matters for translucent edges.
- **Players are skinned on the CPU, not with GX matrix palettes**: no draw uses per-vertex
  PNMTXIDX. Each player mesh keeps the same index data from frame to frame, but its position and
  normal arrays (arrays 0/1, f32) are rewritten every frame at a *different* guest address (a
  per-frame vertex heap; ~0.7 MB each per frame). The texcoord arrays stay put. Every player mesh is
  drawn twice (0x4BD colour pass + 0x2FD shadow pass) from the same arrays. So **matrix blending alone
  moves only ~30% of the vertices**: players need per-vertex lerps of positions and normals.
  The arrays are float (VAT fmt 4), so this is a straightforward element-wise lerp over the indices
  both frames use.
- **xf draws**: field, stadium, goalposts, the ball and the non-skinned player parts (n=109/n=14 meshes,
  likely helmets/face masks). Matrices are always loaded directly in the stream (no indexed XF
  loads were seen), so the bridge has both frames' values.
- **Projection** changes on ~400 draws per frame during the play (camera FOV zoom toward the ball
  carrier). The projection blends like any other matrix.
- **geom draws**: 3D direct-vertex batches (n=162/78/120, ~16 per frame once the crowd is in view:
  crowd billboards), the 2D "NO GAIN" banner and clock digits. Vertex counts stay equal, so an inline
  lerp works for the 3D ones. 2D/HUD should simply take frame B.
- **EFB copies**: none in normal play. One frame of 300 (5659) had a single copy. Re-executing the
  copy per presented frame keeps that correct.
- **Pairing ambiguity**: 16,992 ambiguous skin/xf pairs (several draws with the same key) were checked
  by view-space centroid. 5 looked suspicious (0.03%), all in frames where draw order agreed
  completely and the partner sat at the same draw index: these are players in contact (a 4-16 unit
  move with another player's mesh closer), not mispairs. Pairing by "same key, same order" holds
  here. A production version should still use a real identity (below).
- **Camera cuts**: none in these captures. The camera pans and zooms continuously; the median
  per-frame rotation change of world matrices stayed below 0.006. Replays, kick-off/turnover
  cutaways and the play-select transition were not captured, so the cut threshold is uncalibrated.

## Blockers / risks

1. **CPU-skinned players (68% of vertices)**: vertex lerping is needed, not just XF blending. It's
   tractable (same indices, float arrays, ~45k vertices per frame), but it means the bridge must keep
   both frames' array snapshots and emit a third, blended array per skinned mesh.
2. **Draw identity**: heuristic pairing worked here (0 real mispairs), but players who swap places in
   a reordered list with identical team textures could morph into each other. Mitigation: tag draws
   with the guest RpAtomic/clump pointer through a pre-hook on the atomic render callback (the
   bridge parser already runs inside the game's FIFO writes, as `BLITZ_GX_DRAWSTACK` shows).
3. **Pops**: LOD switches and frustum entry/exit appear in frame B only and pop at the in-between
   frame, just as they do at 60 fps. That's acceptable.
4. **Latency**: interpolating between A and B means showing B one game frame late: about +16.7 ms of
   input latency.
5. **Cost**: Aurora processes each game frame 2x (120 Hz) or more. Pipelines and textures are
   already cached, so the cost is mostly CPU stream processing plus GPU. The existing occasional
   33 ms frames come from conversion stalls and would show as judder at 120 Hz.
6. **Uncaptured cases**: camera cuts, replays, the pause/play-select overlays, kick-offs, halftime.
   They need a cut detector and a capture of each before shipping.

## Verdict

**Go for a time-boxed prototype** (about 1 week, behind a default-off `frame_interp` setting).
Streams line up far better than feared: 99.8% of draws pair 1:1, order is stable, there are
essentially no EFB copies, and everything that moves is float matrices or float vertex arrays. The
real extra work over "blend XF matrices" is the CPU-skinned player arrays, and that is mechanical.
Stop if the prototype shows player morphing that atomic-pointer tagging can't fix, or if doubling
Aurora's per-frame work makes 60 fps itself hitch on the dev laptop.

## Design sketch

Where: `gx_bridge.cpp` (stream side) and `main_window.cpp` (presentation), active only in a match
(`widescreen::inMatch()`) and only for perspective draws.

1. **Per-draw records**: while translating frame N, record for each draw its stream offset, a
   match key (state hash, texture identity, primitive, count, index-data hash, later the atomic
   pointer), the XF matrix words it reads (position/normal by 0x1018 index, texture matrices,
   projection 0x1020-0x1026) and its array snapshots (pos/nrm pointer, stride, used index range).
2. **Keep two frames**: a second arena so frame N-1's array snapshots and records stay valid while N
   is presented (today `m_arena.reset()` drops them after Aurora consumes a frame). Textures are read
   in place from guest memory, which is safe for in-match frames (no copies, rare changes).
3. **Match**: same tiered match as `tools/gxpair.py` (hash map on the key, FIFO per key, order
   preserved), plus atomic-pointer identity once that exists.
4. **Build the in-between stream** for t in (0,1): copy frame B's stream. Before each matched 3D draw,
   emit its matrices blended (lerp the 3x4 position matrix, renormalise the normal matrix;
   use polar/quaternion decomposition only if large rotations show artefacts) and the blended
   projection. Emit them as one block, as Aurora needs for 0x1020-0x1026. For skin/geom draws,
   write lerp(A,B,t) of the used position/normal range into the arena and emit
   `GX_AURORA_LOAD_ARRAYBASE` to it (inline vertex data is patched in the copied stream). Unmatched
   draws and all 2D draws are emitted from B unchanged.
5. **Camera cuts and discontinuities**: skip interpolation (show B twice) when the projection jumps
   beyond a threshold, when the median matched-world-matrix change is large, when under ~90% of 3D
   draws match, or when the frame isn't in a match. Per-draw fallback: if a pair's centroid moved
   more than a few yards in one frame (respawn, teleport), draw it from B.
6. **Presentation**: the main loop already presents on the retrace tick. With interpolation, hold
   game frame B, present lerp(A,B,0.5) at the half-tick and B at the tick (120 Hz). For other
   refresh rates use t = elapsed fraction of the 16.68 ms period. VSync/VRR on; the game thread
   stays locked to the 59.94 Hz retrace, so gameplay speed is unchanged.
7. **Settings/UI**: `frame_interp=off|on` in settings.ini and the F1 menu, plus an FPS-counter
   readout of interpolated vs real frames, and `BLITZ_INTERP_DEBUG` to tint unmatched/new draws.

### Effort

| piece | estimate |
|---|---|
| two-frame records + arena, matching | 2-3 days |
| in-between stream: XF blend + projection | 2 days |
| skinned/inline vertex lerp | 2 days |
| presentation at 120/144 Hz, pacing, latency check | 2-3 days |
| cut detection + captures of replays/cutaways/menus, atomic tagging | 3-5 days |
| settings, polish, artefact hunting | 3-5 days |

**About 2.5-4 weeks in all**, with a first visible 120 Hz prototype after about a week. Artefact
risk: medium-low for gameplay; unknown for cutscene-style camera work until those are captured.

## Reproducing

```
powershell -ExecutionPolicy Bypass -File tools/shot.ps1 -Frame 5700 -Env "BLITZ_GX_DUMP=5400,300|BLITZ_SHOT=5400-5700/5" -Timeout 900
python -I tools/gxpair.py --range 5400 5699
python -I tools/gxpair.py build/gxframe_5507.bin build/gxframe_5508.bin --list
```

Each dumped frame also writes 24 MB of gxmem; delete them afterwards (they're only needed for the
position check). Runs aren't deterministic, so frame numbers land on different moments of the play
from run to run.
