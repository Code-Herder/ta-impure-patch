#!/usr/bin/env python3
"""Two captures of one Vulkan pass, pixel for pixel.

    tools/vk-ab.py a.ppm b.ppm [--out d.png] # two files
    tools/vk-ab.py <gamedir>                 # refused, and says what to do

WHAT IT COMPARES, AND WHY IT REFUSES RATHER THAN SCALES. `tagpu_<pass>.ab` in an
instance's gamedir makes the Vulkan lane capture ONE frame of that pass to
`tagpu_<pass>_vk.ppm`. The pair is two such captures of the same scene from two
BUILDS: same inputs, same frame size. So the only honest verdict is "identical"
or "not", and two captures of different sizes are a setup fault (the fork
letterboxing, or the window not matching the render target) rather than
something to resample: a scaled comparison cannot be 0 px by construction, so
it would turn a real mismatch into a plausible-looking number.

ARM ONE PASS'S `.ab` AT A TIME. The capture is one frame and holds every armed
pass at once, so the lane refuses to capture at all when more than one pass
drew into the frame it was asked for, and says so in tagpu.log -- a
contaminated capture is not written rather than being written and believed.

The exit status is 0 only when every pixel agrees -- including the pixels the
FIRST capture left black, because a build that draws MORE differs only there.
The report labels the first file `A` and the second `B`; `on A ink` /
`on A black` split the difference up by what the first file drew, for
reading, and neither of them softens the verdict.
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
                    help="the pass the captures are of, named in the "
                         "blank-capture hint (fps, scaffold, ...); default fps")
    a = ap.parse_args()

    if len(a.args) == 1:
        # THE GAMEDIR FORM IS REFUSED, AND SAYS SO RATHER THAN SEND THE
        # OPERATOR LOOKING FOR A FILE NOTHING WRITES: a gamedir holds one
        # `_vk.ppm` per pass, and the pair is two builds' captures.
        raise SystemExit("""vk-ab: give two .ppm files, not a gamedir.
  A gamedir holds one capture per pass, and the pair is two BUILDS' captures:
  launch with renderer=vulkan, arm ONE pass's .ab (the seam refuses a frame
  that several passes drew into), and diff the _vk.ppm against one kept from
  an earlier build:
      vk-ab.py <old>/tagpu_<pass>_vk.ppm <new>/tagpu_<pass>_vk.ppm""")
    elif len(a.args) == 2:
        pa, pb = pathlib.Path(a.args[0]), pathlib.Path(a.args[1])
    else:
        raise SystemExit("give two .ppm files")

    for p in (pa, pb):
        if not p.exists():
            raise SystemExit("%s is not there -- did `tagpu_<pass>.ab` fire? "
                             "tagpu.log says: look for `vk: shot: wrote ...`, "
                             "and for the refusal lines above it." % p)

    gw, gh, gp = read_ppm(pa)
    vw, vh, vp = read_ppm(pb)
    print("A  %s  %dx%d" % (pa, gw, gh))
    print("B  %s  %dx%d" % (pb, vw, vh))
    if (gw, gh) != (vw, vh):
        print("\nREFUSED: the two captures are different sizes.")
        print("The two runs did not capture the same frame size: a different")
        print("--window size or `ss`, or the fork letterboxing in one of them")
        print("(a window the render target does not match, or k != 1).")
        print("Re-run both at one size; scaling one would make a 0 px result")
        print("impossible and a mismatch look plausible.")
        return 2

    n = gw * gh
    diff = 0
    worst = 0
    first = None
    ink_a = ink_b = 0
    # WHERE THE FIRST CAPTURE ACTUALLY DREW, counted separately -- see the report
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
        # tuple would always be unequal and count every pixel as ink, which
        # would hide the "both captures are blank" case this exists to catch.
        if a3 != BLACK:
            ink_a += 1
        if b3 != BLACK:
            ink_b += 1

    print("\nnon-black px   A %d   B %d" % (ink_a, ink_b))
    print("differing px   %d of %d" % (diff, n))
    if diff:
        print("  on A ink     %d of %d" % (diff_ink, ink_a))
        print("  on A black   %d" % (diff - diff_ink))
        print("worst channel  %d" % worst)
        print("first at       (%d, %d)  A %s  B %s" % first)
        if first_ink:
            print("first on ink   (%d, %d)  A %s  B %s" % first_ink)

    if a.out and diff:
        out = bytearray(b"P6\n%d %d\n255\n" % (gw, gh))
        for i in range(n):
            o = i * 3
            out += b"\xff\x00\x00" if gp[o:o + 3] != vp[o:o + 3] else vp[o:o + 3]
        pathlib.Path(a.out).write_bytes(bytes(out))
        print("difference     %s (red where they disagree)" % a.out)

    if diff == 0 and ink_a == 0:
        print("\nBOTH CAPTURES ARE BLANK -- that is not a pass. Two empty frames")
        print("agree perfectly and prove nothing. Check that `tagpu_%s.on` is" % a.which)
        print("there, that the pass had something to draw on the captured frame,")
        print("and -- for the readout -- that `mark.on` is there too, because the")
        print("font reaches the render thread in the frame packet at hook 8 and")
        print("without it every string is refused and BOTH builds draw nothing.")
        return 1
    print("\n%s" % ("0 px apart" if diff == 0 else "NOT identical"))
    # THE EXIT CODE NEVER SAYS "PASS" ON A RUN THAT DIFFERS. A run whose every
    # difference falls on a pixel the first capture left black sounds like
    # "the pass agrees" and is not: `diff_ink` is blind to a pass that draws
    # MORE than the first capture did -- a line rasterisation that adds one
    # fragment at the END of each segment (tagpu_vk_pass.h), a terrain or
    # feature pass painting outside the scissor, or over fog the first refused.
    # Every one of those lands on a pixel the first capture left black.
    # The split is still worth printing -- it is what an over-draw looks like --
    # but it is a thing to READ, not a verdict.
    if diff and diff_ink == 0:
        print("...but 0 of the %d pixels the FIRST capture DREW." % ink_a)
        print("Every difference is a pixel the first one left black, i.e. the")
        print("second OVER-DRAWS it. On a cross-build diff that is a change in")
        print("what the pass covers -- a wider line, a looser scissor, fog it no")
        print("longer refuses. Look at the difference image (--out)." )
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
