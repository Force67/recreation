#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/run-common.sh"
launch --data-dir "$FO3_DATA" "$@"
