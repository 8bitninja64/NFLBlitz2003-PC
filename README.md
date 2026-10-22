# NFL Blitz 2003 — PC port

A native Windows and Linux port of **NFL Blitz 2003** (GameCube, USA, GO3E5D) by static
recompilation: the game's PowerPC executable is translated to C++ from **your own disc image** and
compiled for your PC. Graphics run on [Aurora](https://github.com/encounter/aurora) (WebGPU/Dawn),
audio through a host implementation of the GameCube's audio DSP, input through SDL3.

> **This repository contains no game data and no game code.** You need your own legally obtained
> copy of NFL Blitz 2003 for the Nintendo GameCube (USA). The game module built from your disc
> (`blitz_game.dll` / `blitz_game.so`) contains code translated from the game: **do not share it.**
> This project is not affiliated with or endorsed by Midway, Electronic Arts, the NFL or Nintendo.

## Features

- **Widescreen**: native Hor+ 16:9 (or any aspect) gameplay; menus stretched to fill the screen
  (optional); players, crowd and stadium are no longer culled at the edges of the wider view
- Renders at your screen resolution, with optional supersampling up to 6x (2880p), MSAA and
  anisotropic filtering
- Locked 60 fps, D3D11 / D3D12 / Vulkan on Windows, Vulkan / OpenGL on Linux
- **Settings menu** (Esc; pauses the game), fully usable with a controller: settings, key rebinding, disc
  choice, updates from GitHub and
  **cheats** — the game's own codes (infinite turbo, big / huge / team big heads, fast passes,
  fast running, power-ups, super field goals, no punting, chrome ball and more) without entering
  them on the matchup screen
- Gamepads, GameCube controller adapters and keyboard; rumble
- Memory card saves (Dolphin-compatible GCI folder)
- Reaches the title screen in about 6 seconds (skips the logo movies and the intro; optional)
- Dolphin-format HD texture packs and texture dumping
- Reads ISO and compressed dumps (RVZ, WIA, CISO, GCZ)

## Playing on Windows

1. Download `NFLBlitz2003-windows-x64.zip` from the
   [Releases](../../releases) page and unzip it anywhere.
2. Install [Python 3](https://www.python.org/downloads/) and
   [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) with
   *Desktop development with C++* and *C++ Clang tools for Windows*. They are needed once: on its
   first start the game compiles itself from your disc (about a minute).
3. Run `NFLBlitz2003.exe` and pick your `NFL Blitz 2003 (USA)` disc image when asked.

## Playing on Linux

1. Download `NFLBlitz2003-linux-x64.tar.xz` from the [Releases](../../releases) page and unpack it
   (`tar xf NFLBlitz2003-linux-x64.tar.xz`). It runs on x86-64 distributions from about 2022 on
   (glibc 2.35: SteamOS, Ubuntu 22.04, Linux Mint 21, Pop!_OS 22.04, Debian 12, Fedora, Arch, openSUSE
   Tumbleweed and newer) and needs a Vulkan driver (Mesa or
   NVIDIA's, standard on current distributions and the Steam Deck). The OpenGL ES fallback does not
   draw the game correctly yet.
2. Run `NFLBlitz2003/NFLBlitz2003` and pick your disc image when asked (needs `zenity` or `kdialog`,
   which desktops including the Steam Deck have; otherwise start it with
   `--disc "path/to/NFL Blitz 2003 (USA).iso"`). The first start compiles the game code from your disc
   with the compiler the package carries (Zig): a window asks first and shows the progress (a minute
   or two on a desktop, about five on a Steam Deck; controller, keyboard or mouse). It needs
   `python3`, which SteamOS and desktop distributions include. The output is kept in
   `~/.local/share/NFL Blitz 2003/module_build/build.log`.

**Steam Deck:** unpack it in Desktop Mode (e.g. into `~/Games`), start `NFLBlitz2003` once there to
pick the disc and compile, then add it to Steam (*Add a Non-Steam Game*) to play in Game Mode. To
start with a particular disc image, set the shortcut's Launch Options to
`--disc "/path/to/NFL Blitz 2003 (USA).iso"`.

## Building from source

### Windows

Requirements: Windows 10/11, Visual Studio 2022 (or its Build Tools) with C++ and Clang, Python 3,
Git, and your disc image.

```bash
git clone --recursive https://github.com/8bitninja64/NFLBlitz2003-PC.git
```

```bash
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Target blitz -Disc "path/to/NFL Blitz 2003 (USA).iso"
```

Then run `build/windows-clang/NFLBlitz2003.exe`. `tools/package.ps1 -Zip` makes the release zip
(runtime only, ~17 MB).

### Linux

Requirements: clang, cmake, ninja, Python 3 and the SDL3 build dependencies (X11, Wayland, GL, ALSA /
PulseAudio, udev; the Ubuntu package list is in `.github/workflows/build.yml`).

```bash
tools/build_linux.sh "path/to/NFL Blitz 2003 (USA).iso"
```

Run `build/linux-clang/NFLBlitz2003 --disc "path/to/NFL Blitz 2003 (USA).iso"`.
`tools/package_linux.sh` makes the release archive (runtime only, ~6 MB).

## Controls

Gamepads and GameCube adapters work out of the box. Keyboard (player 1):

| Controller | Keyboard |
|---|---|
| Control stick | W A S D |
| D-pad | Arrow keys |
| A / B / X / Y | K / J / L / I |
| Z | Space |
| L / R | Q / E |
| Start | Enter |
| C-stick | Numpad 8 4 2 6 |

**Menu:** Esc, Z+Start on a GameCube pad, or Back/Select on a PC gamepad opens and closes it. In the
menu: D-pad or stick to move, A to choose a section (the highlight moves into its options) or an
option, B (Backspace) to go back to the section list, L / R to switch sections. Menu > Disc picks
another disc image. Alt+Enter or F11: fullscreen.

## Settings and saves

Windows: `%APPDATA%\NFL Blitz 2003\` — Linux: `~/.local/share/NFL Blitz 2003/`
(`settings.ini`, the memory card in `USA/Card A`, texture packs in `Textures/GO3E5D`).

## Credits

- [Aurora](https://github.com/encounter/aurora) (MIT): GX on WebGPU, windowing, input, memory cards
- [Dawn](https://dawn.googlesource.com/dawn), [SDL3](https://libsdl.org),
  [Dear ImGui](https://github.com/ocornut/imgui), [nod](https://github.com/encounter/nod) and the
  other libraries listed in `licenses/` of the release
- [AxioDL/musyx](https://github.com/AxioDL/musyx) (CC0), used as the reference for the MusyX audio
  microcode, and the CC0 GameCube decompilation projects whose SDK knowledge helped identify functions
- Developer notes: `CLAUDE.md`, `docs/`

## License

See [LICENSE](LICENSE). This covers the port's own source code only; NFL Blitz 2003 and everything
derived from it remain the property of their owners.
