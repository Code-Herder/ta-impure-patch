#!/bin/bash
# A command mode whose unit dies — one scripted run, no hand driving.
#
#   tools/b9-command-mode.sh [out-dir]  (instance $B9_INSTANCE, default b9cm, on the private
#                                        Xvfb $B9_DISPLAY, default :146; B9_MODE=attack or move,
#                                        default attack; B9_DLL=stocklimits runs
#                                        ddraw-stocklimits.dll)
#
# scenarios/b9-group.json with the play defaults. Select the CORAK alone, press CORATTACK or
# CORMOVE (the order byte main+0x2CC3 takes the mode, 3 or 2), walk the CORAK into the LLT's
# range with `tacli order`, then left-click a CORCK once. Reads the byte, the tracked unit and the
# CORCK's selection bit at each step, with a window picture. B9's verdicts: once the CORAK is dead
# the byte is 1, and the click selects the CORCK. Stock keeps the mode and swallows the click.
# Prints the wall time it measured.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${B9_INSTANCE:-b9cm}
D=${B9_DISPLAY:-:146}
MODE=${B9_MODE:-attack}
case "$MODE" in
  attack) GADGET=CORATTACK; WANT=3 ;;
  move)   GADGET=CORMOVE;   WANT=2 ;;
  *) echo "B9_MODE is attack or move"; exit 2 ;;
esac
OUT=${1:-${TMPDIR:-/tmp}/b9-command-mode-$MODE}
mkdir -p "$OUT"
start=$(date +%s)
echo "b9 $MODE start $(date -d @"$start" +%T)"
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "b9 $MODE wall $(( $(date +%s) - start )) s"; exit 1; }

if ! xdpyinfo -display "$D" > /dev/null 2>&1; then
  Xvfb "$D" -screen 0 1280x1024x24 -nolisten tcp > /dev/null 2>&1 &
  XPID=$!
  for _ in $(seq 1 20); do xdpyinfo -display "$D" > /dev/null 2>&1 && break; sleep 0.5; done
fi
"$T" create "$I" --display "$D" > "$OUT/create.txt" 2>&1 || grep -q 'already exists' "$OUT/create.txt" \
  || fail "create: $(tail -1 "$OUT/create.txt")"
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
KEEP=()
if [ "${B9_DLL:-}" = stocklimits ]; then
  G=$(info gamedir); [ -n "$G" ] || fail "no gamedir"
  cp "$REPO/tagpu/ddraw/ddraw-stocklimits.dll" "$G/ddraw.dll" || fail "stock-limits dll"
  KEEP=(--keep-dll)
fi
"$T" scenario load "$I" "$REPO/scenarios/b9-group.json" --defaults --los 0 --restart \
  "${KEEP[@]}" > "$OUT/load.txt" 2>&1 || fail "load: $(tail -2 "$OUT/load.txt")"
LOG="$(info gamedir)/log/tagpu.log"
grep -m1 -o "an order mode disarmed with nobody to order ([0-9xA-F ]*) [A-Z]*" "$LOG" \
  | sed 's/^/  enginefix: /'
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
"$T" ui "$I" click "$GADGET" > /dev/null || fail "$GADGET: $("$T" ui "$I" 2>&1 | head -3)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
state "$MODE armed"
ARMED=$(val "$BYTE")
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
TR=$(val '*0x511DE8+0x37E9C:2'); SEL=$(selected "$CK")
echo "  top gui: $("$T" ui "$I" 2>/dev/null | head -1)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
cap 3-clicked

verdict() { if [ "$2" = "$3" ]; then echo "PASS  $1 ($2)"; else echo "FAIL  $1 (read $2, want $3)"; fi; }
verdict "$GADGET arms the mode" "$ARMED" "$WANT"
verdict "the mode disarmed once the CORAK is dead" "$DEAD" 1
verdict "the next left click tracks the CORCK" "$TR" "$CK"
verdict "and selects it" "$SEL" 1

"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
end=$(date +%s)
echo "b9 $MODE wall $((end - start)) s ($(date -d @"$start" +%T) .. $(date -d @"$end" +%T))"
