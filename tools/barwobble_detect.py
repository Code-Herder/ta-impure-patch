#!/usr/bin/env python3
"""The health-bar wobble oracle -- both halves, over a directory of tools/barwobble.sh legs.

    .venv-undither/bin/python tools/barwobble_detect.py /tmp/bw [walk-<tag>.mkv ...]

THE DEFECT IT REGRESSES (fixed 2026-09-09, roadmap "Awaiting review", gpu-status §2.2).
Everything anchored to a unit in our build takes the unit pass's interpolated sub-pixel
anchor -- the body, the selection rect, the unit-anchored order markers.  tagpu_mark.c's
health bar and group digit did not: they read the engine's integer world shorts, which
pinned them to the SIM rate while the body glided at PRESENT rate.  The two then slid
apart by up to a whole sim step of motion, times the zoom on screen.  Stock TA cannot
show it, because there the bar and the body are the same shorts.

TWO INSTRUMENTS, because neither alone covers both builds:

  * the LOG half (`tagpu_spxlog.on`, exact, our build only) -- `spx: f= fix=(ix,iy)
    a=(ax,ay)` is logged once per PRESENT frame for every selected unit, giving the raw
    16.16 position and the anchor the body was actually drawn at.  From those two the
    bar-vs-body separation is arithmetic, with no pixels involved: BEFORE the fix the bar
    sat at floor(ix/65536); AFTER it sits at floor(the body's own anchor), and since the
    eye term is a whole number that is simply -frac(ax).  So ONE run of the current build
    reports both, which is what makes this a regression rather than a snapshot.

  * the VIDEO half (works on any build, stock included) -- the bar is a solid 33x3 block
    and the selection box a thin rotated outline in the same GUI green, so they separate
    geometrically.

    THE PASS CRITERION IS THE ALTERNATION, not the still-fractions.  `alternation` is the
    frame-to-frame zig-zag of (box - bar); stock's own ~0.66 px is the measurement floor,
    and the defect read 1.03 px.  At or below the floor is a pass.

    The two still-fractions are diagnostic only, and neither is a bar to clear:
      - `box still` says which BUILD you are looking at.  ~51 % is a sim-rate body (stock,
        or tagpu_subpix.off); ~2 % is the interpolated body, and in our build that is
        sub-pixel motion working, not a fault.
      - `bar still` is only the floor quantisation, so it tracks how far the unit moves per
        frame and says nothing on its own.  After the fix the same build reads 21 % at
        gamespeed 20 and 59 % at gamespeed 10 -- both correct.  Before the fix it was pinned
        near 54 % at any speed, because it was the SIM rate rather than a floor.

THREE THINGS THAT WILL FOOL YOU, all paid for once:

  * Anchor the bar on its RIGHT edge, not the run centre.  Under ss=2 the downsample adds
    an antialiased 34th column on the LEFT on transition frames; a centre anchor then
    moves half a pixel and reads our bar as moving every frame when the log shows it
    staircasing.
  * Never let samples either side of a dropped frame become adjacent.  The selection box
    can vanish while the bar is fine (a stationary unit in stock stops matching pure
    green), and treating the survivors as contiguous fabricates jumps.  Hence contiguous().
  * The slow term is not the wobble.  The box is a ROTATED outline whose shape breathes
    with the unit's bank and pitch over undulating ground; that reads ~3.9 px rms in every
    leg, stock included.  The wobble alternates frame to frame, so the second difference
    is what isolates it.
"""
import os, subprocess, sys, re, glob
import numpy as np

W, H = 1920, 1080
GREEN = np.array([93, 250, 88])          # the GUI green of both the bar and the box
VP = (128, 32, 1920, 1048)               # world viewport: neither ever appears outside it
SPX = re.compile(r"spx: f=(\d+) fix=\((-?\d+),(-?\d+)\) a=\((-?[\d.]+),(-?[\d.]+)\)")
def frames(path):
    p = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        stdout=subprocess.PIPE)
    n = W * H * 3
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        yield np.frombuffer(buf, np.uint8).reshape(H, W, 3)
    p.stdout.close(); p.wait()


def measure(frame):
    """-> (bar_x, bar_y, box_x, box_y) or None if the bar is not on screen."""
    sub = frame[VP[1]:VP[3], VP[0]:VP[2]].astype(np.int16)
    m = (np.abs(sub - GREEN).max(axis=2) <= 10)
    rows = np.nonzero(m.sum(axis=1) >= 25)[0]
    if len(rows) == 0:
        return None
    # the bar: the row block whose longest run is 25+, and its run
    best = None
    for y in rows:
        r = np.nonzero(m[y])[0]
        br = np.split(r, np.nonzero(np.diff(r) != 1)[0] + 1)
        for b in br:
            if len(b) >= 25 and (best is None or len(b) > best[2]):
                best = (y, b, len(b))
    if best is None:
        return None
    ys, run, _ = best
    # Anchor on the RIGHT edge, not the run centre.  Under ss=2 the downsample puts an
    # antialiased 34th column on the LEFT of the bar on transition frames, which moves a
    # centre-based anchor by half a pixel and destroys the step statistics (it read our
    # bar as moving every frame when the engine's own log shows it staircasing).  The
    # right edge is clean in both builds; the bar is a constant 33 px at full health.
    bx = run[-1] - 16.0
    # every row of the bar block, to get its vertical centre
    barrows = [y for y in rows if abs(y - ys) <= 3]
    by = float(np.mean(barrows))

    # the box: green within a window around the bar, minus the bar's own rows
    y0, y1 = max(0, int(by) - 70), min(m.shape[0], int(by) + 70)
    x0, x1 = max(0, int(bx) - 70), min(m.shape[1], int(bx) + 70)
    win = m[y0:y1, x0:x1].copy()
    for y in barrows:
        if y0 <= y < y1:
            win[y - y0] = False
    yy, xx = np.nonzero(win)
    # The BOX may be absent while the bar is fine (the unit can lose its selection), so
    # report the bar regardless and mark the box missing -- returning None for the whole
    # frame silently dropped 523 of 1320 stock frames and made the retained ones look
    # contiguous when they were not.
    if len(yy) < 20:
        return bx + VP[0], by + VP[1], np.nan, np.nan
    return bx + VP[0], by + VP[1], xx.mean() + x0 + VP[0], yy.mean() + y0 + VP[1]


def measure(frame):
    """-> (bar_x, bar_y, box_x, box_y) or None if the bar is not on screen."""
    sub = frame[VP[1]:VP[3], VP[0]:VP[2]].astype(np.int16)
    m = (np.abs(sub - GREEN).max(axis=2) <= 10)
    rows = np.nonzero(m.sum(axis=1) >= 25)[0]
    if len(rows) == 0:
        return None
    # the bar: the row block whose longest run is 25+, and its run
    best = None
    for y in rows:
        r = np.nonzero(m[y])[0]
        br = np.split(r, np.nonzero(np.diff(r) != 1)[0] + 1)
        for b in br:
            if len(b) >= 25 and (best is None or len(b) > best[2]):
                best = (y, b, len(b))
    if best is None:
        return None
    ys, run, _ = best
    # Anchor on the RIGHT edge, not the run centre.  Under ss=2 the downsample puts an
    # antialiased 34th column on the LEFT of the bar on transition frames, which moves a
    # centre-based anchor by half a pixel and destroys the step statistics (it read our
    # bar as moving every frame when the engine's own log shows it staircasing).  The
    # right edge is clean in both builds; the bar is a constant 33 px at full health.
    bx = run[-1] - 16.0
    # every row of the bar block, to get its vertical centre
    barrows = [y for y in rows if abs(y - ys) <= 3]
    by = float(np.mean(barrows))

    # the box: green within a window around the bar, minus the bar's own rows
    y0, y1 = max(0, int(by) - 70), min(m.shape[0], int(by) + 70)
    x0, x1 = max(0, int(bx) - 70), min(m.shape[1], int(bx) + 70)
    win = m[y0:y1, x0:x1].copy()
    for y in barrows:
        if y0 <= y < y1:
            win[y - y0] = False
    yy, xx = np.nonzero(win)
    # The BOX may be absent while the bar is fine (the unit can lose its selection), so
    # report the bar regardless and mark the box missing -- returning None for the whole
    # frame silently dropped 523 of 1320 stock frames and made the retained ones look
    # contiguous when they were not.
    if len(yy) < 20:
        return bx + VP[0], by + VP[1], np.nan, np.nan
    return bx + VP[0], by + VP[1], xx.mean() + x0 + VP[0], yy.mean() + y0 + VP[1]


def detrend(v, w=31):
    k = np.ones(w) / w
    pad = np.pad(v, (w // 2, w // 2), mode="edge")
    return v - np.convolve(pad, k, mode="valid")[:len(v)]


def contiguous(mask, minlen=40):
    """Index blocks where mask holds for at least minlen consecutive FRAMES.  Every
    statistic here is a frame-to-frame difference, so samples either side of a gap must
    never be adjacent in the array."""
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]:
                j += 1
            if j - i >= minlen:
                out.append(np.arange(i, j))
            i = j
        else:
            i += 1
    return out


def video_leg(path):
    bx, by, sx, sy = [], [], [], []
    for fr in frames(path):
        r = measure(fr)
        v = r if r else (np.nan,) * 4
        bx.append(v[0]); by.append(v[1]); sx.append(v[2]); sy.append(v[3])
    bx, by, sx, sy = map(np.array, (bx, by, sx, sy))
    blocks = contiguous(~np.isnan(bx) & ~np.isnan(sx))
    if not blocks:
        return None
    keep = np.concatenate(blocks)
    edges = np.zeros(len(bx), bool)
    for b in blocks:
        edges[b[0]] = True
    bx, by, sx, sy = bx[keep], by[keep], sx[keep], sy[keep]
    edge = edges[keep]
    moving = np.abs(np.diff(bx, prepend=bx[0])) + np.abs(np.diff(by, prepend=by[0])) > 0
    mv = (np.convolve(moving.astype(float), np.ones(9), mode="same") > 0) & ~edge
    if mv.sum() < 120:
        return None
    d = (sx - bx)[mv]
    alt = float(np.abs(d[1:-1] - 0.5 * (d[:-2] + d[2:])).mean())
    return dict(n=int(mv.sum()), blocks=len(blocks), alt=alt,
                slow=float(detrend((sx - bx)[mv]).std()),
                bar_still=float((np.abs(np.diff(bx))[mv[1:]] < 1e-6).mean()),
                box_still=float((np.abs(np.diff(sx))[mv[1:]] < 1e-6).mean()))


def log_leg(path, zoom):
    f = ix = None
    a = [], [], [], [], []
    rows = [[] for _ in range(5)]
    for line in open(path, errors="ignore"):
        m = SPX.search(line)
        if m:
            for k in range(5):
                rows[k].append(float(m.group(k + 1)))
    if len(rows[0]) < 120:
        return None
    f, ix, iy, ax, ay = (np.array(r) for r in rows)
    ix = ix.astype(np.int64); iy = iy.astype(np.int64)
    step = np.abs(np.diff(ix)) + np.abs(np.diff(iy))
    mv = np.convolve(np.concatenate([[False], step > 0]).astype(float),
                     np.ones(5), mode="same") > 0
    if mv.sum() < 120:
        return None
    before = detrend((ix >> 16) - ax)[mv]        # the bar as it was: the integer shorts
    after = (np.floor(ax) - ax)[mv]              # the bar as it is: floor of the body's anchor
    changed = (np.diff(ix) != 0) | (np.diff(iy) != 0)
    return dict(n=int(mv.sum()), zoom=zoom,
                sim=float(changed[mv[1:]].mean() * 60.0),
                before_rms=before.std() * zoom, before_p2p=(before.max() - before.min()) * zoom,
                after_rms=after.std() * zoom, after_p2p=(after.max() - after.min()) * zoom)


def main(argv):
    d = argv[1] if len(argv) > 1 else "/tmp/bw"
    vids = argv[2:] or sorted(glob.glob(os.path.join(d, "walk-*.mkv")))
    print("VIDEO -- pass = 30 Hz alternation at or below stock's ~0.66 px floor "
          "(the defect read 1.03); the still-fractions are diagnostic, see the docstring")
    for v in vids:
        tag = os.path.basename(v)[5:-4]
        r = video_leg(v)
        if not r:
            print("  %-16s no usable run" % tag); continue
        print("  %-16s n=%4d in %d run(s) | bar still %5.1f %% | box still %5.1f %% | "
              "30 Hz alternation %5.3f px (slow term %.3f)"
              % (tag, r["n"], r["blocks"], 100 * r["bar_still"], 100 * r["box_still"],
                 r["alt"], r["slow"]))
    print("\nLOG -- exact, our build only; ONE run reports the bar before and after the fix")
    for v in vids:
        tag = os.path.basename(v)[5:-4]
        lg = os.path.join(os.path.dirname(v), "log-%s.txt" % tag)
        if not os.path.exists(lg):
            continue
        zoom = 2.0 if "zoom2" in tag or "2x" in tag else 1.0
        r = log_leg(lg, zoom)
        if not r:
            print("  %-16s no spx samples (unit not selected, or spxlog.on absent)" % tag); continue
        print("  %-16s n=%4d  sim %4.1f /s  zoom %.0fx | shorts-anchored %.3f rms %.3f p2p"
              "  ->  body-anchored %.3f rms %.3f p2p"
              % (tag, r["n"], r["sim"], r["zoom"], r["before_rms"], r["before_p2p"],
                 r["after_rms"], r["after_p2p"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
