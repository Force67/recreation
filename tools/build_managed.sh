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

if [ -z "${DOTNET_ROOT:-}" ] || [ ! -x "$DOTNET_ROOT/dotnet" ]; then
  echo "DOTNET_ROOT must point at a .NET root with a dotnet CLI" >&2
  exit 1
fi

export PATH="$DOTNET_ROOT:$PATH"
export DOTNET_CLI_HOME="${DOTNET_CLI_HOME:-$REPO/build/dotnet-home}"
export DOTNET_NOLOGO=1 DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
mkdir -p "$DOTNET_CLI_HOME"

exec dotnet build "$REPO/engine/script/managed/Recreation.Scripting.csproj" \
  -c Release -o "$OUT" -v minimal
