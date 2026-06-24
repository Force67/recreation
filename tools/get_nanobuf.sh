#!/usr/bin/env bash
# Downloads the nanoc schema compiler and the matching nanobuf C++ runtime
# header from the nanobuf GitHub release into third_party/nanobuf/. CMake picks
# them up (via RECREATION_NANOC + the fetched runtime) and regenerates the wire
# protocol from nbuf/*.nb at configure time, so the protocol always tracks the
# released compiler. No Rust or nanobuf checkout required.
#
#   NANOBUF_VERSION   release tag to fetch (default: latest)
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/third_party/nanobuf"
REPO="Force67/nanobuf"
VERSION="${NANOBUF_VERSION:-latest}"

os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Linux)
    case "$arch" in
      x86_64)         target="x86_64-unknown-linux-gnu" ;;
      aarch64|arm64)  target="aarch64-unknown-linux-gnu" ;;
      *) echo "get_nanobuf: unsupported Linux arch '$arch'" >&2; exit 1 ;;
    esac
    ext="tar.gz"; bin="nanoc" ;;
  Darwin)
    case "$arch" in
      x86_64)         target="x86_64-apple-darwin" ;;
      arm64|aarch64)  target="aarch64-apple-darwin" ;;
      *) echo "get_nanobuf: unsupported macOS arch '$arch'" >&2; exit 1 ;;
    esac
    ext="tar.gz"; bin="nanoc" ;;
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    case "$arch" in
      x86_64)         target="x86_64-pc-windows-msvc" ;;
      aarch64|arm64)  target="aarch64-pc-windows-msvc" ;;
      *) echo "get_nanobuf: unsupported Windows arch '$arch'" >&2; exit 1 ;;
    esac
    ext="zip"; bin="nanoc.exe" ;;
  *) echo "get_nanobuf: unsupported OS '$os'" >&2; exit 1 ;;
esac

if [ "$VERSION" = "latest" ]; then
  base="https://github.com/$REPO/releases/latest/download"
else
  base="https://github.com/$REPO/releases/download/$VERSION"
fi

nanoc_asset="nanoc-$target.$ext"
runtimes_asset="nanobuf-runtimes.tar.gz"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "get_nanobuf: fetching $nanoc_asset + $runtimes_asset ($VERSION)"
curl -fsSL -o "$tmp/$nanoc_asset"    "$base/$nanoc_asset"
curl -fsSL -o "$tmp/$runtimes_asset" "$base/$runtimes_asset"
curl -fsSL -o "$tmp/SHA256SUMS"      "$base/SHA256SUMS"

# Verify the assets we use against the published checksums.
( cd "$tmp" && grep -F -e "$nanoc_asset" -e "$runtimes_asset" SHA256SUMS | sha256sum -c - )

mkdir -p "$tmp/x"
case "$ext" in
  tar.gz) tar xzf "$tmp/$nanoc_asset" -C "$tmp/x" ;;
  zip)
    if command -v unzip >/dev/null 2>&1; then
      unzip -q -o "$tmp/$nanoc_asset" -d "$tmp/x"
    elif command -v powershell.exe >/dev/null 2>&1; then
      powershell.exe -NoProfile -Command \
        "Expand-Archive -Force -LiteralPath '$(cygpath -w "$tmp/$nanoc_asset")' -DestinationPath '$(cygpath -w "$tmp/x")'"
    else
      tar -xf "$tmp/$nanoc_asset" -C "$tmp/x"
    fi ;;
esac
tar xzf "$tmp/$runtimes_asset" -C "$tmp"

nanoc_bin="$(find "$tmp/x" -name "$bin" -type f | head -n1)"
[ -n "$nanoc_bin" ] || { echo "get_nanobuf: '$bin' not found in $nanoc_asset" >&2; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST/bin" "$DEST/include"
cp "$nanoc_bin" "$DEST/bin/$bin"
chmod +x "$DEST/bin/$bin" 2>/dev/null || true
cp "$tmp/runtimes/cpp/nanobuf.h" "$DEST/include/nanobuf.h"
echo "$VERSION ($target)" > "$DEST/VERSION"

echo "get_nanobuf: installed to $DEST"
"$DEST/bin/$bin" --version
