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
rm -rf "$out" "$out-linux-x64.tar.xz"   # a failed run must not leave an older archive behind
mkdir -p "$out"

put() { mkdir -p "$(dirname "$out/$2")"; cp "$1" "$out/$2"; }
put "$bin/NFLBlitz2003" NFLBlitz2003
strip "$out/NFLBlitz2003"
# BLITZ_MAX_GLIBC=2.35 (the release): refuse a runtime that needs a newer glibc than the oldest
# distributions it is meant for (a build on a newer system would quietly raise the floor).
if [ -n "$BLITZ_MAX_GLIBC" ]; then
  need=$(objdump -T "$out/NFLBlitz2003" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
  newest=$(printf '%s\n%s\n' "$need" "$BLITZ_MAX_GLIBC" | sort -V | tail -1)
  [ "$newest" = "$BLITZ_MAX_GLIBC" ] || { echo "NFLBlitz2003 needs glibc $need, more than $BLITZ_MAX_GLIBC" >&2; exit 1; }
  echo "glibc needed: $need (ceiling $BLITZ_MAX_GLIBC)"
fi
for f in tools/make_module.sh tools/compile_module.py tools/gcdisc.py \
         tools/recomp/analyze.py tools/recomp/dol.py tools/recomp/ppc.py tools/recomp/recomp.py \
         config/hooks.txt config/sdk_symbols.txt config/symbols.txt \
         src/runtime/ctx.h src/runtime/func_table.h src/runtime/module_abi.h src/runtime/module_rt.cpp \
         README.md LICENSE; do
  put "$f" "$f"
done
chmod +x "$out/NFLBlitz2003" "$out/tools/make_module.sh"

# The compiler for the first-start build: Zig 0.17.0 (MIT; ZIG_DIR, the unpacked official release
# zig-x86_64-linux-0.17.0), so players need no compiler (SteamOS has none). Only what an
# x86_64-linux-gnu build uses is kept (the other targets' libc trees, docs and TSan are dropped);
# every license text it ships stays. Packed as tools/zig.tar.xz, unpacked by make_module.sh.
zig_dir=${ZIG_DIR:-"$HOME/zig/zig-x86_64-linux-0.17.0"}
[ -x "$zig_dir/zig" ] || { echo "Zig not found at $zig_dir (set ZIG_DIR)" >&2; exit 1; }
ztmp=$(mktemp -d)
mkdir -p "$ztmp/zig/lib/libc/include"
(cd "$zig_dir" && tar cf - --exclude=./doc --exclude=./lib/libtsan --exclude=./lib/libc/include \
   --exclude=./lib/libc/musl --exclude=./lib/libc/mingw --exclude=./lib/libc/wasi --exclude=./lib/libc/darwin \
   --exclude=./lib/libc/freebsd --exclude=./lib/libc/netbsd --exclude=./lib/libc/openbsd .) | tar xf - -C "$ztmp/zig"
for d in generic-glibc any-linux-any x86-linux-any x86-linux-gnu; do
  cp -r "$zig_dir/lib/libc/include/$d" "$ztmp/zig/lib/libc/include/"
done
(cd "$zig_dir" && find . -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' \)) | while read -r f; do
  [ -e "$ztmp/zig/$f" ] || { mkdir -p "$ztmp/zig/$(dirname "$f")"; cp "$zig_dir/$f" "$ztmp/zig/$f"; }
done
tar -C "$ztmp" -cf - zig | xz -9 -T0 > "$out/tools/zig.tar.xz"
rm -rf "$ztmp"
put "$zig_dir/LICENSE" licenses/zig.txt
put "$zig_dir/lib/libc/glibc/LICENSES" licenses/glibc-LICENSES.txt
put third_party_licenses/LGPL-2.1.txt licenses/LGPL-2.1.txt
put third_party_licenses/GPL-2.0.txt licenses/GPL-2.0.txt
put third_party_licenses/Linux-syscall-note.txt licenses/Linux-syscall-note.txt
cat >> "$out/licenses/zig.txt" <<'EOF'

tools/zig.tar.xz holds files of the unmodified Zig 0.17.0 release (https://ziglang.org), limited to
what x86_64 Linux builds use. Besides Zig's own code (above) it carries:
- LLVM's libc++, libc++abi and libunwind (Apache-2.0 WITH LLVM-exception; their LICENSE.TXT files
  are inside the archive, in zig/lib/libcxx, libcxxabi and libunwind),
- glibc headers and ABI lists (LGPL-2.1-or-later: licenses/LGPL-2.1.txt; other permission notices:
  licenses/glibc-LICENSES.txt),
- Linux kernel UAPI headers (GPL-2.0 WITH Linux-syscall-note: licenses/GPL-2.0.txt,
  licenses/Linux-syscall-note.txt).
EOF

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
