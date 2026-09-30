#!/bin/bash
# A build site queued over another queued site turns the build cursor red — one scripted run.
#
#   tools/queued-site-check.sh [out-dir]   (instance $QS_INSTANCE, default qsite, on the
#                                            private Xvfb $QS_DISPLAY, default :144)
#
# scenarios/queued-site.json: two CORCK (A above B) on open ground. Every step arms CORSOLAR
# (5x5 cells) and reads main+0x2CC6 bit 6, the cursor's green (1) or red (0), with the pointer on
# a site that overlaps or does not overlap one already queued. Seven checks (step 2 only places).
#   1  clear ground, no queue                                  green   (the baseline)
#   2  Shift held, A places a site at P (mode stays armed)
#   3  Shift held, A hovers 40 px from P (overlap)             red     (its own queued site)
#   4  Shift held, A hovers 140 px from P (clear)              green
#   5  Shift held, A hovers 8 px from P (Shift-click cancels it) green  (the toggle is kept)
#   6  no Shift, A hovers 40 px from P (its order is replaced) green
#   7  no Shift, B selected, hovers 40 px from P               red     (A's plan, A not selected)
#   8  no Shift, B selected, hovers 140 px from P              green
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
T="$REPO/tools/tacli"
I=${QS_INSTANCE:-qsite}
D=${QS_DISPLAY:-:144}
OUT=${1:-${TMPDIR:-/tmp}/queued-site}
mkdir -p "$OUT"
start=$(date +%s)
XPID=
fail() { echo "FAIL: $*"; "$T" stop "$I" > /dev/null 2>&1; [ -n "$XPID" ] && kill "$XPID"
         echo "queued-site wall $(( $(date +%s) - start )) s"; exit 1; }
if ! xdpyinfo -display "$D" > /dev/null 2>&1; then
  Xvfb "$D" -screen 0 1280x1024x24 -nolisten tcp > /dev/null 2>&1 &
  XPID=$!
  for _ in $(seq 1 20); do xdpyinfo -display "$D" > /dev/null 2>&1 && break; sleep 0.5; done
fi
"$T" create "$I" --display "$D" > "$OUT/create.txt" 2>&1 || grep -q 'already exists' "$OUT/create.txt" \
  || fail "create: $(tail -1 "$OUT/create.txt")"
"$T" scenario load "$I" "$REPO/scenarios/queued-site.json" --defaults --los 0 --restart \
  > "$OUT/load.txt" 2>&1 || fail "load: $(tail -2 "$OUT/load.txt")"
sleep 4         # the roster and the menus are empty for a moment after the load

val() { "$T" peek "$I" "$1" 2>/dev/null | tail -1 | awk '{print $3}'; }
green() { local v; v=$(val '*0x511DE8+0x2CC6:1'); echo $(( (v >> 6) & 1 )); }
unit() {        # nth CORCK of the roster: idx x y
  local l
  for _ in $(seq 1 30); do
    l=$("$T" roster "$I" 2>/dev/null | awk -v n="$1" '$2=="corck" && $3=="own=0" {if (++k==n) {print; exit}}')
    [ -n "$l" ] && { echo "$l" | sed -E 's/.*idx=([0-9]+).*screen=\[(-?[0-9]+), (-?[0-9]+)\].*/\1 \2 \3/'; return; }
    sleep 0.5
  done
}
select_and_arm() {      # $1 x $2 y: click the builder, wait for its menu, press CORSOLAR
  "$T" click "$I" "$1" "$2" > /dev/null
  local up=0
  for _ in $(seq 1 20); do "$T" ui "$I" 2>/dev/null | head -1 | grep -q '^gui CORCK1.GUI' && { up=1; break; }; sleep 0.5; done
  [ "$up" = 1 ] || fail "the CORCK's build menu is not up: $("$T" ui "$I" 2>&1 | head -1)"
  sleep 1
  "$T" ui "$I" click CORSOLAR > /dev/null || fail "CORSOLAR"
}
hover() { "$T" keys "$I" "mouse:$1,$2" > /dev/null; sleep 1; }
results=()
check() {       # name got want
  if [ "$2" = "$3" ]; then results+=("PASS  $1"); else results+=("FAIL  $1 (cursor green=$2, want $3)"); fi
  echo "  ${results[-1]}"
}

read -r AI AX AY <<< "$(unit 1)"; read -r BI BX BY <<< "$(unit 2)"
[ -n "${AI:-}" ] && [ -n "${BI:-}" ] || fail "two corcks are not in the roster"
echo "A idx=$AI at $AX,$AY   B idx=$BI at $BX,$BY"
# a site up and to the right of both builders, on open ground
PX=$((AX + 390)); PY=$((AY - 130))

select_and_arm "$AX" "$AY"
hover "$PX" "$PY";                 check "1 clear ground, no queue is green" "$(green)" 1
"$T" keys "$I" down:shift > /dev/null; sleep 0.5
hover "$PX" "$PY"
"$T" click "$I" "$PX" "$PY" > /dev/null; sleep 1
hover $((PX + 40)) "$PY";          check "3 Shift, over A's own queued site: red" "$(green)" 0
hover $((PX + 140)) "$PY";         check "4 Shift, clear of it: green" "$(green)" 1
hover $((PX + 8)) "$PY";           check "5 Shift, on the site (the click cancels it): green" "$(green)" 1
"$T" keys "$I" up:shift > /dev/null; sleep 1
"$T" ui "$I" click CORSOLAR > /dev/null || fail "CORSOLAR again"     # A is still selected
hover $((PX + 40)) "$PY";          check "6 no Shift, A's order is replaced: green" "$(green)" 1
"$T" click "$I" --right 760 560 > /dev/null; sleep 0.5
select_and_arm "$BX" "$BY"
hover $((PX + 40)) "$PY";          check "7 no Shift, over A's queued site with B selected: red" "$(green)" 0
hover $((PX + 140)) "$PY";         check "8 no Shift, B selected, clear of it: green" "$(green)" 1

printf '%s\n' "${results[@]}"
"$T" stop "$I" > /dev/null
[ -n "$XPID" ] && kill "$XPID"
echo "queued-site wall $(( $(date +%s) - start )) s"
printf '%s\n' "${results[@]}" | grep -q FAIL && exit 1 || exit 0
