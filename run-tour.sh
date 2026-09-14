#!/usr/bin/env bash
# Tour de Recreation: the guided demo, seven stops of what a mod can do to a
# live world, narrated on its own card.
#
# The tour is a gamemode, so it is also a tile on the front screen -- launching
# it there is the normal way in, and this is the shortcut that skips the menu.
# RX_MENU_MODE arms it the way clicking its tile would.
source "$(dirname "${BASH_SOURCE[0]}")/run-common.sh"

export RX_SKYRIM_DATA="${RX_SKYRIM_DATA:-$SKYRIM_DATA}"
# Without this the managed runtime never boots, and the tour IS managed code.
export RECREATION_SCRIPTING_DIR="${RECREATION_SCRIPTING_DIR:-$REPO/build/nix/sdk/managed}"
# The demo wants a clean screen: no legal card in the way, no debug overlays.
export RX_LEGAL="${RX_LEGAL:-0}"
export RX_HIDE_DEBUG_UI="${RX_HIDE_DEBUG_UI:-1}"
# The tour works on the world around the PLAYER, so it needs one. Launching from
# the front screen spawns one on its own; a bare --data-dir run does not, and the
# proximity queries then have no centre to search from and find nothing.
export RX_PLAYER="${RX_PLAYER:-1}"

# A start cell chosen for the demo rather than for debugging: open ground on a
# pine slope with a long view, room for the tour to build its ring, and enough
# standing around to clone. The engine default (5,-3, near Whiterun) drops the
# player against a wall with an NPC in their face, which is a fine place to test
# streaming and a poor place to open a demo.
launch --data-dir "$SKYRIM_DATA" --game-mode TourDeRecreation --cell 0,-8 "$@"
