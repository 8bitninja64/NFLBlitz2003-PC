# Tooling research: NFL Blitz 2003 (GO3E5D) static recompilation

Researched 2026-10-20. Licenses and activity come from GitHub metadata on that date.
Priorities: **must-use**, **useful**, **skip**. "Reference only" means we may read the
material to understand behaviour but must not copy its code into the port, either because
it has no license (all rights reserved) or because it is GPL.

---

## 0. Facts we confirmed from our own disc (read-only inspection)

These findings change which tools matter, so they come first.

| Finding | Evidence | Consequence |
|---|---|---|
| **The game is built on RenderWare Graphics (Criterion)** | DOL strings: `RpGeometry`, `RwGameCubeChangeRenderMode: waiting for frame`, `RwGameCubeGetXFBs`, `RxInterpolants.csl`, `RwSupportMalloc()`, raster-conversion errors, many `*.dff` model names, `*.rtd` texture dictionaries (`ps_tdict.rtd`) | A large part of the DOL is statically linked RenderWare, and RenderWare's GameCube driver sits on top of GX. RenderWare resources (sections 5 and 7) become relevant. Sound and video signature work alone will not name this code. |
| **The GameCube port was likely done by Point of View, Inc.** | `files/PovLogo.avi` (internal stream name `PovLogo_new.avi`) | Other Point of View and Midway GameCube ports from 2002–2003 may share engine code. |
| **The engine descends from the arcade game** | `Blitz2k3::main.c: target_init() failed.`, plus `.cap` camera files, "drone" and "cambot" terminology | The leaked arcade source for Blitz 2000 (section 7) is a good structural reference. |
| **The SDK dates from about spring 2002** | `Dolphin OS $Revision: 52 $`, `HIO built: Mar 28 2002`, build dates `Dec 17 2001` and `Apr 16 2002`, `DSPInit(): Build Date:`, MetroTRK, "Metrowerks CW runtime library initializing default heap" | The closest permissively licensed SDK references are the Super Mario Sunshine decomp (July 2002) and the Metroid Prime decomp (late 2002). |
| **Audio is MusyX behind Midway's own wrapper layer** | `mslMusyX.c`, `mslMusyXStream.c`, `mslMem.c`, `mslAlloc`, `MusyX DMA Stack`, `*.msb` per-team banks | MusyX is present. Midway's "msl" layer is game code; the MusyX library underneath is what the decomp in section 4 covers. |
| **FMVs use the MidiVid VQ codec in AVI** | `strh` fccHandler `mvdv`, `strf` biCompression `MVDV`, 480x360. Audio wFormatTag `0x0069` with an `iadp` extension, stereo, 48 kHz (44.1 kHz in PovLogo) | FFmpeg decodes MVDV (`libavcodec/midivid.c`, LGPL-2.1). FFmpeg maps 0x0069 to ADPCM_IMA_XBOX; whether that matches this `iadp` variant still needs testing. The game also has its own `MoviePlayer`/`MoviePlayerBase` classes, so the recompiled decoder may simply work (section 8). |
| **The `.zip` files are ordinary PKZIP archives (stored entries); `.ziH` is a small binary index** | `teamdata.zip` begins `PK\x03\x04`, method 0, entries such as `ac.rsc`. `teamdata.ziH` begins with a big-endian count (0x21), then per-entry offset and size words | Asset extraction needs no special tool. Any zip reader works, and the recompiled game reads its archives itself. |

---

## 1. Binary analysis: symbols, splits and relocations for a stripped DOL

### decomp-toolkit (dtk): **must-use as a cross-check and data-mining tool**
- **URL:** https://github.com/encounter/decomp-toolkit
- **License:** MIT or Apache-2.0 (dual)
- **Maturity:** Very mature; it is the standard tool for every current GameCube and Wii decomp. Latest release v1.8.4 (2026-09).
- **What it does:** `dtk dol config` produces an initial project config from a DOL. `dtk dol split` and the analysis behind it do the following:
  - **Function boundary analysis** using control-flow analysis, with heuristics that separate tail calls from in-function branches (`src/analysis/cfa.rs`, `tracker.rs`).
  - **Relocation reconstruction** through a small VM and register tracker that follows `lis`/`addi` pairs and r2/r13 small-data-relative accesses. It rebuilds every code→data and data→code pointer, including jump tables and vtables.
  - **Signature matching** against a built-in database of about 141 byte-masked signatures with relocation chains (`assets/signatures/*.yml`). These cover boot, runtime, OS, DVD, EXI, SI, TRK and printf-family code, such as `__start`, `__init_cpp_exceptions`, `OSInit`, `DVDInit`, `GXInit` and `__OSReschedule`. Their main job is to find the section layout and the SDA bases. **The database is not a full SDK identifier.**
  - **extab/extabindex parsing** for C++ exception tables (`util/extab.rs`). Blitz is C++, so every function that has an exception-table entry gets an exact start and size for free.
  - Output is `symbols.txt` (`fn_80xxxxxx` names with sizes and types, plus data objects with sizes) and `splits.txt`. With a `.map` file, real names are filled in.
- **How it helps us:**
  1. Run `dtk dol config` and `split` on `orig/main.dol` and **diff dtk's function list against our Python function-boundary finder**. Every disagreement is a likely bug on one side, especially tail calls, `blr`-less functions and padding.
  2. Harvest **relocations**. Treat every data word dtk marks as a pointer to `.text` as an *address-taken function*, which becomes an entry in our indirect-call table. Jump tables are recovered too. This removes most of the "indirect call to unknown address" failures that the Luigi's Mansion port had to patch around.
  3. extabindex gives authoritative function sizes.
  4. dtk's `symbols.txt` format is what all the CC0 decomps use, so adopting it lets us import names from those projects later.
- **Caveats:** It is a Rust CLI, so we use it as an external tool and do not link it. Its relocation output usually needs some manual fixing in data sections. That matters for producing relinkable objects but much less for our use.

### dtk-template: **useful**
- **URL:** https://github.com/encounter/dtk-template (CC0-1.0)
- A ready-made `configure.py`/ninja project that drives dtk (`config.yml`, `symbols.txt`, `splits.txt`, `build.sha1`). Use it only as the **harness to run dtk analysis** on the DOL. We do not need a matching build. `docs/getting_started.md` describes the stripped-DOL workflow.

### objdiff: **useful (for the SDK-matching agent)**
- **URL:** https://github.com/encounter/objdiff (Apache-2.0, mature)
- An instruction-level diff tool that ignores relocations. It is useful for checking a suspected SDK match by diffing our DOL slice against an object compiled from a CC0 decomp. It is not needed for recompilation itself.

### ppcdis: **skip**
- **URL:** https://github.com/SeekyCt/ppcdis (MIT). Its README says it is effectively superseded by dtk.

### decomp.me and wibo: **useful only for building signatures (see section 3)**
- https://github.com/decompme/decomp.me (MIT) hosts the Metrowerks GameCube compilers (mwcc GC 1.x–3.0). https://github.com/decompals/wibo (MIT) runs those Win32 compilers on Linux; on Windows they run natively.
- **Caveat:** The compilers themselves are proprietary Metrowerks binaries.

### Ghidra with Ghidra-GameCube-Loader: **useful (interactive reverse engineering)**
- Ghidra: https://github.com/NationalSecurityAgency/ghidra (Apache-2.0)
- Loader: https://github.com/Cuyler36/Ghidra-GameCube-Loader (Apache-2.0, maintained 2026). It loads DOL and REL files, maps the memory regions and hardware registers, applies `.map` symbols, and has a paired-single disassembly and decompiler extension.
- **Use:** Investigating game logic, Midway's "msl" sound wrapper, and the `.ziH`/`.rsc`/`.rtd` loaders. A small script that imports our `config/symbols.txt` is worth writing.
- **Ghidra Function ID databases:** We found **no public FID database for the Dolphin SDK**. FID could be generated from objects built out of CC0 decomps (see section 3), but dtk-style masked signatures are simpler.

---

## 2. Existing GameCube/Wii static recompilers and PC-port runtimes

| Project | URL | License | Maturity | Verdict |
|---|---|---|---|---|
| **Luigi's Mansion PC port** | https://github.com/rob646464/luigis-mansion-pc-port | **No license file** (third-party parts listed in NOTICE.md) | Active, about 97 stars, ships releases | **Must-use as a design reference; do not copy code.** See below. |
| **Dusklight** (Twilight Princess source port) | https://github.com/TwilitRealm/dusklight | **CC0-1.0** | Completed source port, about 6k stars | **Must-use: code is reusable.** |
| **Aurora** | https://github.com/encounter/aurora | MIT | Powers Metaforce, Dusklight and the LM port | **Must-use** (already chosen) |
| Metaforce | https://github.com/AxioDL/metaforce | MIT | Mature | Useful reference for how Aurora is integrated |
| XenonRecomp / XenosRecomp | https://github.com/hedge-dev/XenonRecomp | MIT | Very mature (Unleashed Recompiled) | Useful design reference (see below) |
| DolRecomp + ModernGekko | https://github.com/ExpansionPak/DolRecomp | **GPL-3.0** | Many forks and game projects, uneven quality | Skip (GPL) |
| RingOut | https://github.com/jackpoison-prog/RingOut | **GPL-2.0**, runtime derived from Dolphin | Active | Skip (GPL, uses Dolphin code) |
| NWiiRecomp | https://github.com/BlackLineInteractive/NWiiRecomp | Unclear; bundles Dolphin's `totaldb.dsy` | Early | Skip |
| GekkoAOT, gcrecomp (sp00nznet), gcn-recomp, Porpoise-Tool | various | Unclear or none | Early, mostly autogenerated | Skip |
| GameCubeRecompiled (Rust) | https://github.com/KaiserGranatapfel/GameCubeRecompiled | CC0 | Experimental | Skip |

**Luigi's Mansion port: the closest blueprint, but reference only**
- From `docs/RECOMPILER.md`, `docs/AUDIO.md` and `NOTICE.md`:
  - Its recompiler is written in C++ (`src/recompiler`) and emits one function per guest function, `void f_XXXXXXXX(Ctx&)`.
  - It uses labels and `goto` inside functions, an address→function table for `bctrl`/`blrl`, and a `switch` for known jump tables.
  - `Ctx` holds the GPRs, FPRs with ps1, CR, LR, CTR, XER, FPSCR and the GQRs.
  - Guest MEM1 is addressed as `addr & 0x01FFFFFF`, with big-endian load/store helpers.
- It takes function boundaries from **the LM decomp's `symbols.txt`** (CC0, generated by dtk). It was still bitten by merged functions and had to add entry points for every address-taken location and every intra-function branch target. **This is the strongest argument for running dtk relocation analysis on our DOL.**
- SDK translation units are HLE'd onto Aurora (GX, VI, PAD), DVD, AR/ARQ and CARD. Its OS thread, mutex and context code is adapted from Dusklight (CC0).
- **Audio:** LM uses JAudio. The port disassembled the game's own DSP microcode and wrote a host stand-in that reads the voice parameter blocks from main memory, while keeping AI DMA interrupt timing realistic. Copy this approach for MusyX (section 4).
- **Hard-won lessons worth copying as ideas:**
  - Guest threads must release the CPU during long host waits.
  - Disc reads must take realistic time, because async-callback races appear otherwise.
  - Never strand interrupts.
  - Use a windowed-sinc resampler for audio output.
- **License:** The port's own code has no license, so do not copy it. Its third-party parts (Dusklight, lm-decomp: CC0; Aurora: MIT) can be taken from those upstream sources directly.

**Dusklight: directly reusable code**
- `libs/dolphin`, `libs/PowerPC_EABI_Support` (MSL, Runtime, MetroTRK) and Dusklight's PC-side implementations of OS threads, mutexes, contexts, alarms and so on are CC0. They are the cleanest starting point for our OS HLE layer, and the LM port proves the approach.
- Note that Twilight Princess uses a 2005-era SDK. Struct layouts that the game reads directly, such as `OSThread`, `DVDFileInfo` and `CARDStat`, must be checked against the 2002 SDK headers from the SMS or Prime decomps.

**XenonRecomp: useful MIT design reference**
- Xbox 360 PPC is a different variant (64-bit, VMX, no paired singles). Still useful, legally borrowable ideas:
  - XenonAnalyse's jump-table pattern detection
  - Mid-asm hooks configured in TOML
  - Switch-table emission
  - The `PPCContext` and function-mapping design
  - Its approach to invalid and self-modifying instructions

---

## 3. Dolphin SDK, MSL and runtime references, and resources for SDK signature matching

The other agent owns signature matching. These are the inputs it can use.

| Resource | URL | License | SDK era | Use |
|---|---|---|---|---|
| **Super Mario Sunshine decomp** | https://github.com/doldecomp/sms | **CC0** | ~mid-2002, closest in date | Must-use: SDK, MSL and Runtime source and headers for struct layouts; `config/*/symbols.txt` has names and sizes |
| **Metroid Prime decomp** | https://github.com/PrimeDecomp/prime | **CC0** | late 2002 | Must-use: SDK + **MusyX** + MSL + Runtime + TRK, all from the same era and toolchain |
| Pikmin decomp | https://github.com/projectPiki/pikmin | CC0 | 2001 | Useful: an older SDK variant for multi-version signatures |
| Pikmin 2 decomp | https://github.com/projectPiki/pikmin2 | CC0 | 2004 | Useful for newer variants |
| Twilight Princess decomp, and Dusklight | https://github.com/zeldaret/tp | CC0 | 2005–06 | Headers and later versions |
| dtk signature DB | `decomp-toolkit/assets/signatures` | MIT/Apache | many | Must-use: ready-made masked signatures with relocation-chain format |
| doldecomp/dolsdk2001 and dolsdk2004 | https://github.com/doldecomp/dolsdk2001, https://github.com/doldecomp/dolsdk2004 | **No license** | 5-22-2001, 4-20-2004 | Reference only. Their per-library progress tables show which functions exist in each SDK version. |
| Melee decomp | https://github.com/doldecomp/melee | **No license** | 2001 | Reference only |
| Dolphin emulator `.dsy`/`.mega` signature DBs | (Dolphin repo) | **GPL-2.0+** | — | **Avoid** (project policy) |

**Recommended signature pipeline:** build the SDK, MSL and Runtime translation units from the SMS and Prime decomps with the matching mwcc version, using their `configure.py`. Take the resulting `.o` files and produce dtk-style masked signatures in our own Python, masking relocated fields and recording the relocation target names. Then match against our DOL and follow call chains outward from anchored functions. No retail binaries are needed, and the source is CC0.

Note that our SDK build is ~April 2002 and Metrowerks CodeWarrior ~1.3.2. Expect small differences from SMS and Prime, so use relocation-aware fuzzy matching.

---

## 4. MusyX audio

| Tool | URL | License | Maturity | Priority |
|---|---|---|---|---|
| **AxioDL/musyx** (MusyX decompilation with a PC backend) | https://github.com/AxioDL/musyx | **CC0-1.0** | Very active (Oct 2026). Matches MusyX **1.5.0, 1.5.3, 2.0.x** | **Must-use** |
| PrimeDecomp/prime (contains MusyX) | see above | CC0 | Matching | Useful (same code, in game context) |
| amuse (derselbst fork; the AxioDL original) | https://github.com/derselbst/amuse | MIT | Maintained fork | Useful: a standalone player for checking data (`.proj/.pool/.sdir/.samp`) |
| musyx-extract | https://github.com/Nisto/musyx-extract | No license | Small tool | Reference: sample extraction for checking `sound.zip` contents |
| GCMusyXTools | https://github.com/soneek/GCMusyXTools | No license | Old (2015) | Skip |
| vgmstream | https://github.com/vgmstream/vgmstream | Permissive custom license (ISC-like) | Mature | Useful for auditioning streams and ADPCM from `sound.zip` |

**AxioDL/musyx details:** It provides `src/musyx/runtime/` (`seq.c`, `synth*.c`, `snd*.c`, `stream.c`, `hw_dspctrl.c`, `hw_aramdma.c`, `hw_dolphin.c`, effects) and also a **PC hardware backend** (`hw_pc_*.c`: decode, mix, rate conversion, SDL output, null device). It is version-selectable through `MUSYX_VERSION_*`.

**How it helps:** there are two viable routes.

1. **Recommended: recompile the game's MusyX code as-is and HLE only the DSP microcode.**
   - `hw_dspctrl.c` (`salBuildCommandList`) and `dsp_import.c` document exactly what command list and voice blocks MusyX hands to its DSP microcode. That is the spec for a host mixer, just as the LM port did for JAudio.
   - This keeps all game-side behaviour bit-exact and confines HLE to AI, DSP and ARAM.
2. **Alternative: function-level HLE.** Identify the MusyX `snd*`/`synth*` entry points and redirect them to a natively compiled AxioDL/musyx built for the PC target. This is less work on DSP details but needs exact ABI marshalling of the game's sound data in guest memory and ARAM, plus every callback. Keep it as a fallback.

**First step:** determine the MusyX version, which AxioDL/musyx needs to select the right layouts. Match a few MusyX functions (for example `sndInit` or `salBuildCommandList`) against the decomp built at 1.5.x and 2.0.x. Avoid Dolphin's DSP HLE and LLE code (GPL).

---

## 5. RenderWare (newly identified as a major component)

| Tool | URL | License | Priority | Use |
|---|---|---|---|---|
| **ShulkMaster/gameware** | https://github.com/ShulkMaster/gameware | **No license** | **Useful, reference only** | Reconstructs RenderWare 3.2.0.0 (as in Midway's *Mortal Kombat: Deadly Alliance* GameCube release, 2002) and 3.6.0.3 from retail ELF symbols. It shows which RenderWare objects and functions a 2002 Midway GameCube title links, and how `RwEngineGetVersion` reveals the version (`lis r3,3; addi r3,r3,0x2000` → 0x32000). |
| **MKDA GameCube `mk5gc_release.elf`** (shipped on the retail MK: Deadly Alliance disc, per retroreversing.com's "GameCube Games with Debug Symbols") | — | Retail data | **Useful**, if the user owns the game | A symbolized 2002 Midway GameCube build containing RenderWare, the Dolphin SDK and Midway libraries. Building byte-masked signatures from it could name **RenderWare** functions in our DOL. Keep it as a local, never-redistributed input. |
| **librw** | https://github.com/aap/librw | MIT | Useful | An open reimplementation of RenderWare Graphics 3.x. Use it to understand `.dff` and texture-dictionary (`.rtd`) stream formats and for asset viewers. ABI and layout differences from GameCube RW 3.2 make it a poor drop-in replacement for the game's RenderWare. |
| DragonFF (Blender add-on) | https://github.com/Parik27/DragonFF | GPL-3.0 | Useful (as a tool only) | Viewing and exporting `.dff` models for asset inspection; we do not ship it |

**Recommendation:**
- Check `RwEngineGetVersion` in our DOL to get the RenderWare version.
- Leave RenderWare **recompiled as normal game code**. Its GameCube driver ends in GX calls, which Aurora handles.
- Name the RenderWare functions (from MKDA ELF signatures if available, or by hand in Ghidra) so that hot paths can later be hooked for widescreen, resolution and interpolation enhancements.

---

## 6. Ghidra and other RE utilities
- **Ghidra + Ghidra-GameCube-Loader**: covered in section 1. Useful.
- **GCFT** (https://github.com/LagoLunatic/GCFT, MIT): a disc, RARC and texture multitool, mostly for Nintendo formats. Skip, since our archives are plain zip.
- **Aurora's bundled nod** (disc and RVZ reader): use it for runtime DVD access to the user's ISO. This is already part of Aurora.

---

## 7. Midway, NFL Blitz and related reverse engineering

| Resource | URL | License | Priority | Notes |
|---|---|---|---|---|
| **historicalsource/nflblitz2000** | https://github.com/historicalsource/nflblitz2000 | **None** (leaked Midway source) | **Useful, reference only; never copy** | Arcade *NFL Blitz 2000* C source: `playbook.c`, `teamdata.c`, `drone.c`, `cambot.c`, `plyrseq.c`, `replay.c` and others. The GameCube game's `Blitz2k3::main.c`, `.cap` camera files and drone/cambot naming suggest this codebase is an ancestor, so it helps name game functions and data structures during debugging. |
| NFLBlitzDataEditor.Core | https://github.com/DMGambone/NFLBlitzDataEditor.Core | GPL-3.0 | Skip (arcade data only) | Arcade roster and data formats |
| TBORG "NFL Blitz 20-16" GameCube roster mod | https://tecmobowl.org/files/file/459-nfl-blitz-20-16/ | — | Useful (evidence) | Shows that `teamdata.zip` and `Start.dol` can be modded and that Dolphin texture packs exist. The mod does not document any format. |
| Other Midway GameCube titles (Blitz 2002, Blitz Pro, NHL Hitz 2002/2003, RedCard, MLB Slugfest) | — | — | Unknown | We found **no public decomp or format documentation**. If the user owns any, check its disc for leftover `.map`/`.elf` files with symbols; only MKDA and Mortal Kombat: Deception are confirmed to ship ELF files with symbols. |
| Any NFL Blitz decomp effort | — | — | — | **None found.** |

**`.zip` and `.ziH` archives:** `.zip` is standard PKZIP with stored entries such as `*.rsc`. `.ziH` appears to be a precomputed big-endian directory (entry count followed by offset and size tuples) for fast lookup. Python's `zipfile` and a short `.ziH` parser are enough for extraction tooling, and the recompiled game parses both itself.

---

## 8. FMV (`*.avi`)

- **Container and codecs (verified from the headers):** AVI with video **MidiVid VQ (`MVDV`)** at 480x360, and audio format tag `0x0069` with an `iadp` extension: 4-bit IMA-style ADPCM, stereo, 48 kHz or 44.1 kHz, 1024-byte blocks.
- **FFmpeg:** `libavcodec/midivid.c` decodes MVDV (LGPL-2.1+; FFmpeg ≥ 4.3). FFmpeg's `riff.c` maps `MVDV` correctly and maps 0x0069 to `ADPCM_IMA_XBOX`. **Test with `ffprobe`/`ffplay`; ffmpeg is not installed on this machine.**
- **Recommended approach:**
  1. **Default:** the game ships its own `MoviePlayer` (MidiVid decoder plus ADPCM). Once recompiled, it decodes on the CPU and uploads through GX, so FMVs should work without extra code. Make sure the DVD streaming read rate and audio (AI) pacing behave as on hardware.
  2. **Optional enhancement or fallback:** hook `MoviePlayer` and play the file with FFmpeg linked dynamically, which satisfies the LGPL. This only makes sense if the recompiled decoder is too slow or the user wants higher quality.

---

## 9. Recommended toolchain

1. **Analysis:** keep our Python decoder and function finder as the main path.
   - **Run dtk** (`dtk dol config` / `split` through a dtk-template harness) once and **merge its results**:
     - function starts and sizes, used as a cross-check
     - extabindex-derived sizes
     - **relocations**, so that every pointer into `.text` becomes an indirect-call table entry, and jump tables
   - Keep dtk's `symbols.txt` format as our canonical symbol format.
2. **SDK naming (for the other agent):** obtain masked signatures from objects built out of the **CC0 SMS and Prime decomps**, plus dtk's `assets/signatures`. Do not use Dolphin's GPL `.dsy`/`.mega` DBs, or the unlicensed dolsdk and Melee repos, as code sources.
3. **Runtime:**
   - **Aurora** (MIT) for GX, PAD, DVD (with nod) and CARD.
   - OS threads, mutexes, alarms and the MSL/Runtime host layer adapted from **Dusklight** (CC0).
   - Struct layouts checked against the **SMS and Prime** 2002 SDK headers.
4. **Recompiler design:** follow the **LM port's** architecture (function-per-guest-function, a `Ctx` struct, address→function dispatch, HLE at SDK translation-unit boundaries), together with its threading, interrupt and DVD-timing lessons. Borrow MIT code and ideas from **XenonRecomp** (jump-table detection, mid-asm hooks) where useful. Avoid DolRecomp, ModernGekko and RingOut (GPL).
5. **Audio:** recompile the MusyX library and the Midway "msl" wrapper. HLE AI, DSP and ARAM, with a host mixer that implements the MusyX microcode command list as documented by **AxioDL/musyx** (CC0). Use amuse and vgmstream to check sound data.
6. **Graphics engine:** treat RenderWare as ordinary recompiled code on top of Aurora. Use **gameware** (as reference) and, optionally, signatures from the user's own **MKDA ELF** to name RenderWare functions for later enhancement hooks. Use **librw** and **DragonFF** for asset inspection.
7. **RE workbench:** **Ghidra** + **Ghidra-GameCube-Loader**, with our `symbols.txt` imported. Consult **historicalsource/nflblitz2000** for game-logic structure (reference only).
8. **FMV:** rely on the game's own MidiVid decoder. Keep FFmpeg (LGPL, dynamically linked; `mvdv` decoder) as an optional replacement and as a tool for checking the `.avi` files.
9. **Assets:** Python `zipfile` plus a small `.ziH` parser. Only needed for tooling and texture-pack work; the game reads the archives at runtime.

---

### Sources
- https://github.com/encounter/decomp-toolkit, https://github.com/encounter/dtk-template (`docs/getting_started.md`)
- https://github.com/rob646464/luigis-mansion-pc-port (`docs/RECOMPILER.md`, `docs/AUDIO.md`, `NOTICE.md`)
- https://github.com/TwilitRealm/dusklight, https://github.com/encounter/aurora, https://github.com/AxioDL/metaforce
- https://github.com/AxioDL/musyx, https://github.com/derselbst/amuse, https://github.com/Nisto/musyx-extract
- https://github.com/ExpansionPak/DolRecomp, https://github.com/BlackLineInteractive/NWiiRecomp, https://github.com/jackpoison-prog/RingOut, https://github.com/hedge-dev/XenonRecomp
- https://github.com/doldecomp/sms, https://github.com/PrimeDecomp/prime, https://github.com/doldecomp/dolsdk2001, https://github.com/doldecomp/dolsdk2004
- https://github.com/Cuyler36/Ghidra-GameCube-Loader, https://github.com/aap/librw, https://github.com/ShulkMaster/gameware
- https://www.retroreversing.com/gamecube-debug-symbols, https://github.com/historicalsource/nflblitz2000, https://tecmobowl.org/files/file/459-nfl-blitz-20-16/
- https://ffmpeg.org/doxygen/6.1/midivid_8c.html, FFmpeg `libavformat/riff.c`
