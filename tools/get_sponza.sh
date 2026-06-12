#!/usr/bin/env bash
# Downloads the Khronos Sponza sample (glTF + textures, ~100 MB) into
# assets/sponza/. Run once, then: ./build.sh run -- --gltf assets/sponza/Sponza.gltf
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/assets/sponza"
BASE="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Sponza/glTF"
API="https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/Sponza/glTF"

mkdir -p "$DEST"
echo "fetching file list..."
files=$(curl -sfL "$API" | grep '"name"' | sed 's/.*"name": "\(.*\)",/\1/')
[ -n "$files" ] || { echo "could not list files (github api rate limit?)" >&2; exit 1; }

total=$(echo "$files" | wc -l)
n=0
for f in $files; do
  n=$((n + 1))
  if [ -s "$DEST/$f" ]; then
    echo "[$n/$total] $f (cached)"
    continue
  fi
  echo "[$n/$total] $f"
  curl -sfL "$BASE/$f" -o "$DEST/$f"
done
echo "done: $DEST/Sponza.gltf"
