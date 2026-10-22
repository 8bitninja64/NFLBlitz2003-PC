#!/bin/sh
# Builds only the game module (blitz_game.so) from a disc image or main.dol: extract the
# executable, analyze, recompile, compile. Needs python3 and clang++; the runtime (NFLBlitz2003) is
# not rebuilt. This is the step a first-run setup performs on the player's machine (the Linux
# counterpart of make_module.ps1).
#   tools/make_module.sh <disc image | main.dol> [out, default build/linux-clang/blitz_game.so] [work dir]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: make_module.sh <disc image | main.dol> [out] [work dir]}
out=${2:-"$root/build/linux-clang/blitz_game.so"}
work=${3:-"$root/build/module"}
case $src in /*) ;; *) src="$PWD/$src" ;; esac
case $out in /*) ;; *) out="$PWD/$out" ;; esac
case $work in /*) ;; *) work="$PWD/$work" ;; esac
cd "$root"
for tool in python3 clang++; do
  command -v $tool >/dev/null || { echo "$tool is required (install the python3 and clang packages)" >&2; exit 1; }
done

mkdir -p "$work/gen" "$work/obj"
case $src in
  *.dol|*.DOL) dol=$src ;;
  *) python3 -I tools/gcdisc.py "$src" "$work" --dol-only; dol="$work/main.dol" ;;
esac
python3 -I tools/recomp/analyze.py "$dol" "$work/functions.json"
python3 -I tools/recomp/recomp.py "$dol" "$work/functions.json" config/sdk_symbols.txt,config/symbols.txt \
  config/hooks.txt "$work/gen"

# Compile in parallel; unchanged chunks are not recompiled.
export CXXFLAGS="-std=c++20 -O2 -fPIC -fvisibility=hidden -DNDEBUG -Wno-unused-label -Wno-unused-variable \
  -Wno-unreachable-code -Wno-parentheses-equality -Wno-tautological-compare"
export RT="$root/src/runtime" GEN="$work/gen" OBJ="$work/obj"
jobs=$(nproc 2>/dev/null || echo 4)
echo "compiling the game code ($jobs jobs)..."
ls "$work"/gen/*.cpp "$root/src/runtime/module_rt.cpp" | xargs -P "$jobs" -I{} sh -c '
  o="$OBJ/$(basename "$1" .cpp).o"
  if [ ! -f "$o" ] || [ "$1" -nt "$o" ]; then clang++ $CXXFLAGS -I"$RT" -I"$GEN" -c "$1" -o "$o" || exit 255; fi' sh {}
mkdir -p "$(dirname "$out")"
clang++ -shared -Wl,-Bsymbolic -o "$out" "$work"/obj/*.o
echo "built $out"
