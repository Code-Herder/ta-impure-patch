#!/bin/bash
# vkcoexist-pixels.sh -- the half of G19a's coexistence gate that looks at the SCREEN.
#
# tools/vkcoexist.c answers the API question: do the Vulkan calls succeed on a
# window the fork's GL renderer already owns, and does GL still accept draws
# afterwards? Both can be yes while the window shows Vulkan's last frame for
# ever -- measured in-game 2026-09-15, where GL went on producing correct frames
# (`tacli glshot`: 168 distinct colours) that nothing ever saw.
#
# So this script asks the only question that decides the design. Per route it
# runs `vkcoexist --route X --hold N`, which finishes by painting the window a
# known colour for N seconds, grabs the X window, and reports how much of it is
# that colour. Every route names the LANE whose survival it is asking about, and
# that is which lane paints the hold:
#
#   A B C D  the GL lane, green -- "does GL still reach the screen?"
#   GL       the control for those: no Vulkan is touched at all, so a run that
#            reads no green there is measuring a broken harness, not a route.
#   E        VULKAN's own frame, flat magenta -- on the vulkan-only route there
#            is no second lane, so the question is whether Vulkan itself is seen
#            on a window that never had a GL context or a pixel format.
#   F        the GDI lane, the same green -- "after a surface has existed on this
#            HWND, can `gdi_render_main`'s fallback still be SEEN?" That decides
#            whether render_vk.c may fall back late or only before the surface.
#   GD       the control for F: GDI alone, Vulkan never touched.
#
#   tools/vkcoexist-pixels.sh                      # every route, system wine
#   tools/vkcoexist-pixels.sh A D                  # just these
#   tools/vkcoexist-pixels.sh GD E F               # the vulkan-only pair + control
#   WINE=/path/to/proton/files/bin/wine tools/vkcoexist-pixels.sh
#
# It uses a THROWAWAY WINEPREFIX under $TMPDIR, never the project's -- Proton's
# wine 11 upgrades a prefix made by wine 9.0 in place.
#
# THE WINDOW IS SHOWN, because an unmapped one has no pixels to grab. It is
# 320x240, placed at 40,40, and created WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE, so
# it never takes focus and never moves the pointer. Each route is a few seconds.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE="${WINE:-wine}"
HOLD="${HOLD:-4}"
WORK="${TMPDIR:-/tmp}/vkcoexist-pixels.$$"
ROUTES=("$@")
[ ${#ROUTES[@]} -eq 0 ] && ROUTES=(GL A B C D GD E F)

command -v import >/dev/null 2>&1 || { echo "need ImageMagick's 'import'" >&2; exit 2; }
command -v xdotool >/dev/null 2>&1 || { echo "need xdotool (to find the window)" >&2; exit 2; }
command -v "$WINE" >/dev/null 2>&1 || { echo "no wine at: $WINE" >&2; exit 2; }

mkdir -p "$WORK" || exit 2
trap 'rm -rf "$WORK"' EXIT

echo "building vkcoexist32.exe"
i686-w64-mingw32-gcc -std=c99 -O1 -Wall -I"$ROOT/tagpu/ddraw/inc" \
    -o "$WORK/vkcoexist32.exe" "$ROOT/tools/vkcoexist.c" -lopengl32 -lgdi32 || exit 2

export WINEPREFIX="$WORK/prefix" WINEDEBUG=-all
"$WINE"boot -u >/dev/null 2>&1

# The green gl_hold and gdi_hold paint, as the X server will report it: HOLD_G
# 0.85 in a UNORM framebuffer is 217, and gdi_hold uses RGB(0,217,0) to match.
GREEN_R=0; GREEN_G=217; GREEN_B=0
# Route E's hold is Vulkan's own flat magenta. It shares no channel with the
# green, so no reading is ambiguous about which lane it came from.
MAGENTA_R=255; MAGENTA_G=0; MAGENTA_B=255

printf '\n%-5s %-10s %-9s %-6s %s\n' ROUTE "PIXELS" "API" "LANE" "VERDICT"
for r in "${ROUTES[@]}"; do
    # Which lane paints this route's hold, and so which colour is expected.
    case "$r" in
        E)    lane=vulkan; want_r=$MAGENTA_R; want_g=$MAGENTA_G; want_b=$MAGENTA_B ;;
        F|GD) lane=gdi;    want_r=$GREEN_R;   want_g=$GREEN_G;   want_b=$GREEN_B ;;
        *)    lane=gl;     want_r=$GREEN_R;   want_g=$GREEN_G;   want_b=$GREEN_B ;;
    esac
    ( "$WINE" "$WORK/vkcoexist32.exe" --route "$r" --hold "$HOLD" >"$WORK/out.$r" 2>&1 ) &
    runner=$!

    # Grab once the hold has started, and once it has had a frame or two.
    win=""
    for _ in $(seq 1 $((HOLD * 10 + 60))); do
        grep -q '\.\.\. holding ' "$WORK/out.$r" 2>/dev/null && break
        sleep 0.2
    done
    sleep 1
    # --onlyvisible matters: route D creates TWO windows with this title and
    # hides the Vulkan one before the hold, so an unfiltered search can hand
    # back the unmapped one and the grab reads nothing.
    win="$(xdotool search --onlyvisible --name '^vkcoexist$' 2>/dev/null | tail -1)"
    if [ -n "$win" ]; then
        import -window "$win" "$WORK/shot.$r.png" 2>/dev/null
    fi
    wait $runner; api=$?

    pct="(no window)"
    if [ -s "$WORK/shot.$r.png" ]; then
        pct=$(python3 - "$WORK/shot.$r.png" "$want_r" "$want_g" "$want_b" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
r, g, b = (int(x) for x in sys.argv[2:5])
px = list(im.getdata())
# a generous tolerance: the question is "is this our colour", not "which shade"
hit = sum(1 for p in px if abs(p[0]-r) < 24 and abs(p[1]-g) < 24 and abs(p[2]-b) < 24)
print("%.1f%%" % (100.0 * hit / len(px)))
PY
)
    fi

    upper="$(echo "$lane" | tr '[:lower:]' '[:upper:]')"
    case "$pct" in
        100.0%|9[0-9].*%) verdict="$upper REACHES THE SCREEN" ;;
        "(no window)")    verdict="could not grab -- route refused before the hold?" ;;
        *)                verdict="$upper DOES NOT REACH THE SCREEN" ;;
    esac
    printf '%-5s %-10s %-9s %-6s %s\n' "$r" "$pct" \
        "$([ $api -eq 0 ] && echo ok || echo refused)" "$lane" "$verdict"
done

echo
echo "PIXELS is the fraction of the window showing the colour the route's own LANE"
echo "painted after the Vulkan part finished. GL and GD are the controls for their"
echo "lanes and must read ~100%; a route that reads low is a lane the window no"
echo "longer shows."
