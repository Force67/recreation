#!/usr/bin/env bash
# Builds recreation for the Steam Deck inside the Steam Runtime 3 (sniper)
# container, so the result links only libraries SteamOS actually has. See
# tools/deck/Dockerfile for why the container exists at all.
#
# The engine sources live in four sibling checkouts on this box; all four are
# bind-mounted so the container build sees the same tree the nix build does.
#
# Usage:
#   tools/deck/build.sh            # configure (if needed) + build
#   tools/deck/build.sh configure  # (re)run cmake configure only
#   tools/deck/build.sh shell      # interactive shell in the build container
#
# Env overrides: RECREATION_RX_DIR, RECREATION_ZETANET_DIR,
#                RECREATION_NANOBUF_DIR, DECK_BUILD_DIR, DECK_BUILD_TYPE.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RX="$(cd "${RECREATION_RX_DIR:-$REPO/../rx}" && pwd)"
ZETANET="$(cd "${RECREATION_ZETANET_DIR:-$REPO/../zetanet}" && pwd)"
NANOBUF="$(cd "${RECREATION_NANOBUF_DIR:-$REPO/../nanobuf}" && pwd)"
# rx's animation library, normally found as rx's own sibling.
KINEMA="$(cd "${RECREATION_KINEMA_DIR:-$REPO/../kinema}" && pwd)"
# GPU UI middleware behind the HUD and menus. Without it the build silently
# drops the HUD, which on a handheld is most of the interface.
UGUI="$(cd "${RECREATION_LIBULTRAGUI_DIR:-$REPO/../libultragui}" && pwd)"

IMAGE="${DECK_IMAGE:-recreation-deck:sniper}"

# rx compiles four .slang shaders and needs slangc for them. Every prebuilt
# slang release needs GLIBCXX_3.4.29, and sniper's gcc-14 ships libstdc++ only
# as a static archive, so none of them run in the container. The host's nix
# slangc is a self-contained closure, so bind-mounting /nix/store read-only and
# calling it by its absolute path works and keeps the exact version the desktop
# build uses. It is a build tool only: nothing from /nix ends up in the binary.
SLANGC="${RX_SLANGC:-}"
if [ -z "$SLANGC" ]; then
  SLANGC="$(nix build --no-link --print-out-paths nixpkgs#shader-slang 2>/dev/null)/bin/slangc"
fi
[ -x "$SLANGC" ] || { echo "no slangc; set RX_SLANGC to one runnable in the container" >&2; exit 1; }
# Kept out of build/nix so a Deck build and a desktop build can coexist.
BUILD_DIR="${DECK_BUILD_DIR:-build/deck}"
BUILD_TYPE="${DECK_BUILD_TYPE:-RelWithDebInfo}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "image $IMAGE missing; build it with:" >&2
  echo "  docker build -f tools/deck/Dockerfile -t $IMAGE tools/deck/" >&2
  exit 1
fi

# The Deck is RDNA2: DLSS and NRD are NVIDIA-only and their host libraries are
# x86_64 NVIDIA blobs, so they must be off or the link pulls in code that can
# never run. FSR3 is the upscaler the steamdeck tier selects.
configure_flags=(
  -G Ninja
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DRECREATION_RX_DIR=/rx
  -DRECREATION_ZETANET_DIR=/zetanet
  -DRECREATION_NANOBUF_DIR=/nanobuf
  -DRX_KINEMA_DIR=/kinema
  -DRECREATION_DLSS=OFF
  -DRECREATION_NRD=OFF
  -DRECREATION_FSR3=ON
  # The Deck renders through Vulkan natively; the D3D12 backend only exists to
  # validate against vkd3d on a desktop. Off here also drops the per-shader DXIL
  # sidecar, which is half the shader compile time.
  -DRX_RHI_D3D12=OFF
  # third_party/nanobuf/bin/nanoc is a prebuilt checked into the tree and needs
  # glibc 2.34, which sniper does not have. The checked-in nbuf/gen sources are
  # the documented offline fallback; regenerating the protocol is a dev-box
  # task, not something a target build needs to do.
  -DRECREATION_NANOBUF_REGEN=OFF
  -DRX_SLANGC="$SLANGC"
  -DRECREATION_LIBULTRAGUI_DIR=/libultragui
)

run() {
  # A tty only when there is one to give (CI and non-interactive shells).
  local tty=()
  [ -t 0 ] && tty=(-it)
  docker run --rm "${tty[@]}" \
    --user "$(id -u):$(id -g)" \
    -v "$REPO:/src" \
    -v "$RX:/rx" \
    -v "$ZETANET:/zetanet" \
    -v "$NANOBUF:/nanobuf" \
    -v "$KINEMA:/kinema" \
    -v "$UGUI:/libultragui" \
    -v /nix/store:/nix/store:ro \
    -w /src \
    "$IMAGE" "$@"
}

configure='cmake -B "'"$BUILD_DIR"'" '"${configure_flags[*]}"''
build='cmake --build "'"$BUILD_DIR"'" -j"$(nproc)"'

case "${1:-build}" in
  configure) run bash -c "$configure" ;;
  build)     run bash -c "[ -f '$BUILD_DIR/CMakeCache.txt' ] || $configure; $build" ;;
  shell)     run bash ;;
  *) echo "usage: ${0##*/} [build|configure|shell]" >&2; exit 1 ;;
esac
