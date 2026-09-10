#!/bin/bash
# One leg of the health-bar wobble A/B (scenarios/bar-wobble.json, roadmap "Awaiting review").
#
#   tools/barwobble.sh <instance> <tag> <startX> <startY> <endX> <endY> <secs> <zoom> <outdir>
#
# The canonical legs, all at 1920x1080 with the shield ON (this is a measurement):
#   barwobble.sh <i> stock  1958 1296 2658 696 22 1.0 /tmp/bw   # a BARE instance = stock control
#   barwobble.sh <i> ours   1958 1296 2658 696 22 1.0 /tmp/bw   # --defaults, camera at 1818,850
#   barwobble.sh <i> zoom2  2100 1150 2500  800 13 2.0 /tmp/bw   # camera at 2300,975 in the fixture
#   FIX=scenarios/bar-wobble-4x.json \
#     barwobble.sh <i> zoom4 2218 1086 2398 906 10 4.0 /tmp/bw   # the OWN fixture, camera 2244,996
# Then: tools/barwobble_detect.py /tmp/bw
#
# RUN THE 4x LEG. A defect that quantises the bar in PRE-zoom units is worth `zoom` displayed
# pixels, so it is at the measurement floor at 1x and 4 px at 4x -- the 1x legs above passed it
# clean while the owner was watching the bar teleport across the screen. The zoom is taken from
# the TAG (`zoom4`, `4x`), and every length in the detector scales with it, so a mislabelled leg
# is a wrong answer and not a noisy one.
#
# THE FIXTURE OWNS THE CAMERA and `camera.at` is not the eye: the applier subtracts (832, 550).
# The 1x walk is 920 wu and leaves the viewport entirely at 4x, which is why the 4x leg has a
# fixture of its own rather than an argument.
#
# READ gamespeed FIRST. It is shared across every prefix (one user.reg inode) and it scales
# how far a unit moves per sim step, so it scales this artifact: the same walk measures
# 1.68 px peak-to-peak at gamespeed 10 and 2.95 at 20. See the ta-drive skill.
#
# Respawns the walker at the start point with the fixture, selects it, parks the pointer
# clear of the walk, then records the window losslessly at 60 fps while the unit walks a
# straight diagonal to the end point.  No scheduling-pressure rig here on purpose: this
# measures SMOOTHNESS, and pinning the game to one core would manufacture the very stutter
# under test.
set -u
INST="${1:?}"; TAG="${2:?}"; SX="${3:?}"; SY="${4:?}"; EX="${5:?}"; EY="${6:?}"
SECS="${7:?}"; ZOOM="${8:?}"; OUT="${9:?}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"

TACLI="$REPO/tools/tacli"
FIX="${FIX:-$REPO/scenarios/bar-wobble.json}"
mkdir -p "$OUT"

read -r WID PID GD DISP < <("$TACLI" ls --json | python3 -c "
import json,sys
i=[i for i in json.load(sys.stdin) if i['name']=='$INST']
if not i: sys.exit('no such instance: $INST')
i=i[0]
if not i.get('running') or not i.get('window'): sys.exit('$INST is not running')
print(i['window'][0], i['pid'], i['gamedir'], i['display'])")
[ -n "${WID:-}" ] || { echo "  ABORT: $INST is not running"; exit 1; }
WIDHEX=$(printf "0x%x" "$WID")   # ffmpeg x11grab parses the decimal form wrong: it read
                                 # a 1920x1080 window as "screen size 1080x1080" and refused

# The walker respawns at the start point; the camera stays pinned by the fixture. Written to
# a TEMP copy -- the shipped fixture is tracked and a harness must not dirty it.
RUNFIX="$(mktemp -t bar-wobble-XXXXXX.json)"
trap 'rm -f "$RUNFIX"' EXIT
python3 - "$FIX" "$RUNFIX" "$SX" "$SY" <<'EOF'
import json,sys
d=json.load(open(sys.argv[1]))
d["units"][0]["pos"]=[int(sys.argv[3]),int(sys.argv[4])]
json.dump(d,open(sys.argv[2],"w"),indent=2)
EOF
"$TACLI" scenario apply "$INST" "$RUNFIX" >/dev/null 2>&1

# zoom: the FILE wins over the wheel, and it must be written atomically
# ALWAYS write the file, 1.0 included: DELETING it hands the level over to the wheel at
# whatever it currently is, it does not reset -- so a "zoom 1" leg after a 2x leg silently
# stays at 2x and every screen-space calculation here is then wrong.
printf '%s' "$ZOOM" > "$GD/.zoom.tmp" && mv -f "$GD/.zoom.tmp" "$GD/tagpu_zoom.txt"
# The roster is written every 30 PRESENTED frames and `tacli roster` parses the newest
# block, so right after an apply it still reports where the unit was BEFORE the respawn --
# a fixed sleep read the previous leg's end position and the selection click missed.  Wait
# for the roster to actually agree with the spawn point.
for _ in $(seq 1 30); do
    sleep 1
    OKPOS=$("$TACLI" roster "$INST" --json | python3 -c "
import sys,json
d=json.load(sys.stdin)
u=[u for u in d['units'] if u['type']=='ARMCOM']
print(1 if u and abs(u[0]['world'][0]-$SX)<=8 and abs(u[0]['world'][1]-$SY)<=8 else 0)" 2>/dev/null)
    [ "${OKPOS:-0}" = "1" ] && break
done
[ "${OKPOS:-0}" = "1" ] || { echo "  ABORT: the walker never appeared at $SX,$SY"; exit 1; }

# select the walker where the roster says it is, at the CURRENT zoom (roster screen= is
# the 1x projection, so bend it through the zoom about the screen centre ourselves)
read -r CX CY < <("$TACLI" roster "$INST" --json | python3 -c "
import sys,json
d=json.load(sys.stdin); z=float('$ZOOM')
u=[u for u in d['units'] if u['type']=='ARMCOM'][0]
x,y=u['screen']; print(int(round(1024+(x-1024)*z)), int(round(540+(y-540)*z)))")
"$TACLI" click "$INST" "$CX" "$CY" >/dev/null 2>&1
sleep 1
"$TACLI" keys "$INST" mouse:250,1010 >/dev/null 2>&1   # cursor sprite off the walk line
sleep 1
SEL=$(grep -a 'native: ' "$GD/tagpu.log" | tail -1 | grep -o '[0-9]* sel' | cut -d' ' -f1)
echo "  selected: ${SEL:-n/a} (clicked $CX,$CY at zoom $ZOOM)"

OFF=$(stat -c %s "$GD/tagpu.log")

ffmpeg -y -loglevel error -f x11grab -window_id "$WIDHEX" -draw_mouse 0 -framerate 60 \
  -video_size 1920x1080 -i "$DISP" -c:v libx264rgb -preset ultrafast -qp 0 \
  -t "$SECS" "$OUT/walk-$TAG.mkv" &
FF=$!
sleep 0.5
"$TACLI" order "$INST" --unit 1 --expect ARMCOM move pos "$EX" "$EY" >/dev/null 2>&1
wait $FF

tail -c +$((OFF + 1)) "$GD/tagpu.log" | tr -d '\000' > "$OUT/log-$TAG.txt"
echo "  $TAG: $(du -h "$OUT/walk-$TAG.mkv" | cut -f1) video, $(grep -ac 'spx:' "$OUT/log-$TAG.txt") spx samples"
grep -a 'native: ' "$OUT/log-$TAG.txt" | tail -1 | sed 's/^/    /'
