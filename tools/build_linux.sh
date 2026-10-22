#!/bin/sh
# Linux build (Ubuntu/Debian/WSL; clang + Ninja): extract the executable from your disc, recompile it,
# build the runtime and the game module.
#   tools/build_linux.sh [disc image] [build dir, default build/linux-clang]
# Needs: python3, cmake, ninja, clang, and the X11/Wayland/GL/audio dev packages SDL3 builds against
# (see .github/workflows/build.yml for the Ubuntu package list).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
disc=${1:-"$root/NFL Blitz 2003 (USA).iso"}
bdir=${2:-"$root/build/linux-clang"}
cd "$root"
# Local fixes to Aurora (patches/aurora/*.patch), applied once to the submodule checkout.
for patch in patches/aurora/*.patch; do
  [ -f "$patch" ] || continue
  git -C extern/aurora apply --ignore-whitespace --reverse --check "$root/$patch" 2>/dev/null && continue
  git -C extern/aurora apply --ignore-whitespace "$root/$patch" || { echo "cannot apply $patch to extern/aurora" >&2; exit 1; }
  echo "applied $patch"
done
if [ ! -f orig/main.dol ]; then
  [ -f "$disc" ] || { echo "disc image not found: $disc" >&2; exit 1; }
  python3 -I tools/gcdisc.py "$disc" orig --dol-only
fi
mkdir -p build/analysis
python3 -I tools/recomp/analyze.py orig/main.dol build/analysis/functions.json
python3 -I tools/recomp/recomp.py orig/main.dol build/analysis/functions.json config/sdk_symbols.txt,config/symbols.txt \
  config/hooks.txt build/recomp_gen
cmake -S . -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$bdir" --target blitz
echo "built $bdir/NFLBlitz2003 (run it with: $bdir/NFLBlitz2003 --disc \"$disc\")"
