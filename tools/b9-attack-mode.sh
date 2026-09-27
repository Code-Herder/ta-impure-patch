#!/bin/bash
# A command mode outliving its unit (a measurement, not a fix) — one scripted run, no hand driving.
#
#   tools/b9-attack-mode.sh [out-dir]   (instance $B9_INSTANCE, default b9am, on the private
#                                        Xvfb $B9_DISPLAY, default :146)
#
# scenarios/b9-group.json with the play defaults. Select the CORAK alone, press CORATTACK (the order
# byte main+0x2CC3 takes a command mode, 2..0xD), walk the CORAK into the LLT's range with
# `tacli order`, then left-click a CORCK once. Reads the byte, the tracked unit and the CORCK's
# selection bit at each step, with a window picture: whether the mode outlives its unit, whether
# that click is swallowed, and what stays drawn. Prints the wall time it measured.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${B9_INSTANCE:-b9am}
D=${B9_DISPLAY:-:146}
OUT=${1:-${TMPDIR:-/tmp}/b9-attack-mode}
mkdir -p "$OUT"
start=$(date +%s)
echo "b9 attack start $(date -d @"$start" +%T)"
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "b9 attack wall $(( $(date +%s) - start )) s"; exit 1; }

if ! xdpyinfo -display "$D" > /dev/null 2>&1; then
  Xvfb "$D" -screen 0 1280x1024x24 -nolisten tcp > /dev/null 2>&1 &
  XPID=$!
  for _ in $(seq 1 20); do xdpyinfo -display "$D" > /dev/null 2>&1 && break; sleep 0.5; done
fi
"$T" create "$I" --display "$D" > "$OUT/create.txt" 2>&1 || grep -q 'already exists' "$OUT/create.txt" \
  || fail "create: $(tail -1 "$OUT/create.txt")"
"$T" scenario load "$I" "$REPO/scenarios/b9-group.json" --defaults --los 0 --restart \
  > "$OUT/load.txt" 2>&1 || fail "load: $(tail -2 "$OUT/load.txt")"
info() {        # retried: `tacli ls` fails while another session's instance is half made
  local v
  for _ in $(seq 1 20); do
    v=$("$T" ls --json 2>/dev/null | python3 -c "
import json,sys
for r in json.load(sys.stdin):
    if r['name']=='$I': print(r['$1'] if '$1'!='window' else r['window'][0])" 2>/dev/null)
    [ -n "$v" ] && { echo "$v"; return; }
    sleep 0.5
  done
}
val() { "$T" peek "$I" "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }
BYTE='*0x511DE8+0x2CC3:1'
state() {
  printf '%-16s order_byte=0x%02X tracked=%s\n' "$1" "$(val "$BYTE")" "$(val '*0x511DE8+0x37E9C:2')"
}
cap() {         # never `import` without a window: it would wait for a click, grabbing the server
  local w; w=$(info window)
  [ -n "$w" ] || { echo "  picture $1: no window id"; return; }
  DISPLAY="$D" import -window "$w" -depth 8 "$OUT/$1.png" && echo "  picture $OUT/$1.png"
}
unit() {
  local l
  for _ in $(seq 1 30); do
    l=$("$T" roster "$I" 2>/dev/null | awk -v t="$1" '$2==t && $3=="own=0" {print; exit}')
    [ -n "$l" ] && { echo "$l" | sed -E 's/.*idx=([0-9]+).*screen=\[(-?[0-9]+), (-?[0-9]+)\].*/\1 \2 \3/'; return; }
    sleep 0.5
  done
}
selected() {
  local base; base=$(val '*0x511DE8+0x14357:4')
  echo $(( $(val "$(printf '0x%X' $(( base + $1 * 0x118 + 0x110 ))):1") >> 4 & 1 ))
}

read -r AK AKX AKY <<< "$(unit corak)"; [ -n "${AK:-}" ] || fail "no corak in the roster"
read -r CK CKX CKY <<< "$(unit corck)"; [ -n "${CK:-}" ] || fail "no corck in the roster"
echo "corak idx=$AK at $AKX,$AKY; corck idx=$CK at $CKX,$CKY"

"$T" click "$I" "$AKX" "$AKY" > /dev/null
sleep 1
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
"$T" ui "$I" click CORATTACK > /dev/null || fail "CORATTACK: $("$T" ui "$I" 2>&1 | head -3)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
state "attack armed"
MODE=$(val "$BYTE")
cap 1-armed

"$T" order "$I" --unit "$AK" --expect CORAK move pos 1100 7700 > /dev/null || fail "move order"
for _ in $(seq 1 240); do
  "$T" roster "$I" 2>/dev/null | awk -v i="idx=$AK" '$2=="corak" && $4==i' | grep -q . || break
  sleep 0.5
done
"$T" roster "$I" 2>/dev/null | awk -v i="idx=$AK" '$2=="corak" && $4==i' | grep -q . && fail "the CORAK did not die"
echo "corak dead at +$(( $(date +%s) - start )) s"
sleep 1
state "corak dead"
DEAD=$(val "$BYTE")
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
cap 2-dead

"$T" click "$I" "$CKX" "$CKY" > /dev/null
sleep 1
state "corck clicked"
AFTER=$(val "$BYTE"); TR=$(val '*0x511DE8+0x37E9C:2'); SEL=$(selected "$CK")
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
cap 3-clicked

printf 'mode armed 0x%02X; after the death 0x%02X; after one left click on the CORCK 0x%02X, tracked %s, CORCK selected %s\n' \
  "$MODE" "$DEAD" "$AFTER" "$TR" "$SEL"
"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
end=$(date +%s)
echo "b9 attack wall $((end - start)) s ($(date -d @"$start" +%T) .. $(date -d @"$end" +%T))"
