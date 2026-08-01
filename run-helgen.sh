#!/usr/bin/env bash
# Skyrim's opening: the prisoner cart ride down the road into Helgen.
# RX_DISTANT_LOD keeps the valley and the town visible from up the mountain.
source "$(dirname "${BASH_SOURCE[0]}")/run-common.sh"
export RX_HELGEN_INTRO=1
export RX_DISTANT_LOD="${RX_DISTANT_LOD:-1}"
# Leave the world clock alone: a low sun over this snowfield turns the whole
# ride into specular sparkle (RX_GAME_HOUR=7.5 reproduces it with no cutscene).
launch --data-dir "$SKYRIM_DATA" "$@"
