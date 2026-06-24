# Shared helpers for the recreation setup scripts. Source this, do not run it.
# shellcheck shell=bash

set -euo pipefail

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'; C_BLU=$'\033[34m'; C_RST=$'\033[0m'
else
  C_RED=; C_GRN=; C_YEL=; C_BLU=; C_RST=
fi

log()  { printf '%s==>%s %s\n' "$C_BLU" "$C_RST" "$*"; }
ok()   { printf '  %sok%s   %s\n' "$C_GRN" "$C_RST" "$*"; }
warn() { printf '  %swarn%s %s\n' "$C_YEL" "$C_RST" "$*" >&2; }
err()  { printf '  %sfail%s %s\n' "$C_RED" "$C_RST" "$*" >&2; }
die()  { err "$*"; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# Empty when already root, otherwise sudo. Lets the same script serve a CI
# container (root) and a developer laptop.
if [ "$(id -u 2>/dev/null || echo 0)" = "0" ]; then SUDO=; else SUDO="sudo"; fi

# Non-interactive under CI or when ASSUME_YES/-y is set.
ASSUME_YES="${ASSUME_YES:-0}"
[ -n "${CI:-}" ] && ASSUME_YES=1

confirm() {
  [ "$ASSUME_YES" = "1" ] && return 0
  printf '  %s?%s    %s [Y/n] ' "$C_YEL" "$C_RST" "$*"
  local a; read -r a </dev/tty 2>/dev/null || a=y
  case "$a" in n|N|no|NO) return 1 ;; *) return 0 ;; esac
}

# recreation/ root, regardless of the caller's working directory.
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Anything that still blocks a build lands here and is summarised at the end so
# a developer sees the whole picture in one place instead of one error at a time.
_BLOCKERS=()
blocker() { _BLOCKERS+=("$1"); err "$1"; }

print_report() {
  echo
  if [ "${#_BLOCKERS[@]}" -eq 0 ]; then
    log "${C_GRN}ready${C_RST} - configure with: cmake --preset <linux|macos|windows> (or see README)"
    return 0
  fi
  err "${#_BLOCKERS[@]} item(s) still block a build:"
  local m
  for m in "${_BLOCKERS[@]}"; do printf '     %s- %s%s\n' "$C_YEL" "$m" "$C_RST"; done
  return 1
}

# tool <command> <human name> <mitigation hint>: record a blocker if missing.
check_tool() {
  if have "$1"; then ok "$2 ($(command -v "$1"))"; else blocker "$2 missing - $3"; fi
}
