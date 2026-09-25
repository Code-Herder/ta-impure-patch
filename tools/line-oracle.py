#!/usr/bin/env python3
"""A capture of the lane's lines against a CPU model of tagpu_line.h's rule.

    tools/line-oracle.py CAPTURE.ppm LINES.txt [--diff out.png]

WHAT IT COMPARES. A pass whose A/B claims a frame writes the capture
(`tagpu_<pass>_vk.ppm`) and, beside it, the lines it handed the GPU that frame
(`tagpu_<pass>_lines.txt`: a header `# <pass> <grid w> <grid h> <w>`, then
`ax ay bx by col` a line, the ends as tagpu_line.h decided them, in LINE-GRID
pixels -- the world target's own, `w` = ss to a game pixel). When the header
carries a rect as well (`... <L> <T> <R> <B>`, the markers and the effects),
the ends are the ones BEFORE DrawLine's clip, and each line is first clipped
with line-band-check.py's transcription of `0x4BEA20` and `0x4CC650` and the
pixels outside the rect dropped, as the viewport scissor drops them. This
walks every line with `walk` -- a transcription of `0x4CC7AB`'s loops from
the disassembly, not of the fragment stage's closed form -- copies each lit
pixel w times along the line's minor axis (`thicken`), and compares that set
with the capture's lit pixels. The verdict is 0 px or not: exit 0 only when no
pixel is lit in one and not the other. At w = 1 the model is DrawLine's own
line on the game-pixel grid.

THE CAPTURE MUST HOLD ONLY LINES. Anything else the pass draws (a marker's
bar, an effect's sprite, a unit's body) is lit pixels the model does not have,
and a unit's body hides wire behind it. The captures this was run on come from
a build whose recorders drew the line draws alone; the gate's notes say how.

The capture must be the line grid itself (the world target at ss); one of any
other size is refused rather than resampled. Pixels that fall off it are
dropped from the model, as the rasteriser drops them.
"""

import argparse
import importlib.util
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent


def _engine():
    spec = importlib.util.spec_from_file_location("line_band_check",
                                                  HERE / "line-band-check.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_ppm(path):
    data = pathlib.Path(path).read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b"\r"):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    if fields[0] != b"P6" or fields[3] != b"255":
        raise SystemExit("%s: not a binary PPM with maxval 255" % path)
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(data[i:i + w * h * 3], np.uint8).reshape(h, w, 3)


def read_lines(path):
    """(pass, lw, lh, w, rect or None, lines). A header with a rect lists the
    lines before DrawLine's clip (tagpu_line.h)."""
    rows = pathlib.Path(path).read_text().split("\n")
    head = rows[0].split()
    if len(head) not in (5, 9) or head[0] != "#":
        raise SystemExit("%s: no `# <pass> <grid w> <grid h> <w> [<L> <T> <R> <B>]` "
                         "header" % path)
    lw, lh, w = int(head[2]), int(head[3]), int(head[4])
    rect = tuple(map(int, head[5:9])) if len(head) == 9 else None
    lines = []
    for r in rows[1:]:
        if r.strip():
            ax, ay, bx, by, col = map(int, r.split())
            lines.append((ax, ay, bx, by, col))
    return head[1], lw, lh, w, rect, lines


def model(lines, lw, lh, w, rect, eng):
    """The line-grid pixels tagpu_line.h's rule lights for `lines`: DrawLine's
    walk, thickened to w; with a rect, each line clipped first as 0x4BEA20 and
    0x4CC650 clip it and the pixels outside the rect dropped, as the pass's
    viewport scissor drops them."""
    lit = np.zeros((lh, lw), bool)
    for ax, ay, bx, by, _ in lines:
        if rect:
            c = eng.clip(ax, ay, bx, by, *rect, lw, lh)
            if c is None:
                continue
            ax, ay, bx, by = c
        walked = eng.walk(ax, ay, bx, by)
        for x, y in eng.thicken(walked, (ax, ay), (bx, by), w):
            if 0 <= x < lw and 0 <= y < lh:
                lit[y, x] = True
    if rect:
        L, T, R, B = rect
        keep = np.zeros_like(lit)
        keep[max(T, 0):B + 1, max(L, 0):R + 1] = True
        lit &= keep
    return lit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("lines")
    ap.add_argument("--diff", help="write the disagreement as a PNG: red = the "
                    "model lights it and the capture does not, green = the reverse")
    a = ap.parse_args()

    img = read_ppm(a.capture)
    th, tw = img.shape[:2]
    which, lw, lh, w, rect, lines = read_lines(a.lines)
    if (tw, th) != (lw, lh):
        raise SystemExit("line-oracle: the capture is %dx%d and the line grid %dx%d - "
                         "the capture must be the grid the ends were put on"
                         % (tw, th, lw, lh))
    want = model(lines, lw, lh, w, rect, _engine())
    got = img.any(axis=2)
    only_model = want & ~got
    only_cap = got & ~want
    print("line-oracle: %s, %d lines, w=%d: model %d px, capture %d px, "
          "only in the model %d, only in the capture %d"
          % (which, len(lines), w, int(want.sum()), int(got.sum()),
             int(only_model.sum()), int(only_cap.sum())))
    if a.diff:
        from PIL import Image
        out = np.zeros_like(img)
        out[want & got] = (110, 110, 110)
        out[only_model] = (255, 0, 0)
        out[only_cap] = (0, 255, 0)
        Image.fromarray(out).save(a.diff)
    return 0 if not only_model.any() and not only_cap.any() else 1


if __name__ == "__main__":
    sys.exit(main())
