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

    TWO CRITERIA, and the second one is the one that matters away from 1x.

    `alternation` is the frame-to-frame zig-zag of (box - bar); stock's own ~0.66 px is the
    measurement floor and the sim-rate defect read 1.03 px, so at or below the floor is a
    pass FOR THAT DEFECT.  It is a weak instrument at high zoom: the box is a rotated
    outline whose centroid breathes ~11 px at 4x, and against that the pre-zoom
    quantisation defect moved it only 2.79 -> 2.27.

    `|2nd diff|` is the bar measured against ITSELF and has no such blind spot.  A unit
    walks at a constant speed, so a bar that tracks it has a second difference near zero,
    while a bar quantised on a grid of `q` displayed px stands still and then teleports
    `q`.  At 4x the two builds read 2.09 px mean / 4.00 p99 against 0.42 / 1.00, and the
    step histogram is the picture: BEFORE, every step is exactly 0, 4.00 or 8.00 px and
    nothing between; AFTER, a continuous 1.1-3.2 px tracking the unit's real speed.

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
GREEN = np.array([93, 250, 88])          # the GUI green of both the bar and the box, at
                                         # Gamma 10 -- but see calibrate_green()
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


def calibrate_green(path, nframes=12):
    """The GUI green AS THIS CAPTURE HAS IT, not as a constant.

    `Gamma` is one shared mutable registry inode across every instance on the machine
    (memory: the G17a landing), so another session moving it repaints our palette: these
    legs came back with the bar at (83,223,79) where the constant here says (93,250,88),
    and an exact-colour match then found ZERO pixels in every frame and reported "no
    usable run" for both builds -- a silent total miss, not a noisy one.  The GUI green is
    the most saturated green in the frame by a wide margin (its g - max(r,b) is ~140
    against the terrain's ~72), so take the modal colour above a threshold in between."""
    from collections import Counter
    c = Counter()
    for i, fr in enumerate(frames(path)):
        if i >= nframes:
            break
        a = fr.astype(np.int16)
        sat = a[:, :, 1] - np.maximum(a[:, :, 0], a[:, :, 2])
        yy, xx = np.nonzero(sat >= 100)
        c.update(map(tuple, fr[yy, xx]))
    return np.array(c.most_common(1)[0][0]) if c else GREEN


def measure(frame, z=1.0, green=GREEN):
    """-> (bar_x, bar_y, box_x, box_y) or None if the bar is not on screen.

    EVERY LENGTH HERE SCALES WITH THE ZOOM.  The bar is world-sized, not constant screen
    size, so at 4x its green run is 132 px and not 33.

    THE BAR IS FOUND BY ITS THICKNESS, NOT BY BEING THE LONGEST RUN.  It is a filled block
    3 game px tall (so 3*z on screen); the selection box is a 1-px GL_LINES outline drawn
    at 1x whatever the zoom.  At 1x the bar is also the longest run and the two rules
    agree, but by 4x the box's own edges are ~240 px against the bar's 132 and "longest
    run" locks onto the BOX -- which reads as a bar that leaps tens of pixels a frame.  So
    erode vertically first: `m[:-1] & m[1:]` deletes every 1-px-tall horizontal line and
    reduces the box's vertical edges to runs of length 1, and what survives at any zoom is
    the bar."""
    sub = frame[VP[1]:VP[3], VP[0]:VP[2]].astype(np.int16)
    m = (np.abs(sub - green).max(axis=2) <= 10)
    thick = m[:-1] & m[1:]
    minrun = max(8, int(round(25 * z)))
    rows = np.nonzero(thick.sum(axis=1) >= minrun)[0]
    if len(rows) == 0:
        return None
    # the bar: the thick row whose longest run is minrun+, and its run
    best = None
    for y in rows:
        r = np.nonzero(thick[y])[0]
        br = np.split(r, np.nonzero(np.diff(r) != 1)[0] + 1)
        for b in br:
            if len(b) >= minrun and (best is None or len(b) > best[2]):
                best = (y, b, len(b))
    if best is None:
        return None
    ys, run, _ = best
    rows = np.nonzero(m.sum(axis=1) >= minrun)[0]
    # Anchor on the RIGHT edge, not the run centre.  Under ss=2 the downsample puts an
    # antialiased 34th column on the LEFT of the bar on transition frames, which moves a
    # centre-based anchor by half a pixel and destroys the step statistics (it read our
    # bar as moving every frame when the engine's own log shows it staircasing).  The
    # right edge is clean in both builds; the bar is a constant 33 px at full health.
    bx = run[-1] - 16.0 * z
    # every row of the bar block, to get its vertical centre
    barrows = [y for y in rows if abs(y - ys) <= 3 * z]
    by = float(np.mean(barrows))

    # the box: green within a window around the bar, minus the bar's own rows
    win_r = int(round(70 * z))
    y0, y1 = max(0, int(by) - win_r), min(m.shape[0], int(by) + win_r)
    x0, x1 = max(0, int(bx) - win_r), min(m.shape[1], int(bx) + win_r)
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


ZTAG = re.compile(r"(?:zoom|z)(\d+(?:\.\d+)?)|(\d+(?:\.\d+)?)x")


def zoom_of(tag):
    """The leg's zoom, from its tag: `zoom4`, `z4`, `4x`, `2.5x` -- else 1x.  The whole
    artifact scales with this and so does every length in measure(), so a mislabelled leg
    is a wrong answer rather than a noisy one."""
    m = ZTAG.search(tag)
    return float(m.group(1) or m.group(2)) if m else 1.0


def zcx_of(logpath):
    """The zoom centre from the run's own `mark: ... vp=(L,T,WxH)` line."""
    m = None
    for line in open(logpath, errors="ignore"):
        g = re.search(r"mark: .*vp=\((\d+),(\d+) (\d+)x(\d+)\)", line)
        if g:
            m = g
    return float(m.group(1)) + float(m.group(3)) / 2.0 if m else None


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


def video_leg(path, z=1.0):
    green = calibrate_green(path)
    bx, by, sx, sy = [], [], [], []
    for fr in frames(path):
        r = measure(fr, z, green)
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
    # THE BAR AGAINST ITSELF, which is the statistic that catches a quantised anchor.
    # The unit walks at a constant speed, so a bar that tracks it has a near-constant
    # step and a second difference near zero; a bar quantised on a grid of `q` px stands
    # still and then teleports `q`, which is a second difference of `q`.  Referencing the
    # BOX instead nearly missed the 4x defect (its own rotated outline breathes ~11 px at
    # that zoom, swamping the term), so this is reported beside it and not in place of it.
    jerk = np.abs(np.diff(bx, 2))
    step = np.hypot(np.diff(bx), np.diff(by))
    return dict(n=int(mv.sum()), blocks=len(blocks), alt=alt,
                slow=float(detrend((sx - bx)[mv]).std()),
                jerk=float(jerk.mean()), jerk99=float(np.percentile(jerk, 99)),
                step_med=float(np.median(step)), step_max=float(step.max()),
                bar_still=float((np.abs(np.diff(bx))[mv[1:]] < 1e-6).mean()),
                box_still=float((np.abs(np.diff(sx))[mv[1:]] < 1e-6).mean()))


def log_leg(path, zoom, ss=2, zcx=None):
    """The three anchoring rules the bar has had, from ONE run of the current build.

    `ax` is the anchor the BODY was drawn at, in game-frame (pre-zoom) units; the eye
    term in it is a whole number, so a separation is just the bar's rule applied to `ax`
    minus `ax` itself.  Reported in DISPLAYED SCREEN PIXELS, which is what the owner sees:
    one game-frame unit becomes `zoom` of them.

      shorts   the original defect -- the engine's s16, i.e. the SIM rate against a body
               at present rate.  Unbounded in principle: it is the distance the unit
               travels in a sim step, so it grows with unit speed and with `gamespeed`.
      floored  fix 1 (2026-09-09, superseded the same day).  The body's own anchor, but
               quantised in PRE-zoom units, so the step on screen is `zoom` px -- 4 px at
               4x, 8 px at the 8x ZOOM_MAX.  This is the residual the owner then reported
               as a diagonal twitch at max zoom-in.
      snapped  fix 2, what ships.  `snap_device`: forward through the zoom, round onto the
               1/ss grid, back.  The step is 1/ss of a displayed pixel at EVERY zoom, so
               the bound does not grow with the zoom at all.

    `zcx` is the zoom centre (`vpL + vw/2`); the rounding is not shift-invariant, so it is
    read from the run's own `mark:` line rather than assumed."""
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
    if zcx is None:
        zcx = 128 + 1792 / 2.0
    # every separation in game-frame units, then scaled to displayed pixels below
    shorts = detrend((ix >> 16) - ax)[mv]
    floored = (np.floor(ax) - ax)[mv]
    g = (ax - zcx) * zoom + zcx                       # forward through the zoom
    snapped = ((np.floor(g * ss + 0.5) / ss - zcx) / zoom + zcx - ax)[mv]
    changed = (np.diff(ix) != 0) | (np.diff(iy) != 0)
    out = dict(n=int(mv.sum()), zoom=zoom, ss=ss,
               sim=float(changed[mv[1:]].mean() * 60.0))
    for k, v in (("shorts", shorts), ("floored", floored), ("snapped", snapped)):
        out[k + "_rms"] = v.std() * zoom
        out[k + "_p2p"] = (v.max() - v.min()) * zoom
    return out


def main(argv):
    d = argv[1] if len(argv) > 1 else "/tmp/bw"
    vids = argv[2:] or sorted(glob.glob(os.path.join(d, "walk-*.mkv")))
    print("VIDEO -- pass = 30 Hz alternation at or below stock's ~0.66 px floor "
          "(the defect read 1.03); the still-fractions are diagnostic, see the docstring")
    for v in vids:
        tag = os.path.basename(v)[5:-4]
        r = video_leg(v, zoom_of(tag))
        if not r:
            print("  %-16s no usable run" % tag); continue
        print("  %-16s n=%4d in %d run(s) | bar still %5.1f %% | box still %5.1f %% | "
              "30 Hz alternation %5.3f px (slow term %.3f)"
              % (tag, r["n"], r["blocks"], 100 * r["bar_still"], 100 * r["box_still"],
                 r["alt"], r["slow"]))
        print("  %-16s   bar vs itself: step median %.2f max %.2f px | "
              "|2nd diff| mean %.2f p99 %.2f px"
              % ("", r["step_med"], r["step_max"], r["jerk"], r["jerk99"]))
    print("\nLOG -- exact, our build only; ONE run reports the bar before and after the fix")
    for v in vids:
        tag = os.path.basename(v)[5:-4]
        lg = os.path.join(os.path.dirname(v), "log-%s.txt" % tag)
        if not os.path.exists(lg):
            continue
        r = log_leg(lg, zoom_of(tag), zcx=zcx_of(lg))
        if not r:
            print("  %-16s no spx samples (unit not selected, or spxlog.on absent)" % tag); continue
        print("  %-16s n=%4d  sim %4.1f /s  zoom %.2fx |  shorts %.2f rms %.2f p2p"
              " |  floored %.2f rms %.2f p2p  |  snapped %.2f rms %.2f p2p   (displayed px)"
              % (tag, r["n"], r["sim"], r["zoom"],
                 r["shorts_rms"], r["shorts_p2p"], r["floored_rms"], r["floored_p2p"],
                 r["snapped_rms"], r["snapped_p2p"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
