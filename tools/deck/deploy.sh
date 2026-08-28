#!/usr/bin/env bash
# Pushes a sniper-built tree to a Steam Deck over SSH and (optionally) runs it.
#
# Prerequisites on the Deck, once:
#   - a password for the deck user:  passwd
#   - sshd running:                  sudo systemctl enable --now sshd
#   - this box's key installed:      ssh-copy-id deck@<host>
# Developer mode and Valve's devkit service are not required for this path;
# steamos-devkit is the alternative transport when you want its pairing flow.
#
# Usage:
#   tools/deck/deploy.sh <host> [run [recreation args...]]
#
# Env overrides: DECK_USER (default deck), DECK_DEST (default ~/recreation),
#                DECK_BUILD_DIR (default build/deck).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RX="$(cd "${RECREATION_RX_DIR:-$REPO/../rx}" && pwd)"

HOST="${1:-}"
[ -n "$HOST" ] || { echo "usage: ${0##*/} <host> [run [args...]]" >&2; exit 1; }
shift

USER_NAME="${DECK_USER:-deck}"
DEST="${DECK_DEST:-recreation}"
BUILD_DIR="${DECK_BUILD_DIR:-$REPO/build/deck}"
TARGET="$USER_NAME@$HOST"

BIN="$BUILD_DIR/runtime/recreation"
[ -x "$BIN" ] || { echo "no binary at $BIN; run tools/deck/build.sh first" >&2; exit 1; }

# Refuse to ship a host build: those resolve through /nix/store, which the Deck
# has no copy of, and the failure on the far end is a bare "No such file or
# directory" with nothing pointing at the cause.
#
# This reads what the ELF itself records (its interpreter and RUNPATH), not what
# ldd resolves. ldd reports how the *running host* would satisfy the binary, so
# on this NixOS box it prints /nix/store paths even for a correct sniper build
# and rejected every deploy.
elf_interpreter() {
  if command -v patchelf >/dev/null 2>&1; then
    patchelf --print-interpreter "$1" 2>/dev/null && return 0
  fi
  readelf -l "$1" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)\]$/\1/p'
}

interp="$(elf_interpreter "$BIN" || true)"
runpath="$(patchelf --print-rpath "$BIN" 2>/dev/null || true)"
if [ -z "$interp" ]; then
  echo "warning: cannot read $BIN's ELF interpreter, skipping the portability check" >&2
elif case "$interp $runpath" in *"/nix/store"*) true ;; *) false ;; esac; then
  echo "$BIN is a host build (interpreter $interp), so it cannot run on SteamOS." >&2
  echo "Build it in the sniper container: tools/deck/build.sh" >&2
  exit 1
fi

echo "==> $TARGET:$DEST"
# Every destination directory up front. rsync only creates the final component
# of a path (--mkpath would do the rest, but it needs rsync 3.2.3+ on the far
# end), so the nested profile/preset trees have to exist before the transfers.
ssh "$TARGET" "mkdir -p '$DEST/runtime/app/profiles' '$DEST/engine/render/presets' '$DEST/sdk/managed'"

# No -z anywhere below. The binary is ~400MB of dense, already-incompressible
# code: measured against a local target, "rsync -az" had not finished after five
# minutes while plain "rsync -a" took under a second. Compression only pays on a
# slow link with compressible data, and this is neither.
#
# A RelWithDebInfo build is ~400MB, ~24MB of which is the program; the rest is
# debug info that is only useful next to a debugger. Strip what goes over the
# wire and keep the full binary here, so iterating on the Deck moves 24MB.
# DECK_STRIP=0 ships the unstripped binary when you want to debug on-device.
STAGE="$BUILD_DIR/.deploy"
mkdir -p "$STAGE"
if [ "${DECK_STRIP:-1}" = "1" ] && command -v strip >/dev/null 2>&1; then
  cp -f "$BIN" "$STAGE/recreation"
  # --strip-debug, not --strip-all: the dynamic symbol table has to survive or
  # the loader cannot resolve anything.
  strip --strip-debug "$STAGE/recreation"
  echo "    stripped $(du -h "$BIN" | cut -f1) -> $(du -h "$STAGE/recreation" | cut -f1)" \
       "(full binary stays at $BIN)"
  SEND_BIN="$STAGE/recreation"
else
  SEND_BIN="$BIN"
fi

rsync -a --info=progress2 "$SEND_BIN" "$TARGET:$DEST/recreation"

# Profiles are looked up relative to the working directory, so the tree layout
# has to survive the copy: runtime/app/profiles for recreation's own platform
# profiles, engine/render/presets for rx's render tiers.
rsync -a --delete "$REPO/runtime/app/profiles/" "$TARGET:$DEST/runtime/app/profiles/"
rsync -a --delete "$RX/engine/render/presets/"  "$TARGET:$DEST/engine/render/presets/"

# Managed assemblies are portable IL, so the host's dotnet build is what ships.
# The CLR itself is dlopened from DOTNET_ROOT; install a linux-x64 runtime on
# the Deck once and point the launcher at it.
if [ -d "$REPO/build/nix/sdk/managed" ]; then
  rsync -a --delete "$REPO/build/nix/sdk/managed/" "$TARGET:$DEST/sdk/managed/"
fi

echo "==> deployed"

if [ "${1:-}" = "run" ]; then
  shift
  # gamescope owns the display in Game Mode; from an SSH shell the reliable
  # target is the desktop session's compositor.
  ssh -t "$TARGET" "cd '$DEST' && DISPLAY=:0 ./recreation --profile steamdeck $*"
fi
