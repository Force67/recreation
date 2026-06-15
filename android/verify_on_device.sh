#!/usr/bin/env bash
# On-device smoke test: pushes a real glTF asset tree to shared storage, grants
# all-files access, runs the engine against it, and pulls the rendered frame +
# the validation log. Proves real asset loading off storage on the device (the
# same loaders the Bethesda games use), independent of the platform
# screenshotter.
#
# Usage: android/verify_on_device.sh [demo|gltf]   (default: gltf sponza)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG=com.recreation.launcher
ADB="${ADB:-$HOME/Android/Sdk/platform-tools/adb}"
MODE="${1:-gltf}"
SHOT_DIR="$REPO/android/screenshots"
mkdir -p "$SHOT_DIR"

dev() { "$ADB" "$@"; }

[ -n "$(dev devices | grep -w device | grep -v List)" ] || { echo "no device connected"; exit 1; }

dev shell svc power stayon usb >/dev/null 2>&1 || true
dev shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true

# Native file I/O needs all-files access; grant it without the UI.
dev shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true

if [ "$MODE" = "gltf" ]; then
  echo "pushing sponza (~51MB)..."
  dev shell mkdir -p /sdcard/recreation/sponza >/dev/null 2>&1 || true
  dev push "$REPO/assets/sponza/." /sdcard/recreation/sponza/ >/dev/null
  printf 'gltf=/sdcard/recreation/sponza/Sponza.gltf\npreset=android\nvalidation=1\nno_rt=1\nscreenshot=6\n' > /tmp/recreation.cfg
else
  printf 'demo=materials\npreset=android\nvalidation=1\nno_rt=1\nscreenshot=6\n' > /tmp/recreation.cfg
fi

dev shell am force-stop "$PKG"
dev shell "run-as $PKG sh -c 'cat > files/recreation.cfg'" < /tmp/recreation.cfg
dev shell "run-as $PKG rm -f files/frame.png" >/dev/null 2>&1 || true
dev logcat -c
dev shell am start -n "$PKG/.MainActivity" --ez launch true >/dev/null 2>&1
echo "rendering..."; sleep 12

dev shell "run-as $PKG cat files/frame.png" > "$SHOT_DIR/frame_$MODE.png" 2>/dev/null || true
echo "frame: $(stat -c%s "$SHOT_DIR/frame_$MODE.png" 2>/dev/null || echo 0) bytes -> $SHOT_DIR/frame_$MODE.png"
echo "=== engine log ==="
dev logcat -d 2>/dev/null | grep "recreation:" | sed 's/.*recreation: //'
echo "=== validation errors: $(dev logcat -d 2>/dev/null | grep "recreation:" | grep -ciE '\[error\]|VK_ERROR') ==="
