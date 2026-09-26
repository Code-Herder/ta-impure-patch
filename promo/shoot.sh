#!/usr/bin/env bash
# Shoot one montage clip: load a scenario, let the fight develop, grab the window.
#
#   promo/shoot.sh <clip-id> <outdir> [--res WxH] [--settle S] [--len S]
#                  [--instance NAME] [--keep]
#
# Writes <outdir>/<clip-id>.mp4 plus <outdir>/<clip-id>.txt (the real transcript,
# which is what the film's terminal prints) and <outdir>/<clip-id>-settle.png (the
# frame the capture is about to start on, so a bad moment is caught before the
# clip is used rather than after a 4K render).
#
# Why it is a script and not a paragraph of instructions: a new release re-shoots
# every clip, and the parts that are easy to get wrong -- grabbing the frame
# window instead of the client one, a 30 fps grab of a ~34 fps game, forgetting
# that the previous instance is still running -- are exactly the parts that stay
# fixed. See the ta-video-montage skill, *Capturing the clips*.
set -euo pipefail

die() { echo "shoot: $*" >&2; exit 1; }

[ $# -ge 2 ] || die "usage: promo/shoot.sh <clip-id> <outdir> [options]"
CLIP=$1; OUTDIR=$2; shift 2

RES=2048x1536        # hero default; filler-only clips are fine at 1024x768
SETTLE=40            # seconds between "applied" and the first captured frame
LEN=60               # seconds of footage -- longer than the longest time on screen
INSTANCE=""
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --res)      RES=$2; shift 2 ;;
    --settle)   SETTLE=$2; shift 2 ;;
    --len)      LEN=$2; shift 2 ;;
    --instance) INSTANCE=$2; shift 2 ;;
    --keep)     KEEP=1; shift ;;
    *) die "unknown option $1" ;;
  esac
done

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
TACLI=tools/tacli
[ -x "$TACLI" ] || die "no $TACLI under $ROOT"
[ -f "scenarios/$CLIP.json" ] || die "no scenarios/$CLIP.json"
: "${INSTANCE:=shoot_${CLIP//-/_}}"
mkdir -p "$OUTDIR"

echo "== $CLIP -> $OUTDIR/$CLIP.mp4  (res $RES, settle ${SETTLE}s, ${LEN}s)"

# --restart because a previous take may still be up; `load` refuses otherwise.
"$TACLI" scenario load "$INSTANCE" "$CLIP" --res "$RES" --vsync on --restart \
  2>&1 | tee "$OUTDIR/$CLIP.txt"
grep -q "^applied" "$OUTDIR/$CLIP.txt" || die "$CLIP: the scenario did not apply"

# The fight has to develop, and the load-time messages have to clear: killing the
# starting commanders with `clear_existing` eliminates both players, so TA prints
# "... have been exterminated" across the top of the frame for the first seconds.
echo "-- settling ${SETTLE}s"
sleep "$SETTLE"

read -r WIN DISPLAY_ID W H <<EOF
$("$TACLI" ls --json | python3 -c '
import json, sys
name = sys.argv[1]
for i in json.load(sys.stdin):
    if i["name"] == name:
        if not i.get("running") or not i.get("window"):
            raise SystemExit("not running")
        # window[0] is the CLIENT id -- the frame id grabs the decoration too
        print("0x%x" % i["window"][0], i["display"], i["res"][0], i["res"][1])
        break
else:
    raise SystemExit("no such instance")
' "$INSTANCE")
EOF
[ -n "${WIN:-}" ] || die "$CLIP: could not resolve the client window"
# The window has to BE the size we are about to grab. A title-matching leftover
# from the instance stopped moments earlier is the classic wrong answer, and it
# shows up here as TA's 640x480 shell size rather than the game resolution.
if [ "$W" != "${RES%x*}" ] || [ "$H" != "${RES#*x}" ]; then
  die "$CLIP: the window resolved to ${W}x${H}, not $RES — that is not this"\
      " instance's game window (a stale one from the previous take?)"
fi
echo "-- window $WIN on $DISPLAY_ID, ${W}x${H}"

# The frame the capture starts on, so the moment can be judged before the clip is
# spent: the client window itself, grabbed by id, the same pixels x11grab takes.
if DISPLAY="$DISPLAY_ID" import -window "$WIN" -resize 1024x "$OUTDIR/$CLIP-settle.png" 2>/dev/null; then
  :
else
  echo "-- no settle shot (window grab failed)" >&2
fi

# 60 fps, not 30: the game presents ~34 unique frames a second, so a 30 fps grab
# samples it at a beat frequency. The montage extracts the 30 it wants.
# +faststart puts the moov atom at the FRONT: without it a player has to fetch
# the whole file before the first frame, which makes a 400 MB take unreviewable
# over HTTP. It costs one rewrite pass at the end of the capture.
# -draw_mouse 0: the game draws its own cursor, X's pointer would be a second one.
#
# A grab can END EARLY and ffmpeg still exits 0, leaving a short but perfectly
# valid mp4: the window's contents stop being readable (BadMatch on ShmGetImage,
# then on XGetImage) and the demuxer gives up with "Permission denied". It has
# happened at 0.2 s into a take and at 36 s into the next one, with the window
# still mapped, still the right size, and no crash recorded -- something on the
# desktop transiently makes the window unreadable. So: expect a frame count, and
# take the clip again if it is short.
WANT=$(python3 -c "print(int($LEN * 60))")
ATTEMPT=1
while :; do
  ffmpeg -y -v error -f x11grab -window_id "$WIN" -draw_mouse 0 \
    -framerate 60 -video_size "${W}x${H}" -i "$DISPLAY_ID" \
    -c:v libx264 -preset ultrafast -crf 15 -pix_fmt yuv420p \
    -movflags +faststart \
    -t "$LEN" "$OUTDIR/$CLIP.mp4" || true

  GOT=$(ffprobe -v error -count_frames -select_streams v:0 \
        -show_entries stream=nb_read_frames -of csv=p=0 "$OUTDIR/$CLIP.mp4" \
        2>/dev/null | tr -d ',\n')
  GOT=${GOT:-0}
  if [ "$GOT" -ge $((WANT * 95 / 100)) ]; then break; fi
  echo "-- short take: $GOT of $WANT frames (attempt $ATTEMPT)" >&2
  ATTEMPT=$((ATTEMPT + 1))
  [ "$ATTEMPT" -le 3 ] || die "$CLIP: three short takes -- the window keeps"\
      " becoming unreadable mid-grab; shoot it alone with nothing else on screen"
  sleep 3
done

# Did the GAME end while we were filming? When the last unit of a player dies TA
# declares the game over and drops back to its 640x480 shell, the window shrinks
# under the grab, and the take stops there -- reproducibly, at the same frame
# every attempt. Every promo scenario keeps its starting commanders
# (`clear_existing: false`) so no player is ever unitless; this catches the case
# where that is not enough and the answer is a shorter take, not another retry.
END_W=$("$TACLI" ls --json | python3 -c '
import json, sys
for i in json.load(sys.stdin):
    if i["name"] == sys.argv[1] and i.get("window"):
        print(i["window"][3])
        break
else:
    print(0)
' "$INSTANCE")
if [ "${END_W:-0}" != "0" ] && [ "$END_W" != "${RES%x*}" ]; then
  echo "-- the game ended during the take (window is now ${END_W} wide, the"\
       " 640x480 shell): settle earlier or shoot a shorter clip" >&2
fi

[ "$KEEP" = 1 ] || "$TACLI" stop "$INSTANCE" >/dev/null 2>&1 || true

# An encode is unverified until it decodes end to end, and a duplicate-frame rate
# near 100% means the wrong window was grabbed (ta-capture rule 5).
ffmpeg -v error -i "$OUTDIR/$CLIP.mp4" -f null - || die "$CLIP: does not decode"
ffmpeg -v error -i "$OUTDIR/$CLIP.mp4" -vf scale=256:192,format=gray -f rawvideo - \
  2>/dev/null | python3 -c '
import sys, numpy as np
d = np.frombuffer(sys.stdin.buffer.read(), dtype=np.uint8)
d = d.reshape(-1, 192, 256).astype(np.int16)
diff = np.abs(np.diff(d, axis=0)).mean(axis=(1, 2))
dup = float((diff < 0.05).mean())
print(f"   {len(d)} frames, {dup*100:.1f}% duplicate, ~{60*(1-dup):.0f} unique fps")
if dup > 0.85:
    raise SystemExit("   WRONG WINDOW: a near-total duplicate rate is a region "
                     "grab of something static, not a running game")
'
echo "   ok: $OUTDIR/$CLIP.mp4 ($GOT frames)"
