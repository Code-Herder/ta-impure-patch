#!/usr/bin/env python3
"""The Vulkan lane against its GL twin, pixel for pixel (Phase G / G19d).

    tools/vk-ab.py <gamedir>                 # the fps pair in an instance
    tools/vk-ab.py <gamedir> --pass scaffold # another ported pass's pair
    tools/vk-ab.py a.ppm b.ppm [--out d.png] # two files

WHAT IT COMPARES, AND WHY IT REFUSES RATHER THAN SCALES. `tagpu_<pass>.ab` in an
instance's gamedir makes both backends capture ONE frame of that pass over a
black field -- the GL lane writes `tagpu_<pass>_gl.ppm` from `glReadPixels`, the
Vulkan lane writes `tagpu_<pass>_vk.ppm` out of the swapchain image it just
presented. Same inputs, same shader, same frame size, two rasterisers. So the
only honest verdict is "identical" or "not", and two captures of different sizes
are a setup fault (the fork letterboxing, or the window not matching the render
target) rather than something to resample: a scaled comparison cannot be 0 px by
construction, so it would turn a real mismatch into a plausible-looking number.

ARM ONE PASS'S `.ab` AT A TIME. Each GL capture holds one pass, because its twin
blacks the frame and reads back around its own draw; the Vulkan capture is one
frame and holds every armed pass at once. The lane refuses to capture at all
when more than one pass drew into the frame it was asked for, and says so in
tagpu.log, so a contaminated pair is not written rather than being written and
believed.

The exit status is 0 only when every pixel agrees -- including the pixels the
GL capture left black, because a pass that draws MORE than its twin differs
only there. `on GL ink` / `on GL black` split the difference up for reading;
neither of them softens the verdict.
"""

import argparse
import pathlib
import sys


BLACK = b"\x00\x00\x00"


def read_ppm(path):
    """A binary P6 with a decimal maxval of 255. Deliberately not a general
    reader: these files are written by two places in this repo and a surprise
    in one of them should be an error, not a silently different parse."""
    data = pathlib.Path(path).read_bytes()
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] not in (b"\n", b"\r"):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1                       # the single whitespace byte after maxval
    if fields[0] != b"P6" or fields[3] != b"255":
        raise SystemExit("%s: not a binary PPM with maxval 255" % path)
    w, h = int(fields[1]), int(fields[2])
    px = data[i:i + w * h * 3]
    if len(px) != w * h * 3:
        raise SystemExit("%s: %d pixel bytes, expected %d" % (path, len(px), w * h * 3))
    return w, h, px


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("args", nargs="+")
    ap.add_argument("--out", help="write a difference image (PPM) here")
    ap.add_argument("--pass", dest="which", default="fps",
                    help="which ported pass's pair to compare in a gamedir "
                         "(fps, scaffold, ...); default fps")
    a = ap.parse_args()

    lever = None
    if len(a.args) == 1:
        # THE GAMEDIR FORM IS DEAD, AND IT HAS TO SAY SO RATHER THAN SEND THE
        # OPERATOR LOOKING FOR A FILE NOTHING WRITES. It paired
        # `tagpu_<tag>_gl.ppm` with `tagpu_<tag>_vk.ppm` -- one capture from each
        # lane of a single frame, which was route D. The vulkan-only plan's
        # landing 4d-1 deleted route D and 4d-2 deleted the GL capture, so the
        # `_gl.ppm` can never appear again and the old refusal message ("did the
        # lever fire on both lanes?") names a lane that does not exist.
        # [FOUND BY THE 4d-2 LANDING REVIEW.]
        raise SystemExit("""vk-ab: the gamedir form is withdrawn.
  It diffed the GL lane's capture against the Vulkan lane's, and the
  vulkan-only plan's landings 4d-1/4d-2 deleted the GL half -- no
  tagpu_<pass>_gl.ppm is written any more, by anything.
  What replaced it: launch with renderer=vulkan, arm ONE pass's .ab (the seam
  refuses a frame that several passes drew into), and diff the _vk.ppm against
  one kept from an earlier BUILD:
      vk-ab.py <old>/tagpu_<pass>_vk.ppm <new>/tagpu_<pass>_vk.ppm
  The banked two-lane figures are in research/notes/gpu-status.md.""")
    elif len(a.args) == 2:
        gl, vk = pathlib.Path(a.args[0]), pathlib.Path(a.args[1])
    else:
        raise SystemExit("give a gamedir, or two .ppm files")

    for p in (gl, vk):
        if not p.exists():
            raise SystemExit("%s is not there -- did `tagpu_<pass>.ab` fire? "
                             "tagpu.log says: look for `vk: shot: wrote ...`, "
                             "and for the refusal lines above it." % p)

    # THE CAPTURES MUST BE NEWER THAN THE LEVER THAT ASKED FOR THEM.
    # Existence is not freshness: every way a capture silently does not fire
    # this run -- the `.ab` file not re-armed (`touch` on a file that is
    # already there does nothing), the lane down, `ss != 1`, the seam's
    # "N levers claimed this frame" refusal -- leaves the PREVIOUS run's PPMs
    # lying on the disk, and this tool would read them and print that run's
    # verdict for the binary in front of you. The whole landing's evidence is
    # this number, so it refuses instead.
    # [ADDED BY THE G19e RE-REVIEW, 2026-09-15.]
    if lever is not None and lever.exists():
        arm = lever.stat().st_mtime
        stale = [p for p in (gl, vk) if p.stat().st_mtime < arm - 1.0]
        if stale:
            raise SystemExit(
                "REFUSED: %s predate%s tagpu_%s.ab -- these are an EARLIER run's "
                "captures.\nThe lever did not fire this time (`touch` on a file "
                "that already exists does not re-arm).\nrm the .ab and the "
                "_gl/_vk .ppm files, sleep, then touch the .ab again."
                % (", ".join(p.name for p in stale),
                   "" if len(stale) > 1 else "s", a.which))

    gw, gh, gp = read_ppm(gl)
    vw, vh, vp = read_ppm(vk)
    print("GL     %s  %dx%d" % (gl, gw, gh))
    print("Vulkan %s  %dx%d" % (vk, vw, vh))
    if (gw, gh) != (vw, vh):
        print("\nREFUSED: the two captures are different sizes.")
        print("The GL capture is the render target's viewport and the Vulkan one is")
        print("the game window's client rect, so this means the fork is letterboxing")
        print("(a --window size the render target does not match, or k != 1).")
        print("Run both at a size where they agree; scaling one would make a 0 px")
        print("result impossible and a mismatch look plausible.")
        return 2

    n = gw * gh
    diff = 0
    worst = 0
    first = None
    ink_gl = ink_vk = 0
    # WHERE THE GL CAPTURE ACTUALLY DREW, counted separately -- see the report
    # below for why this is not a softer bar but a different question.
    diff_ink = 0
    first_ink = None
    for i in range(n):
        o = i * 3
        a3, b3 = gp[o:o + 3], vp[o:o + 3]
        if a3 != b3:
            diff += 1
            d = max(abs(a3[k] - b3[k]) for k in range(3))
            if d > worst:
                worst = d
            if first is None:
                first = (i % gw, i // gw, tuple(a3), tuple(b3))
            if a3 != BLACK:
                diff_ink += 1
                if first_ink is None:
                    first_ink = (i % gw, i // gw, tuple(a3), tuple(b3))
        # BYTES AGAINST BYTES. `a3` is a slice of the file, so comparing it to a
        # tuple is always unequal and every pixel counts as ink -- which made the
        # first run of this report 786432 non-black pixels on an image that has
        # 89, and would have hidden the "both captures are blank" case it exists
        # to catch.
        if a3 != BLACK:
            ink_gl += 1
        if b3 != BLACK:
            ink_vk += 1

    print("\nnon-black px   GL %d   Vulkan %d" % (ink_gl, ink_vk))
    print("differing px   %d of %d" % (diff, n))
    if diff:
        print("  on GL ink    %d of %d" % (diff_ink, ink_gl))
        print("  on GL black  %d" % (diff - diff_ink))
        print("worst channel  %d" % worst)
        print("first at       (%d, %d)  GL %s  Vulkan %s" % first)
        if first_ink:
            print("first on ink   (%d, %d)  GL %s  Vulkan %s" % first_ink)

    if a.out and diff:
        out = bytearray(b"P6\n%d %d\n255\n" % (gw, gh))
        for i in range(n):
            o = i * 3
            out += b"\xff\x00\x00" if gp[o:o + 3] != vp[o:o + 3] else vp[o:o + 3]
        pathlib.Path(a.out).write_bytes(bytes(out))
        print("difference     %s (red where they disagree)" % a.out)

    if diff == 0 and ink_gl == 0:
        print("\nBOTH CAPTURES ARE BLANK -- that is not a pass. Two empty frames")
        print("agree perfectly and prove nothing. Check that `tagpu_%s.on` is" % a.which)
        print("there, that the pass had something to draw on the captured frame,")
        print("and -- for the readout -- that `mark.on` is there too, because the")
        print("font reaches the render thread in the frame packet at hook 8 and")
        print("without it every string is refused and BOTH lanes draw nothing.")
        return 1
    print("\n%s" % ("0 px apart" if diff == 0 else "NOT identical"))
    # THE TWO CAPTURES ARE NOT THE SAME KIND OF PICTURE SINCE LANDING 4c-1, and
    # a reader who takes the headline number alone will conclude a world pass
    # regressed when it did not. The GL half of a world pass's A/B is the bare
    # world FBO -- black everywhere the pass did not draw -- while the Vulkan
    # half is the SWAPCHAIN IMAGE, which since 4c-1 carries TA's own 8-bit frame
    # underneath it (`tagpu_vk_surf.c`, the bottom layer that made
    # `tagpu_gui.off` show a game). So every pixel the pass did not cover
    # differs by construction, and the question that still has a yes/no answer
    # is the one about the pixels the GL capture actually contains.
    # MEASURED 2026-09-18 on the terrain pass: 118 751 px differ of 786 432, and
    # 0 of the 630 719 the GL FBO drew. That is a pass in exact agreement with
    # its twin and a harness comparing two different framings.
    # THE EXIT CODE NEVER SAYS "PASS" ON A RUN THAT DIFFERS, and the first cut
    # of this report did. It returned 0 whenever every difference fell on a
    # GL-black pixel -- which sounds like "the pass agrees" and is not, because
    # `diff_ink` is blind to the one failure mode this project has already
    # shipped once: a pass that draws MORE than its twin. G19e's line
    # rasterisation was "all 126 of the twin's pixels plus exactly one extra
    # fragment at the END of each line segment" (tagpu_vk_pass.h), and every one
    # of those extra fragments lands on a pixel GL left black. So does a terrain
    # or feature pass painting outside the scissor, or over fog its twin
    # refused. An over-draw would have exited 0 and printed the words "the pass
    # agrees with its twin".
    # The split is still worth printing -- it is what an over-draw looks like --
    # but it is a thing to READ, not a verdict.
    # [FROM THE 4c-2 LANDING REVIEW; the framing case it used to also cover went
    # with route D in 4d-1, so the reading below is narrower and firmer now.]
    if diff and diff_ink == 0:
        print("...but 0 of the %d pixels the FIRST capture DREW." % ink_gl)
        print("Every difference is a pixel the first one left black, i.e. the")
        print("second OVER-DRAWS it. On a cross-build diff that is a change in")
        print("what the pass covers -- a wider line, a looser scissor, fog it no")
        print("longer refuses. Look at the difference image (--out)." )
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
