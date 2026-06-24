#!/usr/bin/env bash
# End-to-end dev setup for recreation on macOS (Apple Silicon / Intel).
#
# Installs the Homebrew toolchain, builds the dxc shader compiler from source
# (Microsoft ships no macOS binary), fetches third-party deps and the sibling
# repos, then reports anything still missing. Safe to re-run.
#
# Usage: scripts/setup-macos.sh [--check] [--system] [--dxc] [--deps] [-y]
#   (no flags)   do everything
#   --check      report only, change nothing
#   --system     install Homebrew packages
#   --dxc        build + install the DirectX Shader Compiler
#   --deps       fetch third-party deps + clone sibling repos
#   -y           assume yes / non-interactive (implied under CI)
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

DXC_TAG="${DXC_TAG:-v1.9.2602.24}"
DXC_PREFIX="${DXC_PREFIX:-$HOME/.local/dxc}"   # dev install location, on PATH below
BREW_PKGS="cmake ninja git pkg-config freetype harfbuzz"

do_system() {
  have brew || blocker "Homebrew not found - install from https://brew.sh then re-run"
  have brew || return 0
  log "installing Homebrew packages"
  # shellcheck disable=SC2086
  brew install $BREW_PKGS
  # MoltenVK provides Vulkan over Metal; needed to run, optional to build.
  brew list molten-vk >/dev/null 2>&1 || brew install molten-vk || warn "molten-vk install failed (only needed at run time)"
  # Hand pkg-config the brew kegs so libultragui finds freetype/harfbuzz.
  if [ -n "${GITHUB_ENV:-}" ]; then
    echo "PKG_CONFIG_PATH=$(brew --prefix freetype)/lib/pkgconfig:$(brew --prefix harfbuzz)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig" >> "$GITHUB_ENV"
  fi
  ok "Homebrew packages installed"
}

do_dxc() {
  if have dxc; then ok "dxc already on PATH ($(command -v dxc))"; return 0; fi
  if [ -x "$DXC_PREFIX/bin/dxc" ]; then ok "dxc present at $DXC_PREFIX (add $DXC_PREFIX/bin to PATH)"; return 0; fi
  if ! confirm "build dxc $DXC_TAG from source (slow, needs ~10 min)?"; then
    blocker "dxc not installed - build it: see the macos job in .github/workflows/build.yml"
    return 0
  fi
  have cmake && have ninja && have git || die "building dxc needs cmake, ninja and git (run --system first)"
  local src=dxc-src build=dxc-build
  rm -rf "$src" "$build"
  git clone --depth 1 --branch "$DXC_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/DirectXShaderCompiler "$src"
  cmake -B "$build" -G Ninja "$src" -C "$src/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_SPIRV_CODEGEN=ON -DSPIRV_BUILD_TESTS=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DHLSL_DISABLE_SOURCE_GENERATION=ON
  cmake --build "$build" --target dxc
  mkdir -p "$DXC_PREFIX/bin" "$DXC_PREFIX/lib"
  cp "$build/bin/dxc" "$DXC_PREFIX/bin/"
  cp "$build"/lib/libdxcompiler.dylib "$DXC_PREFIX/lib/"
  chmod +x "$DXC_PREFIX/bin/dxc"
  rm -rf "$src" "$build"
  ok "dxc installed to $DXC_PREFIX - add to your shell: export PATH=\"$DXC_PREFIX/bin:\$PATH\""
}

do_thirdparty() {
  log "fetching third-party dependencies"
  # DLSS/FSR3/NRD are off on Apple (MoltenVK), so only these are needed.
  bash "$REPO_DIR/tools/get_nanobuf.sh"
  bash "$REPO_DIR/tools/get_jolt.sh"
}

clone_sibling() { # name repo
  local dir="$REPO_DIR/../$1"
  if [ -e "$dir/CMakeLists.txt" ]; then ok "$1 present ($dir)"; return 0; fi
  log "cloning $1 next to recreation"
  git clone --recurse-submodules "$2" "$dir"
}

do_siblings() {
  clone_sibling zetanet     https://github.com/Force67/zetanet
  clone_sibling libultragui https://github.com/Force67/libultragui
  git -C "$REPO_DIR/../zetanet" checkout develop 2>/dev/null || true
  git -C "$REPO_DIR/../zetanet" submodule update --init --recursive 2>/dev/null || true
}

do_doctor() {
  log "checking the toolchain"
  check_tool cmake "cmake" "brew install cmake"
  check_tool ninja "ninja" "brew install ninja"
  check_tool clang++ "C++ compiler" "install Xcode command line tools: xcode-select --install"
  check_tool git "git" "xcode-select --install"
  check_tool pkg-config "pkg-config" "brew install pkg-config"
  if have dxc || [ -x "$DXC_PREFIX/bin/dxc" ]; then ok "dxc"
  else blocker "dxc missing - run: scripts/setup-macos.sh --dxc"; fi
  if have pkg-config && pkg-config --exists freetype2 harfbuzz; then ok "freetype + harfbuzz"
  else warn "freetype/harfbuzz not on pkg-config path - configure with -DCMAKE_PREFIX_PATH=\$(brew --prefix)"; fi
  brew list molten-vk >/dev/null 2>&1 && ok "MoltenVK (Vulkan runtime)" \
    || warn "MoltenVK not installed - needed to run the renderer (brew install molten-vk)"
  [ -e "$REPO_DIR/../zetanet/CMakeLists.txt" ] && ok "zetanet sibling" \
    || blocker "zetanet missing - run: scripts/setup-macos.sh --deps"
  [ -e "$REPO_DIR/../libultragui/CMakeLists.txt" ] && ok "libultragui sibling" \
    || warn "libultragui missing - HUD/menus compile out (scripts/setup-macos.sh --deps)"
}

DO_SYS=0 DO_DXC=0 DO_TP=0 DO_SIB=0 CHECK_ONLY=0 ANY=0
for a in "$@"; do
  case "$a" in
    --check)   CHECK_ONLY=1 ;;
    --system)  DO_SYS=1; ANY=1 ;;
    --dxc)     DO_DXC=1; ANY=1 ;;
    --deps)    DO_TP=1; DO_SIB=1; ANY=1 ;;
    -y|--yes)  ASSUME_YES=1 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) die "unknown option: $a" ;;
  esac
done
if [ "$CHECK_ONLY" = "1" ]; then do_doctor; print_report; exit $?; fi
DEFAULT_ALL=0
if [ "$ANY" = "0" ]; then DO_SYS=1 DO_DXC=1 DO_TP=1 DO_SIB=1; DEFAULT_ALL=1; fi
[ "$DO_SYS" = "1" ] && do_system
[ "$DO_DXC" = "1" ] && do_dxc
[ "$DO_TP"  = "1" ] && do_thirdparty
[ "$DO_SIB" = "1" ] && do_siblings
if [ "$DEFAULT_ALL" = "1" ]; then do_doctor; print_report || true; fi
