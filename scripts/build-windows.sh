#!/usr/bin/env bash
# Cross-build the Windows (x86-64 PE) recreation from Linux, and run it under
# the Wine that winer builds.
#
# The compiler is winer's llvm-mingw (clang, UCRT target); the shader and code
# generators (dxc, slangc, nanoc) stay host Linux binaries and come from the nix
# dev shell, exactly as the native build gets them.
#
#   scripts/build-windows.sh              # configure (if needed) + build
#   scripts/build-windows.sh configure    # (re)run cmake configure only
#   scripts/build-windows.sh test         # build, then ctest the PEs under wine
#   scripts/build-windows.sh package      # assemble dist/win, a runnable tree
#   scripts/build-windows.sh run [args]   # build, then launch under wine
#
# Env: WINER_ROOT           winer checkout (default ../winer)
#      RECREATION_WIN_BUILD_DIR, RECREATION_BUILD_TYPE
#      RECREATION_WIN_JOBS  build width; defaults to a value derived from free
#                           memory, because clang at -j16 on this tree peaks
#                           well past what is left after a game is running.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINER_ROOT="$(cd "${WINER_ROOT:-$REPO/../winer}" && pwd)"
BUILD_DIR="${RECREATION_WIN_BUILD_DIR:-$REPO/build/win}"
BUILD_TYPE="${RECREATION_BUILD_TYPE:-RelWithDebInfo}"
ZETANET="$(cd "${RECREATION_ZETANET_DIR:-$REPO/../zetanet}" && pwd)"
NANOBUF="$(cd "${RECREATION_NANOBUF_DIR:-$REPO/../nanobuf}" && pwd)"
RX="$(cd "${RECREATION_RX_DIR:-$REPO/../rx}" && pwd)"
UGUI="$(cd "${RECREATION_LIBULTRAGUI_DIR:-$REPO/../libultragui}" && pwd)"

[ -x "$WINER_ROOT/build/toolchain/llvm-mingw/bin/x86_64-w64-mingw32-clang++" ] || {
  echo "winer llvm-mingw missing; run $WINER_ROOT/scripts/10-fetch-toolchain.sh" >&2
  exit 1
}

# Build width from free memory, not core count. Each clang here peaks around
# 1.5 GB on the heavier TUs (the papyrus VM, the bethesda record readers), and
# the box has no swap, so an over-wide build takes the session down instead of
# failing. Leave 4 GB of headroom for everything else.
jobs_for_memory() {
  local avail_mb per_job by_mem
  avail_mb=$(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo)
  per_job=1600
  by_mem=$(( (avail_mb - 4096) / per_job ))
  [ "$by_mem" -lt 1 ] && by_mem=1
  [ "$by_mem" -gt "$(nproc)" ] && by_mem=$(nproc)
  echo "$by_mem"
}
JOBS="${RECREATION_WIN_JOBS:-$(jobs_for_memory)}"

dev() {
  ( . "$REPO/devenv.sh"
    devenv_load build \
      --override-input zetanet-src "path:$ZETANET" \
      --override-input nanobuf-src "path:$NANOBUF" \
      --override-input rx-src "path:$RX"
    exec "$@" )
}

# rxpack packs the .rxp archives (shaders, engine fonts) during the build, so it
# has to run on this machine, not the target. Build a host copy from the same rx
# checkout: a tiny native tree with only the modules the tool links.
HOST_TOOLS_DIR="$REPO/build/host-tools"
host_rxpack() {
  local exe="$HOST_TOOLS_DIR/rxpack"
  if [ ! -x "$exe" ]; then
    echo "==> building a host rxpack" >&2
    dev cmake -B "$HOST_TOOLS_DIR" -S "$RX" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DRX_MODULES="core;asset" \
      -DRX_BUILD_RUNTIME=OFF -DRX_BUILD_TESTS=OFF -DRX_BUILD_TOOLS=ON \
      -DRX_RHI_VULKAN=OFF -DRX_RHI_D3D12=OFF \
      -DRX_JOLT=OFF -DRX_DLSS=OFF -DRX_NRD=OFF -DRX_FSR3=OFF -DRX_USD=OFF \
      -DRX_MIMALLOC=OFF -DRX_INSTALL=OFF >&2
    dev cmake --build "$HOST_TOOLS_DIR" --target rxpack -j"$JOBS" >&2
  fi
  echo "$exe"
}

# RX_USD is off because tinyusdz does not cross-compile: it builds itself with
# -fno-exceptions and its own Windows path throws, and it reaches for std::malloc
# without including <cstdlib>. USD scene loading is an authoring feature the game
# does not need, so this build goes without rather than patching a vendored
# download.
configure() {
  dev cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$WINER_ROOT/cmake/llvm-mingw.cmake" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DRX_RXPACK="$(host_rxpack)" \
    -DCMAKE_CROSSCOMPILING_EMULATOR="$WINER_ROOT/scripts/xrun.sh" \
    -DRECREATION_ZETANET_DIR="$ZETANET" \
    -DRECREATION_NANOBUF_DIR="$NANOBUF" \
    -DRECREATION_RX_DIR="$RX" \
    -DRECREATION_LIBULTRAGUI_DIR="$UGUI" \
    -DRECREATION_FETCH_SDL3=ON \
    -DRECREATION_DLSS=OFF \
    -DRECREATION_NRD=OFF \
    -DRECREATION_FSR3=OFF \
    -DRECREATION_AUDIO_FFMPEG=OFF \
    -DRX_USD=OFF \
    "$@"
}

# Assemble a tree that runs on a Windows machine that has never seen this repo:
# the executables plus every file they otherwise reach through a compiled-in
# build path. The runtime looks beside its own executable for each of these, so
# the layout here is the contract.
package() {
  local dist="${RECREATION_WIN_DIST:-$REPO/dist/win}"
  rm -rf "$dist"
  mkdir -p "$dist"
  cp "$BUILD_DIR/runtime/recreation.exe" "$BUILD_DIR/runtime/recreation-server.exe" "$dist/"
  for pack in "$BUILD_DIR/shaders.rxp" "$BUILD_DIR/rx/rx_fonts.rxp"; do
    [ -f "$pack" ] && cp "$pack" "$dist/"
  done
  cp -r "$REPO/runtime/ui/screens" "$dist/screens"
  [ -d "$REPO/runtime/ui/art" ] && cp -r "$REPO/runtime/ui/art" "$dist/art"
  [ -d "$REPO/runtime/ui/vanilla" ] && cp -r "$REPO/runtime/ui/vanilla" "$dist/vanilla"
  [ -d "$RX/engine/render/presets" ] && cp -r "$RX/engine/render/presets" "$dist/presets"
  # The engine's own Roboto, loose rather than only inside rx_fonts.rxp: the UI
  # loads a face by path, and a machine with no system font would otherwise draw
  # every label blank.
  if [ -d "$RX/engine/assets/fonts/roboto" ]; then
    mkdir -p "$dist/fonts"
    cp "$RX"/engine/assets/fonts/roboto/*.ttf "$dist/fonts/"
  fi
  if [ -d "$BUILD_DIR/sdk/managed" ]; then
    cp -r "$BUILD_DIR/sdk/managed" "$dist/managed"
  fi
  echo "==> $dist"
  du -sh "$dist"
}

cmd="${1:-build}"; shift || true
case "$cmd" in
  configure) configure "$@" ;;
  test)
    # ctest runs each Windows test through CMAKE_CROSSCOMPILING_EMULATOR, which
    # is winer's xrun.sh, so the regression suite exercises the real PEs.
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || configure
    dev cmake --build "$BUILD_DIR" -j"$JOBS"
    ( export XRUN_DESKTOP=0; cd "$BUILD_DIR"; dev ctest "$@" )
    ;;
  package)
    [ -f "$BUILD_DIR/runtime/recreation.exe" ] || { echo "build first" >&2; exit 1; }
    package
    ;;
  build)
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || configure
    echo "==> building with -j$JOBS ($(awk '/^MemAvailable:/{print int($2/1024)}' /proc/meminfo) MB available)"
    dev cmake --build "$BUILD_DIR" -j"$JOBS" "$@"
    ;;
  run)
    [ -f "$BUILD_DIR/CMakeCache.txt" ] || configure
    dev cmake --build "$BUILD_DIR" -j"$JOBS"
    exec "$WINER_ROOT/scripts/xrun.sh" "$BUILD_DIR/runtime/recreation.exe" "$@"
    ;;
  *) echo "usage: ${0##*/} [configure|build|test|package|run [args]]" >&2; exit 1 ;;
esac
