#!/bin/bash
# Gate C — the pose race's flicker regression (research/notes/gpu-posing.md §4 step 7,
# gpu-status.md §2.9).  One capture of the walk fixture, under scheduling pressure,
# with the transient detector run over the world band.
#
#   tools/gatec.sh <instance> <tag> [outdir]
#
# It does NOT set the pose levers: arm them first and run it once per arm state, so the
# same fixture and the same pressure produce the control and the gate run.
#
#   tools/tacli arm g16c 'native.on=all wrecks' terr.on feat.on zoom.on vpwide.on
#   tools/tacli scenario load g16c walk-gatec --restart
#   tools/tacli eye g16c 1818 850 && printf 2.0 > <gamedir>/tagpu_zoom.txt
#   tools/gatec.sh g16c off                      # the CPU emitters and the guard
#   tools/tacli arm g16c posedraw.on             # then WAIT for two fresh native: lines
#   tools/gatec.sh g16c on                       # the posed program
#
# WHY THE PRESSURE RIG.  The race's window is microseconds wide and opens ~30 times a
# second, so an idle machine samples it essentially never — a minute of walking caught it
# zero times over four runs.  Pinning the game and three spinners to ONE core makes its
# render and game threads timeshare, which is what a loaded machine does to a player.  One
# core is used, nothing else is touched, and the pin is released at the end.
#
# READ THE CONTROL BEFORE BELIEVING THE GATE.  A clean run on both sides with no catches on
# either means the rig did not bite, not that the gate passed.  The control's `guard=` total
# is the evidence it did; `tagpu_posewatch.on` on a fifth run is the sharper one (an `err`
# near the model's own size is a buffer caught mid-rewrite).
set -u
INST="${1:?usage: gatec.sh <instance> <tag> [outdir]}"
TAG="${2:?usage: gatec.sh <instance> <tag> [outdir]}"
OUT="${3:-/tmp/gatec}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
TACLI="$REPO/tools/tacli"
mkdir -p "$OUT"

read -r WID PID GD DISP < <("$TACLI" ls --json | python3 -c "
import json,sys
i = [i for i in json.load(sys.stdin) if i['name'] == '$INST']
if not i: sys.exit('no such instance: $INST')
i = i[0]
print(i['window'][0], i['pid'], i['gamedir'], i['display'])")
NCPU=$(nproc)

# The walk band, in world coordinates.  Both ends are chosen so the model stays inside the
# detector's band at 2x on 1920x1080: screen_y(1x) = wz - 860, and the 2x window is
# screen_y(1x) in [286, 794], of which the top 120 px are dropped for the message log.
WX=2700; ZN=1240; ZS=1620
LEGS=8; LEG_S=7.8            # 8 traverses of ~7 s, four of them southward, over ~62 s

"$TACLI" order "$INST" --unit 1 --expect ARMCOM move pos $WX $ZN >/dev/null 2>&1
"$TACLI" keys  "$INST" mouse:60,900 >/dev/null 2>&1   # the cursor sprite OUT of the world band
sleep 9

OFF=$(stat -c %s "$GD/tagpu.log")                     # slice this run's own log out by offset

taskset -acp 0 "$PID" >/dev/null 2>&1
SPIN=()
for _ in 1 2 3; do taskset -c 0 bash -c 'while :; do :; done' & SPIN+=($!); done

# -window_id reads the window's own redirected pixmap, so the capture is the game's frame
# whatever is stacked over it and the desktop stays the human's.  Lossless RGB at 60 fps:
# a one-present dropout is 16 ms and aliases away in a 30 fps capture, and a lossy encoder
# smears the one-frame artifact the detector thresholds on.
ffmpeg -y -loglevel error -f x11grab -window_id "$WID" -draw_mouse 0 -framerate 60 \
  -video_size 1920x1080 -i "$DISP" -c:v libx264rgb -preset ultrafast -qp 0 -t 62 \
  "$OUT/walk-$TAG.mkv" &
FF=$!
sleep 0.5
for leg in $(seq 1 $LEGS); do
    if [ $((leg % 2)) -eq 1 ]; then Z=$ZS; else Z=$ZN; fi
    "$TACLI" order "$INST" --unit 1 --expect ARMCOM move pos $WX $Z >/dev/null 2>&1
    sleep $LEG_S
done
wait $FF

for p in "${SPIN[@]}"; do kill "$p" 2>/dev/null; done
taskset -acp 0-$((NCPU-1)) "$PID" >/dev/null 2>&1

tail -c +$((OFF + 1)) "$GD/tagpu.log" | tr -d '\000' > "$OUT/log-$TAG.txt"
echo "== gate C, $TAG =="
grep -ao 'guard=[0-9]*' "$OUT/log-$TAG.txt" | cut -d= -f2 | paste -sd+ | bc \
    | xargs echo "  guard trips over the run:"
grep -ao 'rest=[0-9]*'  "$OUT/log-$TAG.txt" | cut -d= -f2 | paste -sd+ | bc \
    | xargs echo "  rest-equality catches:  "
grep -a 'native: ' "$OUT/log-$TAG.txt" | tail -1 | sed 's/^/  /'
python3 "$REPO/tools/gatec_detect.py" "$OUT/walk-$TAG.mkv"
