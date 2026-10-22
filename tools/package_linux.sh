#!/bin/sh
# Assembles the Linux release (dist/NFLBlitz2003 + dist/NFLBlitz2003-linux-x64.tar.xz) WITHOUT any
# game code: the runtime plus the files the first start needs to compile the game module from the
# player's own disc. The Linux counterpart of package.ps1; build the runtime first.
#   tools/package_linux.sh [build dir, default build/linux-clang] [out, default dist/NFLBlitz2003]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
bin=${1:-"$root/build/linux-clang"}
out=${2:-"$root/dist/NFLBlitz2003"}
cd "$root"
[ -f "$bin/NFLBlitz2003" ] || { echo "build NFLBlitz2003 first (tools/build_linux.sh)" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"

put() { mkdir -p "$(dirname "$out/$2")"; cp "$1" "$out/$2"; }
put "$bin/NFLBlitz2003" NFLBlitz2003
strip "$out/NFLBlitz2003"
for f in tools/make_module.sh tools/gcdisc.py \
         tools/recomp/analyze.py tools/recomp/dol.py tools/recomp/ppc.py tools/recomp/recomp.py \
         config/hooks.txt config/sdk_symbols.txt config/symbols.txt \
         src/runtime/ctx.h src/runtime/func_table.h src/runtime/module_abi.h src/runtime/module_rt.cpp \
         README.md LICENSE; do
  put "$f" "$f"
done
chmod +x "$out/NFLBlitz2003" "$out/tools/make_module.sh"

# Third-party notices for the libraries in the runtime.
put extern/aurora/LICENSE licenses/aurora.txt
deps="$bin/_deps"
for pair in imgui-src/LICENSE.txt:imgui.txt fmt-src/LICENSE:fmt.txt xxhash-src/LICENSE:xxhash.txt \
            zstd-src/LICENSE:zstd.txt abseil-cpp-src/LICENSE:abseil.txt sdl-src/LICENSE.txt:sdl3.txt \
            tracy-src/LICENSE:tracy.txt; do
  [ -f "$deps/${pair%%:*}" ] && put "$deps/${pair%%:*}" "licenses/${pair#*:}"
done

# Nothing translated from the game may be in the package.
bad=$(find "$out" \( -name 'blitz_game.so' -o -name 'recomp_*.cpp' -o -name 'main.dol' -o -name '*.iso' -o -name '*.rvz' \))
[ -z "$bad" ] || { echo "game data in the package: $bad" >&2; exit 1; }
echo "packaged $out ($(du -sh "$out" | cut -f1))"
tar -C "$(dirname "$out")" -cJf "$out-linux-x64.tar.xz" "$(basename "$out")"
echo "archived $out-linux-x64.tar.xz ($(du -h "$out-linux-x64.tar.xz" | cut -f1))"
