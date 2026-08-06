# Build environment for the Steam Deck, and for any Linux with glibc >= 2.31.
#
# Valve's Steam Runtime 3 ("sniper") is the same sysroot shipped games are built
# against, so a binary produced here runs on SteamOS without carrying the host's
# libraries along. That is the whole reason for the container: the nix build on
# the dev box links 62 /nix/store paths that do not exist on the Deck.
#
# Sniper already carries SDL3, Vulkan, freetype, harfbuzz, openssl and glslang,
# and it ships gcc-14 next to its default gcc-10. It needs no .NET: the CoreCLR
# host dlopens libhostfxr at runtime, so the managed assemblies and the runtime
# are staged at deploy time instead.
#
# The one real gap is dxc. Every official prebuilt needs glibc >= 2.33, so it
# gets built from source against sniper's own 2.31 in the first stage below.

# --- stage 1: dxc ------------------------------------------------------------
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest AS dxc

ARG DXC_TAG=v1.8.2505
RUN git clone --depth 1 --branch "$DXC_TAG" --recurse-submodules --shallow-submodules \
      https://github.com/microsoft/DirectXShaderCompiler.git /dxc

# PredefinedParams is upstream's own cache file for a dxc-only build; it turns
# on the SPIR-V backend, which is the half we actually use.
RUN cmake -S /dxc -B /dxc/build -G Ninja \
      -C /dxc/cmake/caches/PredefinedParams.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
      -DLLVM_ENABLE_WERROR=OFF \
 && ninja -C /dxc/build dxc

# --- stage 2: the build image ------------------------------------------------
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest

# Sniper defaults to gcc-10, which is C++17 only. gcc-14 has the C++23 the
# engine is written in.
ENV CC=gcc-14
ENV CXX=g++-14

# cmake/shaders.cmake resolves the compiler with a bare find_program(dxc), so
# being on PATH is all it takes.
COPY --from=dxc /dxc/build/bin/dxc /usr/local/bin/dxc
COPY --from=dxc /dxc/build/lib/libdxcompiler.so* /usr/local/lib/
RUN ldconfig

# Builds run as the invoking user (build.sh passes --user) so object files in
# the bind-mounted tree do not come back owned by root.
WORKDIR /src
