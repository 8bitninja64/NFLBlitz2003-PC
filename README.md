# NFL Blitz 2003 — PC port (static recompilation)

A native Windows port of NFL Blitz 2003 (GameCube, USA). The game's PowerPC executable is
translated to C++ from **your own disc image** at build time; graphics run on
[Aurora](https://github.com/encounter/aurora) (WebGPU), sound through a host implementation of the
GameCube's audio microcode, and controllers through SDL3.

No game data is included in this repository.

## Features

- Native rendering at your window/screen resolution, optional supersampling up to 6x (2880p)
- **Widescreen**: Hor+ 16:9 (or any aspect) for gameplay, 2D screens and HUD kept 4:3 centered
  (or stretched, your choice)
- Locked 60 fps (the game's own 59.94 Hz pacing), D3D11 by default on Windows
- MSAA and anisotropic filtering
- Gamepads, GameCube controller adapters and keyboard; rumble
- Memory card saves (Dolphin-compatible GCI folder)
- **F1** settings menu (pauses the game), FPS counter, Alt+Enter / F11 fullscreen

## Building

Requirements: Windows 10/11, Visual Studio 2022 Build Tools (C++ and clang), Python 3, Git, and
your own `NFL Blitz 2003 (USA).iso` (game ID GO3E5D).

```bash
git clone --recursive <this repository>
```

```bash
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Target blitz
```

The build extracts the executable from the ISO (place it in the repository root, or pass
`-Disc <path>`), analyzes it, recompiles it and compiles the port. Aurora downloads its Dawn and SDL3
dependencies on the first configure.

Run `build/windows-clang/NFLBlitz2003.exe`. On first start it looks for the disc next to the
executable or in the working directory, otherwise it asks for it; the path is remembered.

**Do not redistribute the built executable**: it contains code translated from the game. A
redistributable setup (shipping the recompiler and compiling on the user's machine from their disc,
as the Luigi's Mansion PC port does) is planned.

## Controls (keyboard, player 1)

| Controller | Keyboard |
|---|---|
| Control stick | W A S D |
| D-pad | Arrow keys |
| A / B / X / Y | K / J / L / I |
| Z | Space |
| L / R | Q / E |
| Start | Enter |
| C-stick | Numpad 8 4 2 6 |

F1: settings menu · Alt+Enter or F11: fullscreen

## Settings and saves

`%APPDATA%\NFL Blitz 2003\settings.ini` (also editable in the F1 menu) and the memory card in
`%APPDATA%\NFL Blitz 2003\USA\Card A`.

## Status

Boots, menus and Quickplay games are playable with sound; the memory card is detected and read (writing a save is not yet verified).
Known issues and development notes: see `CLAUDE.md`.
