#!/usr/bin/env bash
# Downloads Jolt Physics into third_party/JoltPhysics (pinned release).
# Run once, then reconfigure; cmake picks it up via RECREATION_JOLT.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/third_party/JoltPhysics"
TAG="v5.2.0"

if [ -d "$DEST/Jolt" ]; then
  echo "already present: $DEST"
  exit 0
fi
git clone --depth 1 --branch "$TAG" https://github.com/jrouwe/JoltPhysics.git "$DEST"
echo "done: $DEST ($TAG)"
