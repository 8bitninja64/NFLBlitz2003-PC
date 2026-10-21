#!/bin/sh
# Linux build (Ubuntu/WSL): recompile the DOL, then build the runtime and game module with clang + Ninja.
#   tools/build_linux.sh [build dir, default build/linux-clang]
# Needs: python3, cmake, ninja, clang, and the X11/Wayland/GL dev packages SDL3 builds against.
# The disc's main.dol must be extracted first (python3 -I tools/gcdisc.py <iso> orig), as on Windows.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bdir=${1:-"$root/build/linux-clang"}
cd "$root"
mkdir -p build/analysis
python3 -I tools/recomp/analyze.py orig/main.dol build/analysis/functions.json
python3 -I tools/recomp/recomp.py orig/main.dol build/analysis/functions.json config/sdk_symbols.txt,config/symbols.txt \
  config/hooks.txt build/recomp_gen
cmake -S . -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$bdir" --target blitz
echo "built $bdir/NFLBlitz2003 (+ blitz_game.so next to it)"
