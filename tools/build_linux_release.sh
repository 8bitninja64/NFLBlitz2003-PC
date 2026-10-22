#!/bin/sh
# Builds the Linux release runtime the way CI does, for the widest range of distributions: run it in
# an Ubuntu 22.04 (glibc 2.35) container. Jammy's clang 14/15 cannot build Aurora's C++20 or Dawn's
# headers, so it installs clang 18 (apt.llvm.org, key fingerprint checked) with g++-12's libstdc++
# (clang picks the newest GCC it finds; 11's <ranges> is too old) and a newer CMake; Dawn and SDL3
# are built from source (Aurora's prebuilt Dawn needs a newer glibc), and no system libraries are
# linked dynamically besides glibc/libstdc++ (no libsqlite3/libzstd/libpng needed on the player's
# system). The approach follows the Super Mario Strikers port's release workflow.
#   podman run --rm -v <repo>:/src -v <build dir>:/build ubuntu:22.04 sh /src/tools/build_linux_release.sh /src /build
#   tools/build_linux_release.sh [source dir] [build dir]     (BLITZ_SKIP_DEPS=1: skip the apt part)
set -e
src=${1:-$(cd "$(dirname "$0")/.." && pwd)}
bdir=${2:-"$src/build/linux-release"}
export DEBIAN_FRONTEND=noninteractive
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo

if [ -z "$BLITZ_SKIP_DEPS" ]; then
  . /etc/os-release
  $SUDO apt-get update -qq
  $SUDO apt-get install -y -qq --no-install-recommends \
    ca-certificates curl git gnupg xz-utils python3 python3-pip pkg-config ninja-build \
    libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
    libxinerama-dev libxss-dev libxtst-dev libxkbcommon-dev \
    libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
    libwayland-dev wayland-protocols libdecor-0-dev libdbus-1-dev libudev-dev libusb-1.0-0-dev >/dev/null
  if [ "$VERSION_CODENAME" = jammy ]; then
    fpr=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor > /tmp/llvm.gpg
    gpg --show-keys --with-colons /tmp/llvm.gpg | grep -q "^fpr:*$fpr:" ||
      { echo "apt.llvm.org key is not $fpr" >&2; exit 1; }
    $SUDO install -m 644 /tmp/llvm.gpg /usr/share/keyrings/llvm.gpg
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" |
      $SUDO tee /etc/apt/sources.list.d/llvm-18.list >/dev/null
    $SUDO apt-get update -qq
    $SUDO apt-get install -y -qq --no-install-recommends clang-18 lld-18 g++-12 >/dev/null
    $SUDO update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100
    $SUDO update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100
    $SUDO python3 -m pip install -q --no-cache-dir 'cmake==3.31.6'
  else
    $SUDO apt-get install -y -qq --no-install-recommends clang lld cmake >/dev/null
  fi
fi

cd "$src"
git config --global --add safe.directory '*'   # a mounted tree owned by another user
# Local fixes to Aurora (patches/aurora/*.patch), applied once to the submodule checkout.
for patch in patches/aurora/*.patch; do
  [ -f "$patch" ] || continue
  git -C extern/aurora apply --ignore-whitespace --reverse --check "$src/$patch" 2>/dev/null && continue
  git -C extern/aurora apply --ignore-whitespace "$src/$patch"
done
cmake -S "$src" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBLITZ_GEN_DIR=/nonexistent \
  -DAURORA_DAWN_PROVIDER=vendor -DAURORA_SDL3_PROVIDER=vendor \
  -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON -DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=ON -DCMAKE_DISABLE_FIND_PACKAGE_absl=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_fmt=ON
cmake --build "$bdir" --target blitz
echo "built $bdir/NFLBlitz2003"
