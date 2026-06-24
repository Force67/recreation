#!/usr/bin/env bash
# Detect the host OS and hand off to the matching recreation setup script.
# All arguments are passed through (e.g. --check, --system, -y).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in
  Linux)  exec bash "$here/setup-linux.sh" "$@" ;;
  Darwin) exec bash "$here/setup-macos.sh" "$@" ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    echo "On Windows run: powershell -ExecutionPolicy Bypass -File scripts/setup-windows.ps1 $*" >&2
    exit 1 ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
