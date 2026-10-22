#!/bin/sh
# Builds only the game module (blitz_game.so) from a disc image or main.dol: extract the
# executable, analyze, recompile, compile. Needs python3 and coreutils, nothing else (minimal installs
# lack xargs or xz: tools/compile_module.py unpacks and compiles); the compiler is the Zig toolchain the
# release carries (tools/zig.tar.xz, unpacked into the work folder for the build and removed after:
# SteamOS and other systems without a compiler), else clang++ or g++. The runtime (NFLBlitz2003) is
# not rebuilt. This is the step a first-run setup performs on the player's machine (the Linux
# counterpart of make_module.ps1).
#   tools/make_module.sh <disc image | main.dol> [out, default build/linux-clang/blitz_game.so] [work dir]
# BLITZ_CXX=<compiler> picks one (e.g. g++, or "zig" for the bundled toolchain).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: make_module.sh <disc image | main.dol> [out] [work dir]}
out=${2:-"$root/build/linux-clang/blitz_game.so"}
work=${3:-"$root/build/module"}
case $src in /*) ;; *) src="$PWD/$src" ;; esac
case $out in /*) ;; *) out="$PWD/$out" ;; esac
case $work in /*) ;; *) work="$PWD/$work" ;; esac
cd "$root"
command -v python3 >/dev/null || { echo "python3 is required (install your distribution's python3 package)" >&2; exit 1; }

# The compiler: bundled Zig first (the same compiler on every system), then the system's.
zig_kit="$root/tools/zig.tar.xz"
pick=${BLITZ_CXX:-}
if [ -z "$pick" ]; then
  if [ -f "$zig_kit" ] || [ -x "$root/tools/zig/zig" ]; then pick=zig
  elif command -v clang++ >/dev/null; then pick=clang++
  elif command -v g++ >/dev/null; then pick=g++
  else echo "no C++ compiler: this package's tools/zig.tar.xz is missing and neither clang++ nor g++ is installed" >&2; exit 1
  fi
fi
# The unpacked compiler and its cache (~250 MB) go when the script ends, also when it is cancelled
# (signals become a normal exit so that every shell runs the EXIT trap).
zig_tmp=""
cleanup() {
  if [ -n "$zig_tmp" ]; then rm -rf "$zig_tmp" "$work/zig-cache"; fi
  return 0
}
trap cleanup EXIT
trap 'exit 143' TERM INT HUP
if [ "$pick" = zig ]; then
  if [ -x "$root/tools/zig/zig" ]; then zig="$root/tools/zig/zig"
  else
    echo "# Unpacking the compiler"
    zig_tmp="$work/zig"
    rm -rf "$zig_tmp"; mkdir -p "$zig_tmp"
    python3 -I tools/compile_module.py unpack "$zig_kit" "$zig_tmp"
    zig="$zig_tmp/zig/zig"
  fi
  export ZIG_GLOBAL_CACHE_DIR="$work/zig-cache" ZIG_LOCAL_CACHE_DIR="$work/zig-cache"
  # By name on PATH: $CXX is split into words below, and the user directory has spaces in it.
  PATH="$(dirname "$zig"):$PATH"
  export CXX="zig c++ -target x86_64-linux-gnu"
else
  export CXX="$pick"
fi
echo "compiler: $CXX"

# Every build starts clean: the recompiler rewrites every chunk anyway, and chunks or objects left by
# another version must not be linked in.
rm -rf "$work/gen" "$work/obj"
mkdir -p "$work/gen" "$work/obj"
echo "# Reading the game code"
case $src in
  *.dol|*.DOL) dol=$src ;;
  *) python3 -I tools/gcdisc.py "$src" "$work" --dol-only; dol="$work/main.dol" ;;
esac
python3 -I tools/recomp/analyze.py "$dol" "$work/functions.json"
echo "# Translating the game code"
python3 -I tools/recomp/recomp.py "$dol" "$work/functions.json" config/sdk_symbols.txt,config/symbols.txt \
  config/hooks.txt "$work/gen"

# Compile in parallel (one job per CPU). Its "# Compiling N files" total and "compiled <file>" lines
# drive the first-start progress bar.
export CXXFLAGS="-std=c++20 -O2 -fPIC -fvisibility=hidden -DNDEBUG -w"
export RT="$root/src/runtime" GEN="$work/gen" OBJ="$work/obj"
python3 -I tools/compile_module.py compile "$work"/gen/*.cpp "$root/src/runtime/module_rt.cpp"
echo "# Linking"
mkdir -p "$(dirname "$out")"
$CXX -shared -s -Wl,-Bsymbolic -o "$out.tmp" "$work"/obj/*.o
mv -f "$out.tmp" "$out"
echo "built $out"
