#!/bin/bash
# A command mode whose unit dies under a modal screen — one scripted run, no hand driving.
#
#   tools/b9-under-chat.sh [out-dir]    (instance $B9_INSTANCE, default b9uc, on the private
#                                        Xvfb $B9_DISPLAY, default :148)
#
# scenarios/b9-group.json with the play defaults. Select the CORAK alone, press CORATTACK (the
# order byte main+0x2CC3 takes 3), open the chat with Enter (the key switch's case 0x4964FD pushes
# TALK.GUI through 0x494050 and sets main+0x37EBE bit 2 at 0x49412C; unlike the options stack it
# does not stop the ticks), walk the CORAK into the LLT's range with `tacli order`, and close the
# chat once the CORAK is dead. B9's verdicts: under the chat the byte stays 3 (the deferral: the
# cancel would look for STOP in TALK.GUI), after the close it is 1, and CORATTACK is not drawn
# pressed: either the engine has dropped the dead unit's menu (the drop it deferred under the
# chat) or the button's rectangle in the window picture is the one taken before it was pressed
# (the snapshot carries no pressed state for a radio button). Prints the wall time it measured.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${B9_INSTANCE:-b9uc}
D=${B9_DISPLAY:-:148}
OUT=${1:-${TMPDIR:-/tmp}/b9-under-chat}
mkdir -p "$OUT"
start=$(date +%s)
echo "b9 chat start $(date -d @"$start" +%T)"
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "b9 chat wall $(( $(date +%s) - start )) s"; exit 1; }

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
"$T" scenario load "$I" "$REPO/scenarios/b9-group.json" --defaults --los 0 --restart \
  > "$OUT/load.txt" 2>&1 || fail "load: $(tail -2 "$OUT/load.txt")"
LOG="$(info gamedir)/log/tagpu.log"
grep -m1 -o "an order mode disarmed with nobody to order ([0-9xA-F ]*) [A-Z]*" "$LOG" \
  | sed 's/^/  enginefix: /'
val() { "$T" peek "$I" "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }
BYTE='*0x511DE8+0x2CC3:1'
state() {
  printf '%-16s order_byte=0x%02X flags37EBE=0x%04X tracked=%s\n' "$1" "$(val "$BYTE")" \
    "$(val '*0x511DE8+0x37EBE:2')" "$(val '*0x511DE8+0x37E9C:2')"
}
top() { "$T" ui "$I" 2>/dev/null | head -1 | awk '{print $2}'; }
rect() {        # a gadget's rectangle as W x H + X + Y, from the snapshot of its screen
  "$T" ui "$I" --json 2>/dev/null | python3 -c "
import json, sys
def walk(o):
    if isinstance(o, dict):
        if o.get('name') == '$1': yield o
        for v in o.values(): yield from walk(v)
    elif isinstance(o, list):
        for v in o: yield from walk(v)
g = next(walk(json.load(sys.stdin)), None)
if g: x, y, w, h = g['rect']; print(f'{w}x{h}+{x}+{y}')"
}
differ() {      # pixels that differ inside $R between pictures $1 and $2, or "none" when either is missing
  [ -f "$OUT/$1.png" ] && [ -f "$OUT/$2.png" ] || { echo none; return; }
  convert "$OUT/$1.png" -crop "$R" +repage "$OUT/btn-$1.png"
  convert "$OUT/$2.png" -crop "$R" +repage "$OUT/btn-$2.png"
  compare -metric AE "$OUT/btn-$1.png" "$OUT/btn-$2.png" null: 2>&1 | awk '{print $1}'
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

read -r AK AKX AKY <<< "$(unit corak)"; [ -n "${AK:-}" ] || fail "no corak in the roster"
echo "corak idx=$AK at $AKX,$AKY"

"$T" click "$I" "$AKX" "$AKY" > /dev/null
sleep 1
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
R=$(rect CORATTACK); [ -n "$R" ] || fail "no CORATTACK on $(top)"
echo "  CORATTACK at $R"
cap 0-selected
"$T" ui "$I" click CORATTACK > /dev/null || fail "CORATTACK: $("$T" ui "$I" 2>&1 | head -3)"
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
state "attack armed"
ARMED=$(val "$BYTE")
echo "  top gui $(top)"
cap 1-armed

"$T" keys "$I" return > /dev/null
sleep 1
TOP1=$(top)
state "chat open"
echo "  top gui $TOP1"
cap 2-chat

"$T" order "$I" --unit "$AK" --expect CORAK move pos 1100 7700 > /dev/null || fail "move order"
for _ in $(seq 1 240); do
  "$T" roster "$I" 2>/dev/null | awk -v i="idx=$AK" '$2=="corak" && $4==i' | grep -q . || break
  sleep 0.5
done
"$T" roster "$I" 2>/dev/null | awk -v i="idx=$AK" '$2=="corak" && $4==i' | grep -q . && fail "the CORAK did not die"
echo "corak dead at +$(( $(date +%s) - start )) s"
sleep 2
state "dead, chat up"
UNDER=$(val "$BYTE"); TOP2=$(top)
echo "  top gui $TOP2"
cap 3-dead-under-chat

"$T" keys "$I" escape > /dev/null
sleep 1
if [ "$(top)" = TALK.GUI ]; then
  echo "  escape left TALK.GUI up; closing with Enter"
  "$T" keys "$I" return > /dev/null
  sleep 1
fi
"$T" keys "$I" mouse:600,300 > /dev/null
sleep 1
state "chat closed"
AFTER=$(val "$BYTE"); TOP3=$(top)
echo "  top gui $TOP3"
cap 4-closed
P_ARMED=$(differ 0-selected 1-armed)
if [ -n "$(rect CORATTACK)" ]; then P_CLOSED=$(differ 0-selected 4-closed)
else P_CLOSED="0 (no CORATTACK on $TOP3: the engine dropped the dead unit's menu)"; fi
echo "  CORATTACK pixels changed: armed vs before $P_ARMED, closed vs before $P_CLOSED"

verdict() { if [ "$2" = "$3" ]; then echo "PASS  $1 ($2)"; else echo "FAIL  $1 (read $2, want $3)"; fi; }
verdict "CORATTACK arms the mode" "$ARMED" 3
verdict "Enter puts the chat on top" "$TOP1" TALK.GUI
verdict "the chat is still on top after the death" "$TOP2" TALK.GUI
verdict "under the chat the mode waits (deferred)" "$UNDER" 3
verdict "the chat is closed" "$([ "$TOP3" != TALK.GUI ] && echo closed || echo "$TOP3")" closed
verdict "after the close the mode is disarmed" "$AFTER" 1
verdict "CORATTACK is drawn pressed while armed" "$([ "$P_ARMED" != none ] && [ "$P_ARMED" -gt 0 ] && echo pressed || echo "$P_ARMED px")" pressed
verdict "CORATTACK is not drawn pressed after the close" "${P_CLOSED%% *}" 0

"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
end=$(date +%s)
echo "b9 chat wall $((end - start)) s ($(date -d @"$start" +%T) .. $(date -d @"$end" +%T))"
