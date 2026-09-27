#!/bin/bash
# The tracked unit dies with a build order armed — one scripted run, no hand driving.
#
#   tools/b6-tracked-death.sh [out-dir]     (instance $B6_INSTANCE, default b6td, on the
#                                            private Xvfb $B6_DISPLAY, default :142)
#
# Launch scenarios/b6-tracked-death.json with the play defaults (the build ghost is one) ->
# select the CORCK -> press CORSOLAR (main+0x2CC3 = 0x0E) -> order it into the ARM LLT's range
# with `tacli order`, not the mouse -> wait for it to die -> read the order byte 0x2CC3, the
# BuildUnitID 0x2CC4 and the tracked unit 0x37E9C, the top GUI and the ghost's counters ->
# left-click the map with nothing selected -> left-click the CORAK -> left-click the map
# again -> read the CORAK's first order node's type -> stop. A window picture at each stage.
# Prints the wall time it measured. The engine map's "The tracked unit's death leaves the
# placement armed" has what one run showed.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${B6_INSTANCE:-b6td}
D=${B6_DISPLAY:-:142}
OUT=${1:-${TMPDIR:-/tmp}/b6-tracked-death}
mkdir -p "$OUT"
start=$(date +%s)
echo "b6 start $(date -d @"$start" +%T)"
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "b6 wall $(( $(date +%s) - start )) s"; exit 1; }

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
LOG="$(info gamedir)/log/tagpu.log"

val() { "$T" peek "$I" "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }
state() {
  printf '%-14s order_byte=0x%02X build_id=%s tracked=%s tick=%s\n' "$1" \
    "$(val '*0x511DE8+0x2CC3:1')" "$(val '*0x511DE8+0x2CC4:2')" "$(val '*0x511DE8+0x37E9C:2')" \
    "$(val '*0x511DE8+0x38A47:4')"
}
ghost() {   # the heartbeat's next ghost line
  local n0; n0=$(grep -c '^ghost: curs' "$LOG")
  for _ in $(seq 1 40); do [ "$(grep -c '^ghost: curs' "$LOG")" -gt "$n0" ] && break; sleep 0.5; done
  echo "  $(grep '^ghost: curs' "$LOG" | tail -1 | cut -d' ' -f1-5)"
}
cap() { DISPLAY="$D" import -window "$(info window)" -depth 8 "$OUT/$1.png" && echo "  picture $OUT/$1.png"; }
unit() {    # roster type -> "idx sx sy"
  local l
  for _ in $(seq 1 30); do
    l=$("$T" roster "$I" 2>/dev/null | awk -v t="$1" '$2==t {print; exit}')
    [ -n "$l" ] && { echo "$l" | sed -E 's/.*idx=([0-9]+).*screen=\[(-?[0-9]+), (-?[0-9]+)\].*/\1 \2 \3/'; return; }
    sleep 0.5
  done
}

read -r CK CKX CKY <<< "$(unit corck)"; [ -n "${CK:-}" ] || fail "no corck in the roster"
read -r AK AKX AKY <<< "$(unit corak)"; [ -n "${AK:-}" ] || fail "no corak in the roster"
echo "corck idx=$CK at $CKX,$CKY; corak idx=$AK at $AKX,$AKY"
state "before"

"$T" click "$I" "$CKX" "$CKY" > /dev/null
up=0
for _ in $(seq 1 10); do "$T" ui "$I" 2>/dev/null | head -1 | grep -q CORCK1.GUI && { up=1; break; }; sleep 0.5; done
[ "$up" = 1 ] || fail "the CORCK's build menu is not up: $("$T" ui "$I" 2>&1 | head -1)"
"$T" ui "$I" click CORSOLAR > /dev/null || fail "CORSOLAR"
"$T" keys "$I" mouse:600,560 > /dev/null
sleep 1
state "armed"
ghost
cap 1-armed

"$T" order "$I" --unit "$CK" --expect CORCK move pos 1100 7700 > /dev/null || fail "move order"
dead=0
for _ in $(seq 1 120); do
  "$T" roster "$I" 2>/dev/null | awk '$2=="corck"' | grep -q . || { dead=1; break; }
  sleep 0.5
done
[ "$dead" = 1 ] || fail "the CORCK did not die within 60 s"
echo "corck dead at +$(( $(date +%s) - start )) s"
sleep 1
state "after death"
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
ghost
cap 2-dead

"$T" keys "$I" mouse:600,560 > /dev/null
"$T" click "$I" 600 560 > /dev/null
sleep 1
state "map clicked"
ghost
"$T" roster "$I" 2>/dev/null | sed 's/^/  /'

"$T" click "$I" "$AKX" "$AKY" > /dev/null
sleep 1
state "corak clicked"
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
cap 3-corak
"$T" keys "$I" mouse:600,560 > /dev/null
"$T" click "$I" 600 560 > /dev/null
sleep 2
state "map clicked"
ghost
BASE=$(val '*0x511DE8+0x14357:4')
H=$(val "$(printf '0x%X' $(( BASE + AK * 0x118 + 0x5C ))):4")
if [ "${H:-0}" != 0 ]; then
  echo "  corak first order: node $(printf '0x%X' "$H") type=$(val "$(printf '0x%X' $((H + 4))):1")"
else
  echo "  corak first order: none"
fi
"$T" roster "$I" 2>/dev/null | sed 's/^/  /'
cap 4-after-click

"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
end=$(date +%s)
echo "b6 wall $((end - start)) s ($(date -d @"$start" +%T) .. $(date -d @"$end" +%T))"
