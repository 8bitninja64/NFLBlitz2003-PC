# Audio research: DSP / AX / MusyX on the host (2026-10-21)

Scope: how other GameCube/Wii PC ports handle audio, which permissively licensed code covers the
DSP side of MusyX (parameter blocks, SRC, ADPCM, ramps, depop, LPF), and what Blitz2003's
`src/runtime/dsp_ax.cpp` can reuse. Licenses were checked through the GitHub API on this date.
Everything downloaded was read as text only.

## 0. Key finding: MusyX on GameCube does not run on AX

MusyX uploads its own DSP microcode, `dspSlave`, and builds its own command lists
(`salBuildCommandList`). The AxioDL/musyx decomp (CC0) has both:

- `src/musyx/runtime/dsp_import.c`: the microcode image, per MusyX version.
- `src/musyx/runtime/hw_dspctrl.c`: the command list builder. It also has the depop math and the
  volume-ramp math.
- `include/musyx/dspvoice.h`: `_PB` (voice parameter block), `_SPB` (studio depop block), `_PBMIX`,
  `_PBUPDATE`, `_PBDPOP`, `_PBITD`, `_PBSRC`, `_PBLPF`.

The commands are: 0 SETUP, 1 DL_AND_VOL_MIX, 2 PB_ADDR, 3 PROCESS, 4 MIX_AUXA, 5 MIX_AUXB,
6 UPLOAD_LRS, 9 MIX_AUXB_NOWRITE (adds a master studio to the final mix), 13 MORE, 14 OUTPUT,
15 END, 16 MIX_AUXB_LR, 17 SET_OPPOSITE_LR (its argument is the surround buffer `dspSurround`),
and 18 compressor (2.0.2+).

The PB looks like AX but is not AX's layout. `dsp_ax.cpp` already uses the MusyX `_PB`, so it is
on the right track. Two consequences:

- Rename it mentally: this is a "MusyX DSP HLE", not an AX HLE.
- AX documentation and code (Dolphin's AX HLE, Cemu's AX) are only loose references. The exact
  per-sample behaviour is in `dspSlave`, which is inside our own DOL. It can be disassembled, the
  way the Luigi's Mansion port did for JAudio's microcode.

Our `_PB` size of 0xBC means MusyX 1.5.3 to 2.0.0. In the decomp, `streamLoopCnt` is at 0xBA and
there is no `_PBLPF`. The per-voice low-pass filter only arrived in 2.0.1 ("2.0 Patch 1"). If the
game's version is below 2.0.1, there is no LPF to emulate. Confirm the version with
`tools/sdkid` against the decomp's version gates (`MUSY_VERSION_CHECK`).

## 1. What other ports do

| Project | License | Audio approach |
|---|---|---|
| **AxioDL/musyx** (MusyX decomp), https://github.com/AxioDL/musyx | **CC0-1.0** | Matching decomp of MusyX 1.5.x–2.0.2, **plus a native PC backend** (`src/musyx/runtime/hw_pc*.c`, added May–Sep 2026). The backend replaces `salCtrlDsp` with a host mixer (details in §2.1) and outputs through SDL3. It works above the command-list level, on MusyX's `DSPvoice` structs. |
| **Metaforce** (Metroid Prime), https://github.com/AxioDL/metaforce | MIT (`LICENSE.decomp` for the decomp parts) | Links `extern/musyx` (the repo above) and uses its PC backend. No DSP emulation at all. Aurora for GX. |
| **Aurora**, https://github.com/encounter/aurora | MIT | **No audio, AX or DSP support.** It only has `include/dolphin/dsp.h` and `lib/dolphin/thp/THPAudio.cpp` (THP movie audio decode). Each port supplies its own audio. |
| **Dusklight** (Twilight Princess), https://github.com/TwilitRealm/dusklight | **CC0-1.0** | TP uses JAudio2, not AX or MusyX. `src/dusk/audio/DuskDsp.cpp` replaces the JAudio DSP at the channel level: float math, **linear** resampler with state carried across subframes ("prevents popping on pitch change"), biquad IIR, an initial-volume fix ("Initialize previous volume to new volume on first render"), and its own reverb. `DuskAudioSystem.cpp` uses an SDL3 `SDL_AudioStream` in pull mode (callback, F32), and `Adpcm.cpp` decodes ADPCM. Usable as code (CC0), but its algorithms are approximations, not microcode-exact. |
| **Luigi's Mansion Recompiled**, https://github.com/rob646464/luigis-mansion-recompiled (the old pc-port URL redirects) | **No license: design only** | `src/recomp/audio_dsp.cpp`, `audio_out.cpp`, `hle_audio.cpp`, `tools/dsp_disasm.py`. Design notes are below this table. |
| **Super Mario Sunshine decomp**, https://github.com/doldecomp/sms | CC0-1.0 | Decomp only (JAudio). No host audio. |
| **zeldaret/tp**, https://github.com/zeldaret/tp | CC0-1.0 | Decomp only. |
| sp00nznet/gcrecomp (MIT) / ww (MIT) / ikaruga | MIT / NOASSERTION | Toy mixers (`src/audio/audio_mixer.cpp`): pan plus volume, no SRC filter, no command lists. Not useful. |
| BlackLineInteractive/NWiiRecomp | "Modified MIT with commercial restrictions" (not OSI) | `nWiiRuntime/src/hle/ax.cpp` is logging stubs only. Not useful, and the license is unsuitable. |
| Cemu (Wii U AX HLE), `src/Cafe/OS/libs/snd_core/ax_mix.cpp` | MPL-2.0 (file-level copyleft) | Wii U AX: ADPCM, **linear** SRC only, plus `AXMix_DepopVoice`. Reference only. |

Design notes from the Luigi's Mansion port (studied, nothing copied):

- It disassembled the game's own DSP microcode (`jdsp` image in the DOL) with its own DSP
  disassembler.
- It reimplements the microcode on the host with the same fixed-point arithmetic, 40-bit or 16-bit
  accumulator saturation rules and per-subframe order. Its 4-tap polyphase resampler takes the
  interpolation table from a game-supplied pointer, uses the top 6 bits of the fraction for the
  phase, and picks the nearest sample from a ratio of 4.0 up.
- Volume ramps follow the microcode, and stop requests become exponential fades.
- Output runs at the console's real AI rate, 54 MHz/1124·2/3 = **32028.5 Hz**, not 32000.
- It resamples to the device rate itself with a 64-tap Kaiser-windowed sinc (512 phases, cutoff
  0.92 of the lower Nyquist), then queues on SDL3.
- A pacing thread drives the AI interrupt.
- It keeps the surround, centre and rear buses for discrete 5.1/7.1 output.

**Lesson for us:** the faithful route is to disassemble MusyX's `dspSlave` from our own DOL. That
is legal: it is the user's disc, and the CC0 decomp has the same bytes in `dsp_import.c` for
comparison. The pureikyubu disassembler (CC0, below) can do it.

## 2. Permissive implementations

### 2.1 AxioDL/musyx PC backend (CC0): the best direct-reuse candidate

Files under `src/musyx/runtime/`:

- **`hw_pc.c`** (~800 lines), the core mixer:
  - **SRC:** a generated 32-tap Blackman-windowed sinc polyphase table (`initResampleTables`) with
    64 cutoff bands × 128 phases, Q23, and each phase normalised to unity DC gain. The cutoff
    drops as pitch rises above 1.0, which is anti-aliasing. `srcCoefSelect` 0/1/2 maps to a cutoff
    of 0.5/0.8/1.0 (their interpretation, not proven hardware behaviour). `SRC_LINEAR` and
    `SRC_NONE` modes are kept. The comment says "independently authored".
  - **Ramps:** volume interpolates linearly from `lastVol` to `vol` across the whole 5 ms cycle,
    sample by sample (`mixRampChannel`). The ADSR envelope interpolates per 1 ms subframe from
    `adsrHandle` start to end.
  - **Depop:** each voice's last contribution per bus (11 buses) is kept. When a voice stops, a
    linear fade from that value to 0 over one cycle goes into a 1024-sample ring per studio and
    bus (`retireContribution`). The note says reusing a voice "cannot truncate the preceding
    generation's ramp".
  - **ITD:** a 128-sample history with a fractional delay that ramps to `itdShift × rate / 32000`
    (`applyITD`).
  - **LPF (≥ 2.0.1):** `y = (coefA*x + coefB*y1) >> 15`, one pole.
  - Per-subframe handling of the `changed[]` flags: 0x08 pitch, 0x10 ADSR setup, 0x20 break,
    0x40 release.
- **`hw_pc_decode.c`:** DSP-ADPCM nibble decoder with an explicit arithmetic floor, and loop and
  stream-loop predictor handling. For streams, history is kept and only the PS is reloaded.
- **`hw_pc_rate.c`:** causal 32-tap rate adapter for 32 kHz aux blocks at other mix rates.
- **`hw_pc_output.c`, `hw_pc_sdl.c`:** SDL3 output. A render thread keeps at most about 40 ms
  queued. It uses `SDL_SetCurrentThreadPriority(HIGH)` and pause/resume barriers.

Caveat: this backend is new, about a month old. It is tuned for sounding good, not bit-exact.

### 2.2 DSP-ADPCM

| Source | License | Path |
|---|---|---|
| AxioDL/musyx | CC0 | `src/musyx/runtime/hw_pc_decode.c` |
| derselbst/amuse (MusyX-compatible sequencer; the AxioDL/amuse repo is gone) | MIT | `lib/DSPCodec.cpp`, `include/amuse/DSPCodec.hpp` |
| jackoalan/gc-dspadpcm-encode | MIT | encoder plus reference decode |
| Thealexbarney/DspTool | MIT | encode and decode library |
| vgmstream | ISC-style ("NOASSERTION" on GitHub) | `ngc_dsp_decoder.c` |

The algorithm is the standard one: `((nib << scale) << 11) + c1*yn1 + c2*yn2 + 1024) >> 11`, then
clamp. `dsp_ax.cpp` already does this correctly.

### 2.3 The polyphase SRC coefficient table

The real 4-tap table is not in any ucode. It lives in the **DSP DROM**, Nintendo data mapped at DSP
data address 0x1000.

- **pureikyubu** (https://github.com/ogamespec/pureikyubu, **CC0**), `wiki/dsp_drom.md`, describes
  its structure without the data. There are 3 cells of 512 words each (byte offsets 0x000, 0x400
  and 0x800), one per `coefSelect`. Each cell holds 4 interleaved lanes, which are the 4 taps, and
  is a word-level palindrome, so 128 phases × 4 taps with phases symmetric. Peak magnitudes get
  larger from cell 0 to cell 2, i.e. cell 2 has the widest bandwidth.
- **Do not copy:** the original DROM is Nintendo's copyrighted data. Dolphin's free replacement
  `dsp_coef.bin` (windowed sinc, 2015) is part of Dolphin and therefore GPL.
- **Recommendation:** generate our own 3 × 128 × 4 table at startup in 1.15 fixed point, as a
  clean-room design. Use, for example, a Kaiser- or Blackman-windowed sinc, with the cutoff
  rising from coefSelect 0 to 2 and each phase normalised to sum to 0x7FFF. Alternatively, use
  musyx's 32-tap generator (CC0), which sounds better than the hardware anyway.
- Get the phase index bits from the `dspSlave` disassembly. LM's JAudio ucode uses the top 6
  bits; AX docs suggest 7 bits (128 phases).

### 2.4 Other references

- **pureikyubu (CC0):** a full DSP LLE emulator (`src/dspcore.cpp`, `dspdisasm.cpp`, JIT).
  - Its **disassembler can be used to read `dspSlave`**.
  - In theory we could run `dspSlave` LLE on its core instead of HLE, which would be exact. That
    needs the IROM and DROM, which are Nintendo data the user would have to dump. Free
    replacements are GPL. Not recommended as the shipping path.
- **libogc** (https://github.com/devkitPro/libogc): **zlib license**, with RTEMS-derived threading
  under BSD-style terms. It has `libogc/dsp.c` (mailbox/task protocol) and the homebrew ucodes
  `libasnd` and `libaesnd`. **There is no AX or MusyX.** Only useful for DSP mailbox/task
  semantics.
- **YAGCD:** no maintained repository with a stated license was found. Its DSP/AX sections are
  old and incomplete, so use it only for hardware registers (AI/DSP/ARAM).
- **amuse** (MIT): reimplements the MusyX *sequencer and macros* on the host (`lib/Voice.cpp`,
  `lib/Envelope.cpp`, `lib/Studio.cpp`). That is above our layer, because Blitz's MusyX is
  recompiled, so it is not needed.

## 3. Exact algorithms documented by the CC0 decomp (`hw_dspctrl.c`, `synth_adsr.c`)

These define what the DSP must do with what the CPU writes:

- **Timing:** one command list per 5 ms frame, 160 samples at 32 kHz, split into 5 subframes of
  32 samples.
  - `_PBUPDATE.updNum[5]` counts (word offset, value) pairs per subframe at `update.data`. Word
    offsets are indexes into `_PB`: 7 is `state`, 0x32/0x33 are `ve.currentVolume`/`currentDelta`,
    0x53/0x54 are the `src.ratio` hi/lo halves, and `pbOffsets[] = {10,12,24,14,16,26,18,20,22}`
    are the mix *delta* words.
  - **Updates apply at the start of their subframe.**
- **Voice start mid-frame:** for `singleOffset != 0`, the CPU writes `pb->state = STOPPED` and puts
  `{7, 1}` (state = PLAYING) in `updNum[mix_start]`. **The DSP must apply updates before checking
  state,** and output silence for the earlier subframes. When the ADSR ends, the CPU queues
  `{7, 0}` at that subframe.
- **Envelope (VE):**
  - `adsrHandle` returns `start = currentVolume >> 16` and `delta = currentDelta >> 21`, which is
    a per-sample step over the 32 samples of a subframe.
  - The DSP does `s = s*ve >> 15` and then `ve += delta` every sample.
  - The CPU re-sends `ve.currentVolume` (and the delta if it changed) every subframe, so the DSP's
    running VE is re-synced each millisecond.
- **Mix volumes:**
  - Pre-1.5.3, and when the volume changed this frame: `delta = (target - last) / 160` per sample,
    applied over the whole frame.
  - 1.5.3+ when the volume did *not* change (rounding residue): for a residue d with 32 ≤ |d| < 160,
    `delta = ±1` per sample, and an update at subframe `d >> 5` writes 0 to that delta word.
  - So the DSP adds the delta every sample and the CPU uses updates to stop it.
  - `mixerCtrl` gates which buses are mixed:
    - < 2.0.1: bit0 = aux A, bit1 = aux B, bit2 = surround, bit3 = ramping on.
    - ≥ 2.0.1: per-bus bits 0x1 L, 0x2 R, 0x4 S, 0x10/0x20/0x80 A L/R/S, 0x200/0x400/0x1000 B L/R/S,
      with 0x8 and 0x40/0x100/0x800/0x2000 for ramps.
- **Depop:**
  - The DSP writes the voice's last output sample per bus into `pb->dpop` (9 words; order L, AL,
    BL, R, AR, BR, S, AS, BS).
  - When the CPU stops a voice (`HandleDepopVoice`), it adds those values to `hostDPopSum` per bus,
    clamped to ±0x7FFFFF.
  - Each frame, `DoDepopFade` writes `_SPB` with start = sum and delta = −sum/160, clamped to ±20
    per sample (no fade below |160|).
  - The DSP seeds the bus with `start + delta*n`. `dsp_ax.cpp` does this correctly.
- **Aux:**
  - `auxA`/`auxB` are **triple-buffered**: MIX_AUXA writes `aux[salAuxFrame]` and reads the return
    from `aux[(salAuxFrame+1)%3]`.
  - Studio mains are double-buffered: DL_AND_VOL_MIX reads `main[salFrame^1]`.
  - MIX_AUXB_LR (DPL2 studios) uses `auxB[salFrame]`/`[salFrame^1]`.
- **LPF (≥ 2.0.1 only):**
  - `hwLowPassFrqToCoef`: c = 2 − cos(2π f/32000), b1 = √(c²−1) − c, a0 = 32768(1+b1),
    b0 = −32768·b1.
  - Filter: y = (a0·x + b0·y1) >> 15.
- **SRC selection:**
  - `srcSelect`: 0 = polyphase, 1 = linear, 2 = none. When it is 2, the host assumes 32 samples per
    ms, from `sal_update_hostplayinfo`.
  - `coefSelect` 0..2 comes from `hwSetPolyPhaseFilter`. Its cycle-cost table `dspSRCCycles`
    confirms there are 3 coefficient sets.
- **ITD:** a 0x40-byte (32-sample) per-voice delay buffer. `shiftL`/`shiftR` move toward
  `targetShiftL`/`targetShiftR`, and `itd.flag` enables it.

## 4. Likely causes of our symptoms in `src/runtime/dsp_ax.cpp`

These come from comparing it with §3. They have not been tested.

1. **Voices that start mid-frame are dropped.** `renderVoice` returns when `PB_STATE != 1` *before*
   `applyUpdates(v, 0..4)`. A voice whose start comes through `updNum[mix_start]` never starts:
   the CPU never writes state = 1 itself. Fix: run the subframe loop for every PB in the list,
   apply updates first, and output zeros while state is 0.
2. **"Tinny/robotic" points to SRC.**
   - Catmull-Rom has no low-pass, so pitches above 1.0 alias, and MusyX SFX often run above 1.0.
   - Replace it with a polyphase filter whose cutoff tracks pitch: §2.1 code (CC0) or our own
     generated 3×128×4 table (§2.3).
   - Also honour `coefSelect`.
3. **Clicks at exact 5 ms boundaries point to block handoff, not the voice math.** The per-voice
   state (frac, history, ADPCM yn, VE, volumes) is carried across frames correctly. A
   discontinuity at every 160 samples plus a "robotic" (200 Hz) buzz is the classic sign of a
   whole 5 ms block being dropped, repeated or read before it is written. Check this:
   - Record the `OUTPUT` buffer and the AI DMA stream separately (`BLITZ_WAV` taps both).
   - Look for repeated or zero blocks, or a half-written buffer.
   - Check that the AI DMA plays the buffer from the *previous* list, not the one in progress.
   - Check that the host queue never underruns. musyx-pc keeps a 20–40 ms SDL queue on a
     high-priority thread; LM paces at 32028.5 Hz.
4. **Surround fold.** `output()` folds S into L/R out of phase. In stereo mode MusyX should leave S
   at about 0. If not, out-of-phase content sounds hollow or thin. The real `OUTPUT` takes
   `dspSurround` as a *separate* buffer address. Verify what lands in S before folding, and
   consider making the fold depend on the game's sound mode.
5. **Smaller items:**
   - `mixerCtrl` bus and ramp gating is ignored.
   - The ratio update at subframe 0 is read before `applyUpdates(v, 0)`.
   - ITD is not modelled.
   - Volumes are treated as unsigned up to 0xFFFF, while MusyX keeps them at ≤ 0x7FFF.
   - Truncating `>> 15` rounding adds a small negative bias. This is harmless.

## 5. Recommendation

| Item | Action |
|---|---|
| AxioDL/musyx `hw_pc.c`: polyphase generator, `resampleVoice`, `mixRampChannel`, depop ring, `applyITD` (CC0) | **Reuse directly** (adapt into `dsp_ax.cpp`, keep the command-list front end). Credit is optional under CC0, but add a note. |
| AxioDL/musyx `hw_dspctrl.c`, `dspvoice.h`, `synth_adsr.c`, `hardware.c` (CC0) | **Normative reference** for PB fields, update semantics, depop, ramps and LPF. Struct definitions can be copied. |
| AxioDL/musyx `hw_pc_sdl.c` (CC0) | Reuse its SDL3 render-thread and queue-cap pattern (works on Linux). |
| Dusklight `src/dusk/audio/*` (CC0) | Optional reuse: SDL3 pull-callback output, biquad. Its linear SRC is not better than ours. |
| pureikyubu (CC0) | Use its DSP disassembler on `dspSlave` (from our DOL) to confirm SRC phase bits, ramp order and saturation. Its DROM wiki gives the table structure. |
| Own generated 4-tap 3×128 table | Write it ourselves if we want a hardware-like sound. Never ship Nintendo DROM data or Dolphin's `dsp_coef.bin`. |
| libogc (zlib) | Mailbox/task protocol reference only. There is no AX/MusyX in it. |
| derselbst/amuse (MIT), DspTool (MIT), gc-dspadpcm-encode (MIT) | ADPCM cross-checks only. Ours is already correct. |
| Cemu (MPL-2.0) | Reference only (file-level copyleft; linear SRC only). |
| Luigi's Mansion Recompiled (no license) | Design only: disassemble the game's ucode; output at 32028.5 Hz with our own windowed-sinc to the device rate; separate pacing thread. |
| Dolphin, DolRecomp, ModernGekko, Dolphin free DSP ROM | Do not use (GPL). |

Suggested order: fix item 4.1 (start via update). Then instrument and fix the block handoff (4.3).
Then swap in the musyx-pc polyphase SRC (4.2). Then check the S fold (4.4). Last, disassemble
`dspSlave` if bit-exactness matters.

## Sources

- https://github.com/AxioDL/musyx (CC0-1.0): `include/musyx/dspvoice.h`; `src/musyx/runtime/hw_dspctrl.c`, `hw_pc.c`, `hw_pc_decode.c`, `hw_pc_rate.c`, `hw_pc_sdl.c`, `hardware.c`, `synth_adsr.c`, `dsp_import.c`
- https://github.com/AxioDL/metaforce (MIT): `.gitmodules` → `extern/musyx`
- https://github.com/encounter/aurora (MIT)
- https://github.com/TwilitRealm/dusklight (CC0-1.0): `src/dusk/audio/`
- https://github.com/rob646464/luigis-mansion-recompiled (no license): `src/recomp/audio*.cpp`, `docs/AUDIO.md` (design only)
- https://github.com/ogamespec/pureikyubu (CC0-1.0): `wiki/dsp_drom.md`, `testing/DspIrom.md`, `src/dspdisasm.cpp`
- https://github.com/devkitPro/libogc (zlib)
- https://github.com/derselbst/amuse (MIT), https://github.com/Thealexbarney/DspTool (MIT), https://github.com/jackoalan/gc-dspadpcm-encode (MIT)
- https://github.com/cemu-project/Cemu (MPL-2.0): `src/Cafe/OS/libs/snd_core/ax_mix.cpp`
- https://github.com/sp00nznet/gcrecomp (MIT), https://github.com/BlackLineInteractive/NWiiRecomp (custom restricted)
- Dolphin forum thread on the polyphase coefficient file (context only): https://forums.dolphin-emu.org/Thread-hle-dsp-srctype-polyphase-and-srctype-linear-functions
