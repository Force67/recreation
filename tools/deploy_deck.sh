#!/usr/bin/env bash
# Build recreation for the Steam Deck (Zen 2, SteamOS) and push it to one.
#
#   tools/deploy_deck.sh [deck@host]          build + sync to ~/recreation
#
# The nix toolchain's glibc is older than SteamOS's, so the binary runs on the
# Deck's own loader and libraries once the nix interpreter and RUNPATH are
# stripped: SteamOS ships libSDL3, freetype, harfbuzz, libwayland-client and a
# newer libstdc++, and Vulkan comes from the system loader, so gamescope's WSI
# layer, MangoHud and the Deck's RADV all apply as they would to any Steam game.
# tools/deck/ is the alternative that builds inside the Steam Runtime instead.
#
# The Bethesda data is not shipped: point the game at a copy on the Deck with
# --data-dir. DECK_SSH_KEY picks the identity (default ~/.ssh/steamdeck_devkit,
# the key the devkit pairing installs).
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET="${1:-deck@steamdeck}"
KEY="${DECK_SSH_KEY:-$HOME/.ssh/steamdeck_devkit}"
BUILD=build-deck
STAGE="$BUILD/stage"

# DLSS and NRD are NVIDIA-only; the Deck upscales with FSR3. The d3d12 backend
# only exists to validate against vkd3d on a desktop.
nix develop -c bash -c "
  set -euo pipefail
  if [ ! -f $BUILD/build.ninja ]; then
    cmake -B $BUILD -G Ninja \$RECREATION_FETCHCONTENT_FLAGS \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS=-march=znver2 -DCMAKE_CXX_FLAGS=-march=znver2 \
      -DRX_RHI_D3D12=OFF -DRECREATION_DLSS=OFF -DRECREATION_NRD=OFF
  fi
  ninja -C $BUILD recreation
"

rm -rf "$STAGE"
mkdir -p "$STAGE"
# The unstripped binary stays in $BUILD for gdb on this side.
nix shell nixpkgs#patchelf -c patchelf \
  --set-interpreter /lib64/ld-linux-x86-64.so.2 --remove-rpath \
  --output "$STAGE/recreation" "$BUILD/runtime/recreation"
strip --strip-debug "$STAGE/recreation"
# The install layout the build staged beside the binary (rx docs/CONFIG.md):
# Data/rx_engine.rxp -> rxe://, Data/recreation.rxp (shaders, ui) and config/
# -> recreation://.
cp -r "$BUILD/runtime/config" "$BUILD/runtime/Data" "$STAGE/"

rsync -az --delete -e "ssh -i $KEY" "$STAGE/" "$TARGET:recreation/"
echo "deployed to $TARGET:~/recreation (run ~/recreation/recreation --data-dir <Data>)"
