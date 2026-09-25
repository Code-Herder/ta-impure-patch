#!/usr/bin/env python3
"""The claims tagpu_glsl.h's line functions rest on, checked by brute force.

    tools/line-band-check.py            # exit 0 when every claim holds

WHAT IS CHECKED, for every line with both ends in a box around the origin
(exhaustively) and for random long lines out to tagpu_line.h's TAGPU_LINE_MAXC:

  1. THE CLOSED FORM IS THE WALK. `taOnLine`'s per-pixel test, transcribed with
     the same unsigned 32-bit arithmetic, keeps exactly the pixels a step-by-step
     transcription of `0x4CC7AB`'s loops lights -- no pixel more, none fewer --
     and no product in it passes 2^32.
  2. THE BAND HOLDS EVERY LIT PIXEL. Each corner of each lit pixel's square lies
     inside `taBand`'s rectangle (radius taBandR = 2 about the segment between
     the two end centres) with at least MARGIN to spare, so every target pixel
     of a lit game pixel is rasterised whatever the supersample.
  3. THE GRID MAP IS FLOOR. `taGamePx`'s ((2t + 1) * gw) / (2 * tw) is
     floor((t + 0.5) * gw / tw) for the grids the lane uses.

The walk here is written from the disassembly (exe-reverse-engineering.md,
`0x4CC7AB`), not from the closed form, so the two are independent readings.
"""

import math
import random
import sys

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


U32 = 1 << 32


def on_line(g, a, b):
    """taOnLine, line for line, with the GLSL's uint wrap made explicit."""
    if a[0] > b[0]:
        a, b = b, a
    dx, dy = b[0] - a[0], b[1] - a[1]
    ady, sg = abs(dy), (-1 if dy < 0 else 1)
    if ady <= dx:
        i = g[0] - a[0]
        if i < 0 or i > dx:
            return False
        if dx == 0:
            return g[1] == a[1]
        num = 2 * ady * i + dx
        assert num < U32, "unsigned overflow"
        return g[1] == a[1] + sg * (num // (2 * dx))
    j = (g[1] - a[1]) * sg
    if j < 0 or j > ady:
        return False
    num = 2 * dx * j + ady
    assert num < U32, "unsigned overflow"
    return g[0] == a[0] + num // (2 * ady)


def band_slack(a, b, lit):
    """The least distance by which any corner of a lit pixel sits inside the
    band's rectangle (negative = outside)."""
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
            s = min(R - abs(across), along + R, (ln + R) - along)
            worst = min(worst, s)
    return worst


def check(a, b, stats):
    lit = walk(a[0], a[1], b[0], b[1])
    lit_set = set(lit)
    if len(lit_set) != len(lit):
        return "the walk lit a pixel twice"
    # the closed form over the lit pixels' bounding box plus one
    xs = [p[0] for p in lit]
    ys = [p[1] for p in lit]
    if len(lit) <= 4096:
        for x in range(min(xs) - 1, max(xs) + 2):
            for y in range(min(ys) - 1, max(ys) + 2):
                if on_line((x, y), a, b) != ((x, y) in lit_set):
                    return "closed form and walk disagree at (%d, %d)" % (x, y)
    else:
        # long lines: every lit pixel is kept, and its neighbours across the
        # minor axis are not
        for (x, y) in lit:
            if not on_line((x, y), a, b):
                return "closed form drops lit (%d, %d)" % (x, y)
            for q in ((x, y - 1), (x, y + 1), (x - 1, y), (x + 1, y)):
                if q not in lit_set and on_line(q, a, b):
                    return "closed form keeps unlit (%d, %d)" % q
    s = band_slack(a, b, lit)
    stats["slack"] = min(stats["slack"], s)
    if s < MARGIN:
        return "a lit pixel's corner is %.3f inside the band" % s
    stats["lines"] += 1
    return None


def main():
    stats = {"lines": 0, "slack": float("inf")}
    # every shape out to BOX pixels: the walk and the test are both invariant
    # under a whole-pixel translation, so one end at the origin covers them
    # all, and the other end in every quadrant covers both walking directions
    box = 24
    for bx in range(-box, box + 1):
        for by in range(-box, box + 1):
            e = check((0, 0), (bx, by), stats)
            if e:
                print("FAIL (0,0)-(%d,%d): %s" % (bx, by, e))
                return 1
    rng = random.Random(0x4CC7AB)
    for _ in range(400):
        a = (rng.randint(-MAXC, MAXC), rng.randint(-MAXC, MAXC))
        if rng.random() < 0.5:
            b = (rng.randint(-MAXC, MAXC), rng.randint(-MAXC, MAXC))
        else:
            b = (a[0] + rng.randint(-600, 600), a[1] + rng.randint(-600, 600))
            b = (max(-MAXC, min(MAXC, b[0])), max(-MAXC, min(MAXC, b[1])))
        e = check(a, b, stats)
        if e:
            print("FAIL %s-%s: %s" % (a, b, e))
            return 1
    # the extreme corners, where the unsigned products are largest
    for a, b in (((-MAXC, -MAXC), (MAXC, MAXC)), ((-MAXC, MAXC), (MAXC, -MAXC)),
                 ((-MAXC, 0), (MAXC, 1)), ((0, -MAXC), (1, MAXC))):
        e = check(a, b, stats)
        if e:
            print("FAIL %s-%s: %s" % (a, b, e))
            return 1
    for gw, tw in ((1024, 2048), (1024, 1024), (1024, 3072), (640, 1920),
                   (1024, 1920), (768, 1080), (1920, 3840)):
        for t in range(tw):
            if ((2 * t + 1) * gw) // (2 * tw) != math.floor((t + 0.5) * gw / tw):
                print("FAIL taGamePx gw=%d tw=%d t=%d" % (gw, tw, t))
                return 1
    print("line-band-check: %d lines, closed form == walk, every lit corner at "
          "least %.3f inside the band (required %.2f); taGamePx is floor"
          % (stats["lines"], stats["slack"], MARGIN))
    return 0


if __name__ == "__main__":
    sys.exit(main())
