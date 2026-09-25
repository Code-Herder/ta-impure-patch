#!/usr/bin/env python3
"""The claims tagpu_glsl.h's line functions rest on, checked by brute force.

    tools/line-band-check.py            # exit 0 when every claim holds

WHAT IS CHECKED, for every line with one end at the origin and the other in a
box around it (exhaustively: 24 pixels at w = 1, 12 at 2, 10 at 3, 8 at 4) and
for random long lines out to tagpu_line.h's TAGPU_LINE_MAXC, at thickness
w = 1 (the engine's line) and 2, 3 and 4 (supersampled, up to TAGPU_SS_MAX):

  1. THE CLOSED FORM IS THE RULE. `taOnLine`'s per-pixel test, transcribed with
     the same unsigned 32-bit arithmetic, keeps exactly the pixels a
     step-by-step transcription of `0x4CC7AB`'s loops lights, carried along
     the major axis to a w x w block at each end and each copied w times
     along the minor axis from floor(w/2) before it (`thicken`) -- no pixel
     more, none fewer -- and no product in it passes 2^32.
  1b. THE CORNERS CLOSE. An engine line put on the line grid at ss = w lights
     exactly the ss x ss blocks of the game pixels the walk lights at 1x, for
     rows, columns, single pixels and a rect's four edges (`check_blocks`).
  2. THE BAND HOLDS EVERY LIT PIXEL. Each corner of each lit pixel's square lies
     inside `taBand`'s rectangle (radius taBandR = 2 game pixels, 2w line-grid
     pixels, about the segment between the two end centres) with at least
     MARGIN game pixels to spare, so every lit pixel is rasterised.
  3. THE GRID MAP IS FLOOR. `taLinePx`'s ((2t + 1) * lw) / (2 * tw) is
     floor((t + 0.5) * lw / tw) for the grids the lane uses.
  4. THE CLIP IS DrawLine's. tagpu_line.h's `tagpu_line_clip`, compiled with
     the host gcc, gives the same verdict and the same ends as `clip` below
     -- `0x4BEA20` then `0x4CC650` -- on random lines across each edge, long
     ones and ones with an end out to TAGPU_LINE_FAR, and its ends lie on the
     surface.

The walk and the clips here are written from the disassembly
(exe-reverse-engineering.md, `0x4CC7AB`, `0x4BEA20`, `0x4CC650`), not from the
closed form or the C, so each pair is two independent readings.
"""

import math
import pathlib
import random
import sys

HERE = pathlib.Path(__file__).resolve().parent

MAXC = 16383          # tagpu_line.h TAGPU_LINE_MAXC
R = 2.0               # tagpu_glsl.h taBandR
MARGIN = 0.75         # tagpu_glsl.h derives 0.79; the check requires this much


def walk(x0, y0, x1, y1):
    """0x4CC7AB's loops, after the clip, as the disassembly steps them."""
    if x1 - x0 == 0:                       # 0x4CC83B: a column, from the smaller y
        lo, hi = min(y0, y1), max(y0, y1)
        return [(x0, y) for y in range(lo, hi + 1)]
    if x1 < x0:                            # swap so the walk goes +x
        x0, y0, x1, y1 = x1, y1, x0, y0
    dx = x1 - x0
    if y1 - y0 == 0:                       # 0x4CC866: a row, rep stosb
        return [(x, y0) for x in range(x0, x1 + 1)]
    sy = 1
    dy = y1 - y0
    if dy < 0:                             # the pitch is negated, not the walk
        dy, sy = -dy, -1
    ymajor = dy > dx                       # `cmp ebx,ecx; jle` keeps 45 deg x-major
    major, minor = (dy, dx) if ymajor else (dx, dy)
    inc_lo = 2 * minor                     # [ebp-4]
    inc_hi = 2 * minor - 2 * major         # [ebp-8]
    err = 2 * minor - major                # esi
    x, y = x0, y0
    out = []
    for _ in range(major + 1):             # ecx = major + 1, `loop`
        out.append((x, y))
        if ymajor:                         # 0x4CC8AC: y always steps
            y += sy
            if err < 0:
                err += inc_lo
            else:
                err += inc_hi
                x += 1
        else:                              # 0x4CC880: x always steps
            x += 1
            if err < 0:
                err += inc_lo
            else:
                err += inc_hi
                y += sy
    return out


def tdiv(a, b):
    """idiv: the quotient truncated toward zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def clip_rect(x0, y0, x1, y1, L, T, R, B):
    """0x4BEA20, as the disassembly steps it: one pass, end 0 then end 1, each
    against x < L, y < T, x > R, y > B, moved with the ORIGINAL deltas.
    None when it returns 0."""
    xle, yle = x0 <= x1, y0 <= y1          # 0x4BEA3E / 0x4BEA4F setle
    dx, dy = x1 - x0, y1 - y0
    if x0 < L:                             # 0x4BEA72
        if not xle or dx == 0:
            return None
        y0 += tdiv((L - x0) * dy, dx); x0 = L
    if y0 < T:                             # 0x4BEAAD
        if not yle or dy == 0:
            return None
        x0 += tdiv((T - y0) * dx, dy); y0 = T
    if x0 > R:                             # 0x4BEAE7
        if xle or dx == 0:
            return None
        y0 += tdiv((R - x0) * dy, dx); x0 = R
    if y0 > B:                             # 0x4BEB23
        if yle or dy == 0:
            return None
        x0 += tdiv((B - y0) * dx, dy); y0 = B
    if x1 < L:                             # 0x4BEB5E
        if xle or dx == 0:
            return None
        y1 += tdiv((L - x1) * dy, dx); x1 = L
    if y1 < T:                             # 0x4BEBA1
        if yle or dy == 0:
            return None
        x1 += tdiv((T - y1) * dx, dy); y1 = T
    if x1 > R:                             # 0x4BEBDC
        if not xle or dx == 0:
            return None
        y1 += tdiv((R - x1) * dy, dx); x1 = R
    if y1 > B:                             # 0x4BEC11
        if not yle or dy == 0:
            return None
        x1 += tdiv((B - y1) * dx, dy); y1 = B
    return x0, y0, x1, y1


def clip_surface(x0, y0, x1, y1, w, h, passes=None):
    """0x4CC650: loop until every end is on [0, w) x [0, h), rejecting a line
    wholly off one side, moving ends with the CURRENT deltas. None on reject.
    `passes`, a list, gets the number of moving passes appended."""
    n = 0
    while True:
        if 0 <= x0 < w and 0 <= x1 < w and 0 <= y0 < h and 0 <= y1 < h:
            if passes is not None:
                passes.append(n)
            return x0, y0, x1, y1
        n += 1
        ddx, ddy = x1 - x0, y1 - y0
        if ddx >= 0:                       # 0x4CC69E jns
            if x0 >= w or x1 < 0:
                return None
        elif x0 < 0 or x1 >= w:
            return None
        if ddy >= 0:
            if y0 >= h or y1 < 0:
                return None
        elif y0 < 0 or y1 >= h:
            return None
        if x0 < 0:
            y0 += tdiv(-x0 * ddy, ddx); x0 = 0
        elif x0 >= w:
            y0 += tdiv((w - 1 - x0) * ddy, ddx); x0 = w - 1
        if y0 < 0:
            x0 += tdiv(-y0 * ddx, ddy); y0 = 0
        elif y0 >= h:
            x0 += tdiv((h - 1 - y0) * ddx, ddy); y0 = h - 1
        if x1 < 0:
            y1 += tdiv(-x1 * ddy, ddx); x1 = 0
        elif x1 >= w:
            y1 += tdiv((w - 1 - x1) * ddy, ddx); x1 = w - 1
        if y1 < 0:
            x1 += tdiv(-y1 * ddx, ddy); y1 = 0
        elif y1 >= h:
            x1 += tdiv((h - 1 - y1) * ddx, ddy); y1 = h - 1


def clip(x0, y0, x1, y1, L, T, R, B, w, h, passes=None):
    """DrawLine 0x4BE950's two clips in its order, the rect held to the
    surface as vpwide holds it. None when nothing is drawn."""
    L, T, R, B = max(L, 0), max(T, 0), min(R, w - 1), min(B, h - 1)
    if L > R or T > B:
        return None
    r = clip_rect(x0, y0, x1, y1, L, T, R, B)
    return None if r is None else clip_surface(*r, w, h, passes)


U32 = 1 << 32


def thicken(lit, a, b, w):
    """tagpu_line.h's thickening, from the walk's pixels: the walk carried
    floor(w/2) pixels before its lower end and w - 1 - floor(w/2) after its
    upper end along the major axis, at that end's minor coordinate, then each
    pixel copied to w pixels along the minor axis, from floor(w/2) before it.
    The minor axis is y when |dy| <= |dx| -- the walk's own x-major test --
    and x otherwise."""
    xmaj = abs(b[1] - a[1]) <= abs(b[0] - a[0])
    lo = w // 2
    lit = list(lit)
    if xmaj:
        (lx, ly), (hx, hy) = sorted((a, b))
        lit += [(lx - s, ly) for s in range(1, lo + 1)]
        lit += [(hx + s, hy) for s in range(1, w - lo)]
    else:
        (ly, lx), (hy, hx) = sorted(((a[1], a[0]), (b[1], b[0])))
        lit += [(lx, ly - s) for s in range(1, lo + 1)]
        lit += [(hx, hy + s) for s in range(1, w - lo)]
    out = set()
    for (x, y) in lit:
        for k in range(w):
            out.add((x, y - lo + k) if xmaj else (x - lo + k, y))
    return out


def on_line(g, a, b, w=1):
    """taOnLine, line for line, with the GLSL's uint wrap made explicit."""
    if a[0] > b[0]:
        a, b = b, a
    dx, dy = b[0] - a[0], b[1] - a[1]
    ady, sg = abs(dy), (-1 if dy < 0 else 1)
    lo = w // 2
    if ady <= dx:
        if g[0] < a[0] - lo or g[0] > b[0] - lo + w - 1:
            return False
        i = min(max(g[0] - a[0], 0), dx)
        if dx == 0:
            r = a[1]
        else:
            num = 2 * ady * i + dx
            assert num < U32, "unsigned overflow"
            r = a[1] + sg * (num // (2 * dx))
        k = g[1] - r + lo
        return 0 <= k < w
    if g[1] < min(a[1], b[1]) - lo or g[1] > max(a[1], b[1]) - lo + w - 1:
        return False
    j = min(max((g[1] - a[1]) * sg, 0), ady)
    num = 2 * dx * j + ady
    assert num < U32, "unsigned overflow"
    c = a[0] + num // (2 * ady)
    k = g[0] - c + lo
    return 0 <= k < w


def band_slack(a, b, lit, w):
    """The least distance, in GAME pixels, by which any corner of a lit
    line-grid pixel sits inside the band's rectangle (negative = outside):
    the band is taBandR game pixels, R * w line-grid pixels, about the segment
    between the two end pixels' centres."""
    rw = R * w
    ca = (a[0] + 0.5, a[1] + 0.5)
    cb = (b[0] + 0.5, b[1] + 0.5)
    d = (cb[0] - ca[0], cb[1] - ca[1])
    ln = math.hypot(*d)
    u = (d[0] / ln, d[1] / ln) if ln > 0 else (1.0, 0.0)
    n = (-u[1], u[0])
    worst = float("inf")
    for (px, py) in lit:
        for cx, cy in ((px, py), (px + 1, py), (px, py + 1), (px + 1, py + 1)):
            rx, ry = cx - ca[0], cy - ca[1]
            along = rx * u[0] + ry * u[1]
            across = rx * n[0] + ry * n[1]
            s = min(rw - abs(across), along + rw, (ln + rw) - along)
            worst = min(worst, s)
    return worst / w


def check(a, b, stats, w):
    walked = walk(a[0], a[1], b[0], b[1])
    if len(set(walked)) != len(walked):
        return "the walk lit a pixel twice"
    lit_set = thicken(walked, a, b, w)
    # the closed form over the lit pixels' bounding box plus one
    xs = [p[0] for p in lit_set]
    ys = [p[1] for p in lit_set]
    if len(lit_set) <= 4096:
        for x in range(min(xs) - 1, max(xs) + 2):
            for y in range(min(ys) - 1, max(ys) + 2):
                if on_line((x, y), a, b, w) != ((x, y) in lit_set):
                    return "closed form and walk disagree at (%d, %d), w %d" % (x, y, w)
    else:
        # long lines: every lit pixel is kept, and its unlit neighbours are not
        for (x, y) in lit_set:
            if not on_line((x, y), a, b, w):
                return "closed form drops lit (%d, %d), w %d" % (x, y, w)
            for q in ((x, y - 1), (x, y + 1), (x - 1, y), (x + 1, y)):
                if q not in lit_set and on_line(q, a, b, w):
                    return "closed form keeps unlit (%d, %d), w %d" % (q + (w,))
    s = band_slack(a, b, lit_set, w)
    stats["slack"] = min(stats["slack"], s)
    if s < MARGIN:
        return "a lit pixel's corner is %.3f inside the band, w %d" % (s, w)
    stats["lines"] += 1
    return None


def main():
    stats = {"lines": 0, "slack": float("inf")}
    # every shape out to a box: the walk and the test are both invariant
    # under a whole-pixel translation, so one end at the origin covers them
    # all, and the other end in every quadrant covers both walking directions.
    # w = 1 is the engine's line; 2, 3 and 4 (TAGPU_SS_MAX) the supersampled
    # thicknesses. The box shrinks as w grows because the brute force grows
    # with the lit area.
    for w, box in BOXES:
        for bx in range(-box, box + 1):
            for by in range(-box, box + 1):
                e = check((0, 0), (bx, by), stats, w)
                if e:
                    print("FAIL (0,0)-(%d,%d): %s" % (bx, by, e))
                    return 1
    rng = random.Random(0x4CC7AB)
    for i in range(800):
        w = (1, 2, 3, 4)[i % 4]
        a = (rng.randint(-MAXC, MAXC), rng.randint(-MAXC, MAXC))
        if rng.random() < 0.5:
            b = (rng.randint(-MAXC, MAXC), rng.randint(-MAXC, MAXC))
        else:
            b = (a[0] + rng.randint(-600, 600), a[1] + rng.randint(-600, 600))
            b = (max(-MAXC, min(MAXC, b[0])), max(-MAXC, min(MAXC, b[1])))
        e = check(a, b, stats, w)
        if e:
            print("FAIL %s-%s: %s" % (a, b, e))
            return 1
    # the extreme corners, where the unsigned products are largest
    for a, b in (((-MAXC, -MAXC), (MAXC, MAXC)), ((-MAXC, MAXC), (MAXC, -MAXC)),
                 ((-MAXC, 0), (MAXC, 1)), ((0, -MAXC), (1, MAXC))):
        for w in (2, 4):
            e = check(a, b, stats, w)
            if e:
                print("FAIL %s-%s: %s" % (a, b, e))
                return 1
    e = check_blocks()
    if e:
        print("FAIL blocks: %s" % e)
        return 1
    for lw, tw in ((2048, 2048), (1024, 1024), (2048, 3072), (1920, 1920),
                   (2048, 1920), (1536, 1080), (3840, 3840)):
        for t in range(tw):
            if ((2 * t + 1) * lw) // (2 * tw) != math.floor((t + 0.5) * lw / tw):
                print("FAIL taLinePx lw=%d tw=%d t=%d" % (lw, tw, t))
                return 1
    e = check_clip()
    if e:
        print("FAIL clip: %s" % e)
        return 1
    print("line-band-check: %d lines (every shape in a box of %s, and long ones), "
          "closed form == walk + thickening, every lit corner at least %.3f game px inside "
          "the band (required %.2f); %s; taLinePx is floor; %s"
          % (stats["lines"], ", ".join("%d at w=%d" % (b, w) for w, b in BOXES),
             stats["slack"], MARGIN, BLOCKS_SAID[0], CLIP_SAID[0]))
    return 0


BOXES = ((1, 24), (2, 12), (3, 10), (4, 8))
BLOCKS_SAID = [""]


def check_blocks():
    """THE CORNERS CLOSE. An engine line whose ends are integer game pixels,
    put on the line grid at ss * k + ss // 2 (tagpu_line_px of the pixel's
    centre at 1x), lights at ss = w exactly the ss x ss blocks of the game
    pixels 0x4CC7AB lights at 1x -- for rows, columns and single pixels, and
    for the four edges of a rect, whose union must be the blocks of the
    rect's outline with every corner block whole."""
    rng = random.Random(0x438C00)

    def at(k, ss):
        return ss * k + ss // 2

    def blocks(pixels, ss):
        return {(ss * x + u, ss * y + v) for (x, y) in pixels
                for u in range(ss) for v in range(ss)}

    def lit(a, b, ss):
        A, B = (at(a[0], ss), at(a[1], ss)), (at(b[0], ss), at(b[1], ss))
        return thicken(walk(A[0], A[1], B[0], B[1]), A, B, ss)

    n = 0
    for ss in (2, 3, 4):
        for _ in range(400):
            x0, y0 = rng.randint(-40, 40), rng.randint(-40, 40)
            ln = rng.randint(0, 30)
            for a, b in (((x0, y0), (x0 + ln, y0)), ((x0 + ln, y0), (x0, y0)),
                         ((x0, y0), (x0, y0 + ln)), ((x0, y0 + ln), (x0, y0))):
                if lit(a, b, ss) != blocks(walk(a[0], a[1], b[0], b[1]), ss):
                    return "ss %d: %s-%s is not its game pixels' blocks" % (ss, a, b)
                n += 1
            x1, y1 = x0 + rng.randint(0, 30), y0 + rng.randint(0, 30)
            edges = (((x0, y0), (x1, y0)), ((x0, y1), (x1, y1)),
                     ((x0, y0), (x0, y1)), ((x1, y0), (x1, y1)))
            got = set().union(*(lit(a, b, ss) for a, b in edges))
            want = blocks(set().union(*(walk(a[0], a[1], b[0], b[1]) for a, b in edges)), ss)
            if got != want:
                return "ss %d: rect (%d,%d)-(%d,%d) is not its outline's blocks" % (ss, x0, y0, x1, y1)
            n += 1
    BLOCKS_SAID[0] = ("axis-aligned engine lines and rects light exactly their game pixels' "
                      "blocks at ss = 2, 3, 4 (%d cases)" % n)
    return None


CLIP_SAID = [""]
FAR = 1 << 29                              # tagpu_line.h TAGPU_LINE_FAR
CLIP_C = r"""
#include "tagpu_line.h"
int clip(int* e, int L, int T, int R, int B, int w, int h)
{ return tagpu_line_clip(&e[0], &e[1], &e[2], &e[3], L, T, R, B, w, h); }
"""


def check_clip():
    """4. tagpu_line_clip IS THE ENGINE'S CLIP: the C function, compiled from
    tagpu_line.h, against clip() above on random lines -- short ones across
    each edge, long ones, and ends out to TAGPU_LINE_FAR -- with the same
    verdict and the same ends, the ends inside the surface, and no line
    needing more than the passes the C bound allows."""
    import ctypes
    import subprocess
    import tempfile
    src = HERE.parent / "tagpu" / "ddraw" / "src"
    with tempfile.TemporaryDirectory() as td:
        c = pathlib.Path(td) / "clip.c"
        so = pathlib.Path(td) / "clip.so"
        c.write_text(CLIP_C)
        r = subprocess.run(["gcc", "-shared", "-fPIC", "-O1", "-D__inline=inline",
                            "-D_snprintf=snprintf", "-I", str(src), "-o", str(so), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            return "gcc: " + r.stderr.strip()
        lib = ctypes.CDLL(str(so))
        lib.clip.restype = ctypes.c_int
        rng = random.Random(0x4BEA20)
        n = moved = rejected = 0
        passes = []
        for (L, T, R, B, w, h) in ((128, 32, 1023, 735, 1024, 768),
                                   (128, 32, 1919, 1047, 1920, 1080),
                                   (-40, -40, 2000, 900, 1024, 768)):
            for i in range(30000):
                k = i % 3
                if k == 0:                 # short, across one edge
                    cx = rng.choice((L, R, rng.randint(L, R)))
                    cy = rng.choice((T, B, rng.randint(T, B)))
                    e = [cx + rng.randint(-60, 60), cy + rng.randint(-60, 60),
                         cx + rng.randint(-60, 60), cy + rng.randint(-60, 60)]
                elif k == 1:               # long, anywhere near the frame
                    e = [rng.randint(-3000, 4000) for _ in range(4)]
                else:                      # one end far out, as a deep zoom makes
                    e = [rng.randint(-FAR, FAR), rng.randint(-FAR, FAR),
                         rng.randint(0, w - 1), rng.randint(0, h - 1)]
                    if rng.random() < 0.5:
                        e = e[2:] + e[:2]
                want = clip(*e, L, T, R, B, w, h, passes)
                arr = (ctypes.c_int * 4)(*e)
                ok = lib.clip(arr, L, T, R, B, w, h)
                got = tuple(arr) if ok else None
                if got != want:
                    return "%s rect %s: C %s, transcription %s" % (e, (L, T, R, B, w, h),
                                                                   got, want)
                n += 1
                if want is None:
                    rejected += 1
                    continue
                if not (0 <= want[0] < w and 0 <= want[2] < w and
                        0 <= want[1] < h and 0 <= want[3] < h):
                    return "%s: ends off the surface %s" % (e, want)
                moved += tuple(e) != want
        if max(passes) > 3:
            return "a line took %d passes of 0x4CC650" % max(passes)
    CLIP_SAID[0] = ("tagpu_line_clip == 0x4BEA20 + 0x4CC650 on %d lines (%d moved, "
                    "%d rejected, at most %d surface pass(es))"
                    % (n, moved, rejected, max(passes)))
    return None


if __name__ == "__main__":
    sys.exit(main())
