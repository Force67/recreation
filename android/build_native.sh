#!/usr/bin/env bash
# Cross-compiles librecreation.so for Android and stages it into the app's
# jniLibs so Gradle can package it. The native build needs the host dxc (for
# shader compilation) and the pinned FetchContent sources, both provided by the
# nix dev shell; the Gradle build itself stays toolchain-light.
#
# Usage: android/build_native.sh [abi]   (abi defaults to arm64-v8a)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="${1:-arm64-v8a}"
API="${RECREATION_ANDROID_API:-33}"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/android-ndk-r27c}"
BUILD_DIR="$REPO/build/android-$ABI"
ZETANET="$(cd "${RECREATION_ZETANET_DIR:-$REPO/../zetanet}" && pwd)"
JNILIBS="$REPO/android/app/src/main/jniLibs/$ABI"

dev() { nix develop --override-input zetanet-src "path:$ZETANET" --command "$@"; }

dev bash -c "
  set -e
  cmake -B '$BUILD_DIR' -G Ninja \$RECREATION_FETCHCONTENT_FLAGS \
    -DCMAKE_TOOLCHAIN_FILE='$NDK/build/cmake/android.toolchain.cmake' \
    -DANDROID_ABI='$ABI' -DANDROID_PLATFORM='android-$API' \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build '$BUILD_DIR' --target recreation -j\"\$(nproc)\"
"

mkdir -p "$JNILIBS"
cp "$BUILD_DIR/runtime/librecreation.so" "$JNILIBS/librecreation.so"

# Ship the Vulkan validation layer next to the engine so the loader discovers
# it; enabled at runtime only when the launcher writes validation=1. The NDK no
# longer bundles it, so fetch the prebuilt from the Khronos release once.
LAYER="$JNILIBS/libVkLayer_khronos_validation.so"
VVL_VERSION="${RECREATION_VVL_VERSION:-1.4.350.0}"
if [ ! -f "$LAYER" ]; then
  ZIP="$REPO/build/vvl-$VVL_VERSION.zip"
  if [ ! -f "$ZIP" ]; then
    curl -sL -o "$ZIP" \
      "https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-$VVL_VERSION/android-binaries-$VVL_VERSION.zip"
  fi
  python3 -c "
import zipfile, shutil, sys
z = zipfile.ZipFile('$ZIP')
member = 'android-binaries-$VVL_VERSION/$ABI/libVkLayer_khronos_validation.so'
with z.open(member) as src, open('$LAYER', 'wb') as dst:
    shutil.copyfileobj(src, dst)
"
fi

echo "staged $ABI -> $JNILIBS"
ls -la "$JNILIBS"
