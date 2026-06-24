#!/usr/bin/env bash
# End-to-end dev setup for recreation on Linux (Debian/Ubuntu).
#
# Brings an unknown box to a buildable state: system packages, the dxc shader
# compiler, the fetched third-party deps and the sibling repos, then a doctor
# report of anything still missing. Safe to re-run.
#
# Usage: scripts/setup-linux.sh [--check] [--system] [--dxc] [--deps]
#                               [--minimal] [-y]
#   (no flags)   do everything
#   --check      report only, change nothing
#   --system     install system packages (apt)
#   --dxc        install the DirectX Shader Compiler
#   --deps       fetch third-party deps + clone sibling repos
#   --minimal    system: toolchain only, skip the GUI/X11/Wayland stack
#                (used by the headless android cross-build)
#   -y           assume yes / non-interactive (implied under CI)
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

DXC_TAG="${DXC_TAG:-v1.9.2602.24}"
DXC_LINUX_X64_ASSET="linux_dxc_2026_05_26.x86_64.tar.gz"
ARCH="$(uname -m)"

# ---- packages -------------------------------------------------------------
PKGS_CORE="build-essential cmake ninja-build git pkg-config zlib1g-dev"
PKGS_SHADERS="glslang-tools"
PKGS_UI="libfreetype-dev libharfbuzz-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libgl-dev libegl-dev libdrm-dev libgbm-dev"
# Optional at build time, needed to actually run the renderer.
PKGS_RUNTIME="libvulkan1 vulkan-tools mesa-vulkan-drivers"

do_system() {
  if ! have apt-get; then
    blocker "non-apt distro: install the equivalents of [$PKGS_CORE $PKGS_SHADERS $PKGS_UI] with your package manager"
    return 0
  fi
  local pkgs="$PKGS_CORE"
  if [ "${MINIMAL:-0}" = "1" ]; then
    log "installing minimal toolchain (headless cross-build)"
  else
    pkgs="$pkgs $PKGS_SHADERS $PKGS_UI $PKGS_RUNTIME"
    log "installing build toolchain + GUI/runtime libraries"
  fi
  $SUDO apt-get update -qq
  # shellcheck disable=SC2086
  $SUDO apt-get install -y $pkgs
  ok "apt packages installed"
}

# ---- dxc ------------------------------------------------------------------
do_dxc() {
  if have dxc; then ok "dxc already on PATH ($(command -v dxc))"; return 0; fi
  case "$ARCH" in
    x86_64|amd64)
      log "installing prebuilt dxc $DXC_TAG"
      curl -fsSL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_TAG/$DXC_LINUX_X64_ASSET" \
        | $SUDO tar -xz -C /usr/local
      $SUDO chmod +x /usr/local/bin/dxc
      $SUDO ldconfig
      ok "dxc installed to /usr/local" ;;
    aarch64|arm64)
      # Microsoft ships no aarch64 Linux dxc, so it is built from source.
      if ! confirm "no aarch64 dxc release exists; build it from source (slow, needs ~10 min)?"; then
        blocker "dxc not installed - build it: see the linux-arm job in .github/workflows/build.yml"
        return 0
      fi
      build_dxc_from_source ;;
    *) blocker "unknown arch '$ARCH' - install dxc manually" ;;
  esac
}

build_dxc_from_source() {
  have cmake && have ninja && have git || die "building dxc needs cmake, ninja and git (run --system first)"
  local src=dxc-src build=dxc-build
  rm -rf "$src" "$build"
  git clone --depth 1 --branch "$DXC_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/DirectXShaderCompiler "$src"
  cmake -B "$build" -G Ninja "$src" -C "$src/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_SPIRV_CODEGEN=ON -DSPIRV_BUILD_TESTS=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DHLSL_DISABLE_SOURCE_GENERATION=ON
  cmake --build "$build" --target dxc
  $SUDO cp "$build/bin/dxc" /usr/local/bin/
  $SUDO cp -a "$build"/lib/libdxcompiler.so* /usr/local/lib/
  $SUDO chmod +x /usr/local/bin/dxc
  $SUDO ldconfig
  rm -rf "$src" "$build"
  ok "dxc built and installed to /usr/local"
}

# ---- third-party + siblings ----------------------------------------------
do_thirdparty() {
  log "fetching third-party dependencies"
  bash "$REPO_DIR/tools/get_nanobuf.sh"
  bash "$REPO_DIR/tools/get_fidelityfx.sh"
  [ "$ARCH" = "x86_64" ] && bash "$REPO_DIR/tools/get_dlss.sh"   # DLSS is x86_64 only
  bash "$REPO_DIR/tools/get_nrd.sh"
  bash "$REPO_DIR/tools/get_jolt.sh"
}

clone_sibling() { # name repo
  local dir="$REPO_DIR/../$1"
  if [ -e "$dir/CMakeLists.txt" ]; then ok "$1 present ($dir)"; return 0; fi
  log "cloning $1 next to recreation"
  git clone --recurse-submodules "$2" "$dir"
}

do_siblings() {
  clone_sibling zetanet    https://github.com/Force67/zetanet
  clone_sibling libultragui https://github.com/Force67/libultragui
  # recreation tracks zetanet's develop branch.
  git -C "$REPO_DIR/../zetanet" checkout develop 2>/dev/null || true
  git -C "$REPO_DIR/../zetanet" submodule update --init --recursive 2>/dev/null || true
}

# ---- doctor ---------------------------------------------------------------
do_doctor() {
  log "checking the toolchain"
  check_tool cmake "cmake"   "apt install cmake (>=3.20)"
  check_tool ninja "ninja"   "apt install ninja-build"
  check_tool c++   "C++ compiler" "apt install build-essential"
  check_tool git   "git"     "apt install git"
  if [ "${MINIMAL:-0}" != "1" ]; then
    check_tool pkg-config "pkg-config" "apt install pkg-config"
    check_tool dxc "dxc" "run: scripts/setup-linux.sh --dxc"
    if have glslang || have glslangValidator; then ok "glslang"
    else blocker "glslang missing - apt install glslang-tools"; fi
    if have pkg-config && pkg-config --exists freetype2 harfbuzz; then ok "freetype + harfbuzz"
    else blocker "freetype/harfbuzz dev libs missing - apt install libfreetype-dev libharfbuzz-dev"; fi
    have dotnet && ok "dotnet ($(dotnet --version 2>/dev/null))" \
      || warn "dotnet SDK 9 not found (only needed for C# scripting) - https://dotnet.microsoft.com/download"
    have vulkaninfo && ok "vulkan runtime present" \
      || warn "no Vulkan driver detected - the renderer needs one at run time (build still works)"
  fi
  [ -e "$REPO_DIR/../zetanet/CMakeLists.txt" ] && ok "zetanet sibling" \
    || blocker "zetanet missing - run: scripts/setup-linux.sh --deps"
  [ -e "$REPO_DIR/../libultragui/CMakeLists.txt" ] && ok "libultragui sibling" \
    || warn "libultragui missing - HUD/menus compile out (scripts/setup-linux.sh --deps)"
}

# ---- arg parsing ----------------------------------------------------------
DO_SYS=0 DO_DXC=0 DO_TP=0 DO_SIB=0 CHECK_ONLY=0 MINIMAL=0 ANY=0
for a in "$@"; do
  case "$a" in
    --check)    CHECK_ONLY=1 ;;
    --system)   DO_SYS=1; ANY=1 ;;
    --dxc)      DO_DXC=1; ANY=1 ;;
    --deps)     DO_TP=1; DO_SIB=1; ANY=1 ;;
    --minimal)  MINIMAL=1 ;;
    -y|--yes)   ASSUME_YES=1 ;;
    -h|--help)  sed -n '2,20p' "$0"; exit 0 ;;
    *) die "unknown option: $a" ;;
  esac
done
# --check is the audit gate (exits nonzero if anything blocks a build).
if [ "$CHECK_ONLY" = "1" ]; then do_doctor; print_report; exit $?; fi

# Default (no action flag) means a full developer setup; granular flags (as CI
# uses) run just that step and skip the doctor, which expects the full layout.
DEFAULT_ALL=0
if [ "$ANY" = "0" ]; then DO_SYS=1 DO_DXC=1 DO_TP=1 DO_SIB=1; DEFAULT_ALL=1; fi

[ "$DO_SYS" = "1" ] && do_system
[ "$DO_DXC" = "1" ] && [ "$MINIMAL" != "1" ] && do_dxc
[ "$DO_TP"  = "1" ] && do_thirdparty
[ "$DO_SIB" = "1" ] && do_siblings
if [ "$DEFAULT_ALL" = "1" ]; then do_doctor; print_report || true; fi
