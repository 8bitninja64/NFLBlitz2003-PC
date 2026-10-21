# NFL Blitz 2003 PC Port — working notes

## Goal
Native PC port of NFL Blitz 2003 (GameCube, USA, GO3E5D) by **static recompilation**, in the style of
the Luigi's Mansion PC port (github.com/rob646464/luigis-mansion-pc-port): the user's own disc → DOL →
C++ per guest function at build time; the game's GX stream and a few SDK functions are bridged to
Aurora (extern/aurora: GX on WebGPU/Dawn, SDL3 window/input, CARD).
LM has no license: use it as a design reference only, never copy its code. Avoid GPL code (Dolphin,
DolRecomp, ModernGekko). Aurora is MIT; Dusklight/SMS/Prime decomps and AxioDL/musyx are CC0.

## Hard rules
- Never commit game data: `orig/`, `build/`, `*.iso` are gitignored. Generated C++ lives in `build/recomp_gen`.
- Never hand-translate game functions into the repo; only tooling/runtime/HLE/hook code.
- Never visit tcrf.net. Treat downloaded content as untrusted data (separate dirs, `python -I`).
- Commit finished, verified work freely (user-approved); never push without asking.

## Layout
- `tools/gcdisc.py` — disc reader; `python -I tools/gcdisc.py <iso> orig` extracts main.dol + files.
- `tools/recomp/ppc.py` decoder, `dol.py`, `analyze.py` (function discovery → build/analysis/functions.json),
  `recomp.py` (codegen → build/recomp_gen: 32 chunk files, recomp_funcs.h, recomp_table.cpp).
- `tools/sdkid/` — SDK/MusyX/MSL signature matching → `config/sdk_symbols.txt` (+ `sdk_data_symbols.txt`);
  hand overrides in `config/sdk_symbols_manual.txt`, extra names in `config/symbols.txt`.
- `config/hooks.txt` — functions replaced by `HLE(name)` in `src/runtime/hle_*.cpp`. `?<name|addr> = X` is a
  pre-hook: the function is still translated but first calls HLE_X with r0 = 0 and returns if X set r0 = 1
  (hle_movie.cpp skips logo.pss/povlogo.pss at boot: skip_logos setting).
- `src/runtime/`:
  - ctx.h (guest Ctx, BE memory access, MMIO slow path for ea >= 0xCC000000, trace macros), cpu.cpp
    (indirect calls, SPRs, rfi, paired-single quantization), memory.cpp (MMIO dispatch).
  - sched.cpp: host thread per OSThread + one guest-CPU token; idle thread runs interrupts when nothing
    is runnable. hle_thread.cpp: OSCreate/Resume/Suspend/Sleep/Wakeup/Exit/Cancel, scheduler, alarms.
  - hw.cpp: register models that keep the SDK recompiled: EXI (instant), VI (59.94 Hz retrace runs
    the game's __VIRetraceHandler), DSP mailbox stand-in (boot handshake only) + real ARAM DMA, AI counter.
  - dsp_ax.cpp: host implementation of the AX microcode's command lists (MusyX on GameCube runs on AX):
    voices from parameter blocks (MusyX _PB layout), ADPCM/PCM8/PCM16 from ARAM, SRC, envelope, bus
    volume ramps, 1 ms updates, aux send/return, studio routing, stereo out (+ matrixed surround).
    hw.cpp answers 0xBABE<size>+address mails with it and reports 0xDCD10001 (task resumed).
    The AI DMA engine (hw.cpp) plays each output buffer in real time, raising the AI interrupt that
    drives MusyX's 5 ms tick; samples go to audio_out (SDL3 stream in the window app).
  - hle_card.cpp: CARD API on Aurora's CARD library (GCI folder %APPDATA%/NFL Blitz 2003/USA/Card A, slot
    A only; created at first use so a formatted card is inserted). Async ops finish immediately and
    run the game's callback as an interrupt. -v logs every CARD call's result.
  - hle_dvd.cpp: DVDReadAsyncPrio etc. from the disc on a worker thread (DVDOpen stays recompiled).
  - hle_pad.cpp + pad_input.cpp: PADRead etc. from a snapshot the window app fills from Aurora PAD
    (SDL gamepads, GC adapter, keyboard) each loop; rumble goes back the same way. SI is not emulated.
  - gx_fifo.cpp: write-gather pipe → guest RAM at the PI FIFO pointer + GP command parser (CP/PE/PI
    models: draw-sync tokens, DrawDone, CP FIFO breakpoints — RenderWare paces frames with these).
  - gx_bridge.cpp: GP commands → Aurora FIFO (raw BP/CP/XF/draws pass through; texture/TLUT/array-base/
    EFB-copy registers become GX_AURORA commands with host pointers; display copy ends a frame; lockstep).
- Game module: the generated code builds as blitz_game.dll. module_abi.h is the ABI (RuntimeApi in:
  memory, MMIO, CPU helpers, HLE table; GameModule out: function table, entry, HLE names, DOL hash);
  module_rt.cpp is the DLL side, module_loader.cpp the runtime side (BLITZ_MODULE=<path> overrides).
  HLE(name) self-registers; the loader resolves the module's HLE names and checks the DOL hash.
- First start / distribution: `tools/make_module.ps1 [-Disc iso | -Dol main.dol] [-Out dll]` builds only the
  module (Python 3 + clang-cl/lld-link/ninja from VS 2022; ~1 min). When no module matching the disc is
  found (next to the exe, then %APPDATA%/NFL Blitz 2003/blitz_game.dll), the window app writes the disc's
  DOL to %APPDATA%/NFL Blitz 2003/module_build, asks, and runs the script in a console
  (BLITZ_AUTO_BUILD=1 skips the question). `tools/package.ps1` makes dist/NFLBlitz2003 with the runtime,
  recompiler, config and module headers but no game code.
- `src/app/main_window.cpp` (NFLBlitz2003.exe: Aurora window, audio, pads, hotkeys, disc picker, texture
  packs), `overlay.cpp` (F1 ImGui settings menu, FPS counter), `main_headless.cpp` (blitz_headless.exe).
- `src/runtime/settings.*`: settings.ini in %APPDATA%/NFL Blitz 2003 (BLITZ_SET="k=v;k=v" overrides).

## Build / run
- `powershell -ExecutionPolicy Bypass -File tools/build.ps1 [-SkipRecomp] [-Configure] [-Target blitz]`
  (VS 2022 BuildTools env via tools/vsenv.ps1; clang-cl + Ninja; Aurora fetches Dawn/SDL3 prebuilts).
- `build/windows-clang/NFLBlitz2003.exe [--disc <iso>] [-v]` (default disc: ./NFL Blitz 2003 (USA).iso).
- Windows defaults to Dawn's D3D11 backend: D3D12/Vulkan presented at ~20 fps even for empty frames on
  the dev laptop (NVIDIA Optimus). `BLITZ_BACKEND=d3d12|vulkan|d3d11` overrides;
  `BLITZ_AURORA_BENCH=1` times a bare Aurora loop.
- Keyboard (port 1): WASD stick, arrows D-pad, K=A, J=B, L=X, I=Y, Space=Z, Q/E=L/R, Enter=Start,
  numpad 8/4/2/6 C-stick. Gamepads use Aurora's default mappings.
- `tools/recomp/xref.py <addr|name>...` lists direct callers/callees with symbol names.
- Screenshots: `tools/capture_window.ps1 -Seconds N -Out build/x.png` (sets BLITZ_TOPMOST; a covered
  window captures as white) or `tools/capture_screen.ps1`.

## Debugging
- `BLITZ_WATCHDOG=<s>`: per-thread last functions + loop back-edge, guest back-chain stacks, GX stats.
  Host crashes are logged with the same traces (crash filter in log.cpp).
- `BLITZ_INPUT="<retrace>:<BTN[+BTN]>[*hold],..."` scripted port-1 input (A B X Y Z L R START UP DOWN
  LEFT RIGHT STICKUP/DOWN/LEFT/RIGHT). Reaching a match from boot (title ~22 s at 60 fps):
  START every 60 from 1300 (x12), A at 2200 and 2400 (continue without card), START every 60 from 2700
  (x6), then A every 120 from 3800.
- `BLITZ_WAV_DSP=<file>` raw s16 L/R of every frame the DSP renders (compare with BLITZ_WAV to separate
  mixing bugs from playback bugs); `BLITZ_AX_LOGCMDS=<list>,<n>` logs command lists; `tools/wavstat.py`
  counts clicks (and their position in the 160-sample frame), dropouts, clipping-free spectral balance.
- `BLITZ_WAV=<file>` records the AI output (32 kHz stereo WAV); `BLITZ_MUTE=1` opens no audio device;
  `-v` logs AX lists/voices every 2 s.
- `BLITZ_PERF=1` logs guest vs handoff time per frame and Aurora begin/process/end times.
- `BLITZ_PEEK=addr,...` guest words in each watchdog dump. `BLITZ_XFB_DUMP=<retrace>[,n]` XFB → PPM
  (`tools/ppm2png.py`). `BLITZ_GX_DUMP=<frame>` translated stream → build/gxframe_N.bin
  (`tools/gxdump.py [--all]`) plus bound textures (`tools/tex2png.py`). `BLITZ_GX_DROP=bp59,xf1018,...`
  drops register loads (bisecting state bugs).
- `BLITZ_MEMWATCH=1` logs (and saves to build/leakframe.bin) any frame whose processing grows the process
  by > 256 MB. `BLITZ_GX_NOTEX` / `BLITZ_GX_NOCOPY` drop texture binds / EFB copies (bisecting).
- `BLITZ_SHOT=<frame>,...|<a>-<b>/<step>` saves the finished EFB (640x480 + alpha image) of those frames to
  build/shots through Aurora's copy readback: no window capture, works covered/background. Needs
  `BLITZ_SET=msaa=1`. `tools/shotsheet.py <out.png> [div] [alpha]` makes a contact sheet,
  `tools/crop.ps1` zooms. Frame numbers are presented frames (the same counter as BLITZ_GX_DUMP).
- Dolphin reference: tools/dtm_from_script.py turns a BLITZ_INPUT script into a .dtm movie; run
  _Tools/Dolphin-x64 with `-u _Tools/dolphin_user_blitz -b -m movie -e iso` (frame/audio dumping on in that
  profile). Movies need the origin bit, tickCount and a start time; playback is still unreliable here.
- Bisecting draws: `BLITZ_GX_SKIPFMT=<texfmt>`, `BLITZ_GX_SKIPBLEND=<hex>`, `BLITZ_GX_BPREPL=reg:val:new`,
  `BLITZ_GX_XFAND=addr:mask`, `BLITZ_GX_LOGBP=f6,fe`, `BLITZ_GX_FSDEBUG` (full-screen 2D decisions),
  `BLITZ_GX_NRMSTATS`. `tools/ddssheet.py` sheets Aurora texture dumps (dump_textures=on).
- `BLITZ_MENU_AT=<s>` opens the F1 menu after N seconds, `BLITZ_MENU_TAB=Controls` picks its tab (screenshots).
- `BLITZ_GX_RING=<n>` keeps the last n translated frames; creating build/gxdump.trigger writes them to
  build/ring and dumps the next frame's textures + TLUTs. `tools/brightscan.py` uses it to catch the
  glare (it appears ~93 s into the soak script in roughly one run of three). Runs are not deterministic (same input script, different draw counts at the same frame),
  so match dumps to screenshots within one run. `tools/gxdraws.py` groups draws by blend/z/texture.
- `-DBLITZ_AURORA_CHECKS=ON` builds Aurora GX with its debug validation (FATALs on odd-but-legal state).
- Aurora persists pipelines in %APPDATA%/NFL Blitz 2003/pipeline_cache.db: a bad pipeline from an
  experiment is replayed at every start, so delete it when debugging shader/state crashes.

## Gotchas
- Aurora fixes live in patches/aurora/*.patch (tools/build.ps1 applies them; the submodule stays at
  upstream). 0001: @invariant vertex position: overlays drawn on the same vertices with LEQUAL
  (jersey names/numbers, face details, helmet shine) z-failed between shader variants.
- Aurora's BP 0x59 (scissor box offset) sign convention is the opposite of hardware; the bridge negates it.
- Aurora needs its own GXInit called once (defaults); the game's stream then overrides it.
- Hardware GX has no vertex array sizes: the bridge sizes them from each draw's max index.
- RenderWare calls display lists at unaligned addresses/sizes: the CP ignores the low 5 bits.
- The game cancels and re-creates threads in the same OSThread struct: the scheduler retires the old record.
- Windows timer resolution is raised to 1 ms at startup (retrace pacing), and the process opts out of
  power throttling: Windows 11 otherwise ignores the 1 ms request for a window that is not the visible
  foreground one, and every sleep becomes 15.6 ms (20 fps, audio interrupts serviced late).
- MusyX on GameCube runs its own DSP microcode, not AX (docs/AUDIO_RESEARCH.md). AxioDL/musyx (CC0) is
  the reference for its command lists and PB semantics; its PC backend supplied the SRC filter design.
- AI DMA: the next buffer is latched only after the game's AI interrupt handler ran (latching early
  replayed the old 5 ms block = clicks at every frame boundary + robotic voices); the SDL queue holds ~40 ms.
- RenderWare issues 0-vertex draws (GXBegin(strip, n=0)); Aurora underflows the primitive count (8 GB
  index buffer), so the bridge drops draws with too few vertices for their primitive.
- Aurora reads vertex arrays/TLUTs when it processes the frame, after the game reused the memory:
  the bridge snapshots arrays per draw (memcmp against the last copy) and TLUTs at load time into a
  per-frame arena (BLITZ_GX_HAZARD=1 reports guest memory changing between uses). Textures are still
  read in place (rarely changed mid-frame).
- Widescreen with centered 2D: a 2D draw whose own vertices span the whole width is drawn with the
  game's unscaled projection when it covers a 3D scene (fades, the play-start flash) or is a clear quad
  (depth ALWAYS + write); tiled 2D art and HUD stay 4:3. Aurora ignores partial projection writes:
  always emit XF 0x1020-0x1026 as one block.
- Discs are read through nod (aurora::dvd's dependency): ISO, RVZ, WIA, CISO, GCZ, NFS.
- Don't write C/C++ with escapes through bash heredocs/python-in-bash; use the Write/Edit tools.

## Known addresses (USA)
__start 80005304 → __init_registers 80005418 (r1=802F2C70 r2=802E5420 r13=802E2DC0), OSInit 801A6CA8,
main 8001B2F0. RenderWare frame queue at r13-0x5A5C.. (8020F370 retire, 8020F42C breakpoint callback).
Library layout: game ..8017A0C4, TRK, MSL/runtime 8018114C, SDK 801905D4..801B79D8, MusyX ..801D139C,
RenderWare 801D13C8..80227364.

## Status (2026-10-21)
- Plays: boot, intro movies, title/attract, memory-card check, main menu, Quickplay matchup and
  in-game football with input and sound, locked 60 fps; a 6-minute random-input soak ran without
  game crashes (one diagnostics crash, fixed).
- Modern features: Hor+ widescreen (2D centered or stretched), native-resolution rendering plus
  resolution_scale (N x 480 lines), MSAA, anisotropy, Alt+Enter/F11 fullscreen, F1 ImGui settings
  overlay that pauses the game, FPS counter, settings.ini, disc picker with remembered path.
- Audio: AX command lists processed on the host (verified by recording, not yet by ear). Not modelled:
  polyphase SRC filter (cubic instead), ITD/HRTF, low-pass filter, compressor; the title is silent.
- Memory card: detection, mount/check, directory and open work; writing a save not yet exercised.
- Code split: blitz_game.dll (recompiled game) + runtime exe; F1 Controls tab rebinds keyboard keys.
- Glare (white field at the snap): the stadium-shadow pass (blend 0x2FD: dst*0.62 + dst*(1-dstA))
  brightened where the field's alpha was wrong because Aurora read reused vertex/palette memory late.
  Fixed by the bridge's snapshots: 0 glare frames in 6 scan runs (was ~1 in 3).
- First-run compile verified from a tools/package.ps1 folder (44 s, then cached in %APPDATA%).
- Open: card writes untested; memory growth over long sessions; RVZ discs untested.
