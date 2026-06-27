#!/usr/bin/env bash
# Builds the managed scripting assembly (Recreation.Scripting) that ClrHost
# hosts. The engine build never depends on .NET; this is a separate, optional
# step run when managed scripting is wanted.
#
# Usage:
#   DOTNET_ROOT=/path/to/dotnet ./tools/build_managed.sh [output_dir]
#
# DOTNET_ROOT must point at a .NET root containing the dotnet CLI and the shared
# framework (e.g. a nix dotnet-sdk's share/dotnet). Output defaults to
# build/managed; pass that dir and the .runtimeconfig.json to pexrun hosttest.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$REPO/build/managed}"

# Prefer a dotnet already on PATH (the nix dev shell puts one there); otherwise
# fall back to DOTNET_ROOT. The runtime still needs DOTNET_ROOT set so ClrHost
# can find libhostfxr at run time.
DOTNET="$(command -v dotnet || true)"
if [ -z "$DOTNET" ] && [ -n "${DOTNET_ROOT:-}" ] && [ -x "$DOTNET_ROOT/dotnet" ]; then
  DOTNET="$DOTNET_ROOT/dotnet"
fi
if [ -z "$DOTNET" ]; then
  echo "no dotnet CLI on PATH and DOTNET_ROOT has none; enter 'nix develop' or set DOTNET_ROOT" >&2
  exit 1
fi

export DOTNET_CLI_HOME="${DOTNET_CLI_HOME:-$REPO/build/dotnet-home}"
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
mkdir -p "$DOTNET_CLI_HOME"

exec "$DOTNET" build "$REPO/sdk/Recreation.Scripting.csproj" \
  -c Release -o "$OUT" -v minimal
