#!/bin/bash
# mp_leave.sh -- one peer of a live network game surrenders and returns to the main menu,
# through the game's own menus: Tab -> OPTIONS -> EXIT -> MAINMENU -> CHOICE1 (the dialog
# reads "Surrender this battle and return to main menu?").
#
#   ./mp_leave.sh <instance>
#
# The host leaving ends the game on every peer (each joiner drops to its own units and goes
# to ENDMSN); a joiner leaving leaves the others playing. Measured in
# research/notes/tadr-port/sim-fixes.md (B5, the departing host).
set -eu

[ $# -eq 1 ] || { echo "usage: mp_leave.sh <instance>" >&2; exit 2; }
INST="$1"
TACLI="$(cd "$(dirname "$0")" && pwd)/tacli"
ui() { "$TACLI" ui "$INST" "$@"; }

"$TACLI" keys "$INST" tab
ui wait --gui TABMENU --timeout 20
ui click OPTIONS --timeout 20
ui click EXIT --timeout 20
ui click MAINMENU --timeout 20
# The level tears down under this click, so the click's own read-back can find no UI;
# the main menu appearing is the check.
ui click CHOICE1 --timeout 30 || true
ui wait --gui MAINMENU --timeout 60
