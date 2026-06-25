#!/usr/bin/env bash
# Downloads NVIDIA NRD (the ray tracing denoiser) plus its two build
# dependencies into third_party. NRD's own CMake pulls MathLib and ShaderMake
# via FetchContent, but the nix build runs disconnected, so they are vendored
# alongside and third_party/nrd.cmake redirects FetchContent at them. NRI is
# not needed: the engine drives NRD through the direct Vulkan path.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$REPO_DIR/third_party"

NRD_TAG="v4.17.3"
MATHLIB_TAG="v11"
# ShaderMake is pinned by NRD v4.17.3 to this commit (see NRD CMakeLists.txt).
SHADERMAKE_SHA="18f5a344e7ca8fa65daaf079d07bc8ce38453e05"

if [ ! -f "$TP/NRD/Include/NRD.h" ]; then
  rm -rf "$TP/NRD"
  git clone --depth 1 --branch "$NRD_TAG" \
    https://github.com/NVIDIAGameWorks/RayTracingDenoiser "$TP/NRD"
fi

if [ ! -f "$TP/NRD-MathLib/CMakeLists.txt" ]; then
  rm -rf "$TP/NRD-MathLib"
  git clone --depth 1 --branch "$MATHLIB_TAG" \
    https://github.com/NVIDIA-RTX/MathLib "$TP/NRD-MathLib"
fi

if [ ! -f "$TP/NRD-ShaderMake/CMakeLists.txt" ]; then
  rm -rf "$TP/NRD-ShaderMake"
  mkdir -p "$TP/NRD-ShaderMake"
  git -C "$TP/NRD-ShaderMake" init -q
  git -C "$TP/NRD-ShaderMake" remote add origin https://github.com/NVIDIA-RTX/ShaderMake
  git -C "$TP/NRD-ShaderMake" fetch -q --depth 1 origin "$SHADERMAKE_SHA"
  git -C "$TP/NRD-ShaderMake" checkout -q FETCH_HEAD
fi

echo "done: NRD $NRD_TAG, MathLib $MATHLIB_TAG, ShaderMake ${SHADERMAKE_SHA:0:10}"
