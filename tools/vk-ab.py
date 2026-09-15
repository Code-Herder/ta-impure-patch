#!/usr/bin/env python3
"""The Vulkan lane against its GL twin, pixel for pixel (Phase G / G19d).

    tools/vk-ab.py <gamedir>                 # the two captures in an instance
    tools/vk-ab.py a.ppm b.ppm [--out d.png] # two files

WHAT IT COMPARES, AND WHY IT REFUSES RATHER THAN SCALES. `tagpu_fps.ab` in an
instance's gamedir makes both backends capture ONE frame of the frame-rate
readout over a black field -- the GL lane writes `tagpu_fps_gl.ppm` from
`glReadPixels`, the Vulkan lane writes `tagpu_fps_vk.ppm` out of the swapchain
image it just presented. Same quads, same atlas bytes, same shader, same frame
size, two rasterisers. So the only honest verdict is "identical" or "not", and
two captures of different sizes are a setup fault (the fork letterboxing, or the
window not matching the render target) rather than something to resample: a
scaled comparison cannot be 0 px by construction, so it would turn a real
mismatch into a plausible-looking number.

The exit status is 0 only when every pixel agrees.
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
    a = ap.parse_args()

    if len(a.args) == 1:
        d = pathlib.Path(a.args[0])
        gl, vk = d / "tagpu_fps_gl.ppm", d / "tagpu_fps_vk.ppm"
    elif len(a.args) == 2:
        gl, vk = pathlib.Path(a.args[0]), pathlib.Path(a.args[1])
    else:
        raise SystemExit("give a gamedir, or two .ppm files")

    for p in (gl, vk):
        if not p.exists():
            raise SystemExit("%s is not there -- did `tagpu_fps.ab` fire on both "
                             "lanes? tagpu.log says." % p)

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
        print("worst channel  %d" % worst)
        print("first at       (%d, %d)  GL %s  Vulkan %s" % first)

    if a.out and diff:
        out = bytearray(b"P6\n%d %d\n255\n" % (gw, gh))
        for i in range(n):
            o = i * 3
            out += b"\xff\x00\x00" if gp[o:o + 3] != vp[o:o + 3] else vp[o:o + 3]
        pathlib.Path(a.out).write_bytes(bytes(out))
        print("difference     %s (red where they disagree)" % a.out)

    if diff == 0 and ink_gl == 0:
        print("\nBOTH CAPTURES ARE BLANK -- that is not a pass. The readout needs")
        print("`tagpu_fps.on` and a couple of seconds for its first averaging")
        print("window before either lane has a quad to draw.")
        return 1
    print("\n%s" % ("0 px apart" if diff == 0 else "NOT identical"))
    return 0 if diff == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
