#!/bin/bash
# The build placement's own ends, unchanged by B9 — one scripted run, no hand driving.
#
#   tools/b9-placement-paths.sh [out-dir]   (instance $B9_INSTANCE, default b9pp, on the
#                                            private Xvfb $B9_DISPLAY, default :143)
#
# scenarios/b6-tracked-death.json with the play defaults; the CORCK stays out of the LLT's reach.
# 1. Select the CORCK, press CORSOLAR, right-click the map: the right button cancels
#    (0x499100), the byte returns to 1, and the CORCK is still the tracked unit.
# 2. Select the CORCK again, press CORSOLAR, left-click a clear site: the byte returns to 1 and
#    a CORSOLAR is placed (it appears in the roster). The cancel goes first because the build
#    order walks the CORCK away from the screen position the roster gave for selecting it.
# Ends with a verdict per step and the wall time it measured.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${B9_INSTANCE:-b9pp}
D=${B9_DISPLAY:-:143}
OUT=${1:-${TMPDIR:-/tmp}/b9-placement-paths}
mkdir -p "$OUT"
start=$(date +%s)
echo "b9 paths start $(date -d @"$start" +%T)"
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "b9 paths wall $(( $(date +%s) - start )) s"; exit 1; }

if ! xdpyinfo -display "$D" > /dev/null 2>&1; then
  Xvfb "$D" -screen 0 1280x1024x24 -nolisten tcp > /dev/null 2>&1 &
  XPID=$!
  for _ in $(seq 1 20); do xdpyinfo -display "$D" > /dev/null 2>&1 && break; sleep 0.5; done
fi
"$T" ls --json | python3 -c "import json,sys; sys.exit(not any(r['name']=='$I' for r in json.load(sys.stdin)))" \
  || "$T" create "$I" --display "$D" > /dev/null || fail create
"$T" scenario load "$I" "$REPO/scenarios/b6-tracked-death.json" --defaults --los 0 --restart \
  > "$OUT/load.txt" 2>&1 || fail "load: $(tail -2 "$OUT/load.txt")"
info() { "$T" ls --json | python3 -c "
import json,sys
for r in json.load(sys.stdin):
    if r['name']=='$I': print(r['$1'] if '$1'!='window' else r['window'][0])"; }
val() { "$T" peek "$I" "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }
state() {
  printf '%-16s order_byte=0x%02X build_id=%s tracked=%s\n' "$1" \
    "$(val '*0x511DE8+0x2CC3:1')" "$(val '*0x511DE8+0x2CC4:2')" "$(val '*0x511DE8+0x37E9C:2')"
}
cap() { DISPLAY="$D" import -window "$(info window)" -depth 8 "$OUT/$1.png" && echo "  picture $OUT/$1.png"; }
unit() {
  local l
  for _ in $(seq 1 30); do
    l=$("$T" roster "$I" 2>/dev/null | awk -v t="$1" '$2==t {print; exit}')
    [ -n "$l" ] && { echo "$l" | sed -E 's/.*idx=([0-9]+).*screen=\[(-?[0-9]+), (-?[0-9]+)\].*/\1 \2 \3/'; return; }
    sleep 0.5
  done
}
arm() {     # select the CORCK and press CORSOLAR
  local up=0
  "$T" click "$I" "$CKX" "$CKY" > /dev/null
  for _ in $(seq 1 10); do "$T" ui "$I" 2>/dev/null | head -1 | grep -q CORCK1.GUI && { up=1; break; }; sleep 0.5; done
  [ "$up" = 1 ] || fail "the CORCK's build menu is not up: $("$T" ui "$I" 2>&1 | head -1)"
  "$T" ui "$I" click CORSOLAR > /dev/null || fail "CORSOLAR"
}

read -r CK CKX CKY <<< "$(unit corck)"; [ -n "${CK:-}" ] || fail "no corck in the roster"
echo "corck idx=$CK at $CKX,$CKY"

# 1. the right-button cancel
arm
"$T" keys "$I" mouse:760,560 > /dev/null
sleep 1
state "armed"
ARMED1=$(val '*0x511DE8+0x2CC3:1')
"$T" click "$I" --right 760 560 > /dev/null
sleep 1
state "right-clicked"
CANCEL_BYTE=$(val '*0x511DE8+0x2CC3:1'); CANCEL_TRACKED=$(val '*0x511DE8+0x37E9C:2')
cap 2-cancelled

# 2. a normal placement on clear grass, up and to the right of the CORCK
arm
"$T" keys "$I" mouse:760,200 > /dev/null
sleep 1
state "armed again"
ARMED2=$(val '*0x511DE8+0x2CC3:1')
cap 3-armed-again
"$T" click "$I" 760 200 > /dev/null
sleep 1
state "placed"
PLACED_BYTE=$(val '*0x511DE8+0x2CC3:1')
SOLAR=
for _ in $(seq 1 40); do
  SOLAR=$("$T" roster "$I" 2>/dev/null | awk '$2=="corsolar" && $3=="own=0"' | head -1)
  [ -n "$SOLAR" ] && break
  sleep 0.5
done
echo "  roster: ${SOLAR:-no own corsolar}"
cap 4-placed

verdict() { if [ "$2" = "$3" ]; then echo "PASS  $1 ($2)"; else echo "FAIL  $1 (read $2, want $3)"; fi; }
verdict "a build button arms the placement" "$ARMED1" 14
verdict "a right click cancels it" "$CANCEL_BYTE" 1
verdict "the CORCK is still the tracked unit after the cancel" "$CANCEL_TRACKED" "$CK"
verdict "a build button arms it again" "$ARMED2" 14
verdict "a left click on a clear site ends it" "$PLACED_BYTE" 1
verdict "that click placed a CORSOLAR" "$([ -n "$SOLAR" ] && echo yes || echo no)" yes

"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
end=$(date +%s)
echo "b9 paths wall $((end - start)) s ($(date -d @"$start" +%T) .. $(date -d @"$end" +%T))"
