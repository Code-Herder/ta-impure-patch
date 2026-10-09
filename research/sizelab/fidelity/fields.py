#!/usr/bin/env python3
"""fields.py -- the unit fields this study measures, cut out of extract.py's dense images.

    fields.py <datadir>     writes <datadir>/f_<name>.npy, each [T, S]

Byte offsets in a unit image (tagpu_recfmt.h: the unit record, then its mover at 0x118):
  x, alt, z     i32 16.16 at +0x6A, +0x6E, +0x72  (exe-reverse-engineering.md: the 16.16 position)
  tx, hd, tz    u16 at +0x64, +0x66, +0x68         (the turn triple; +0x66 the heading)
  vx, vy, vz    i32 at mover +0x08, +0x0C, +0x10   (velocity: the placement adds it, 0x43D7FF..)
  spd           i32 at mover +0x20                  (the speed the walk class reads, 0x43DA70)
  w24           i16 at mover +0x24
  claim         u32 at mover +0x26                  (the claim stamp, s_raw2 in tagpu_replay.c)
  moved         u32 at mover +0x2A                  (GameTime of the last move, 0x43D865)
  layer         u8  at mover +0x2E
  fx, fz        i16 at +0x76, +0x78                 (the footprint cell)
  sx, sz        i16 at +0x7A, +0x7C                 (the sight stamp's cell)
  sh            u8  at +0xF8                        (the sight stamp's height)
  bucket        u32 at +0x82                        (the sort bucket's ID)
  build         f32 at +0x104, hp u16 at +0x108, water u16 at +0x10A, state u32 at +0x110
  type          u16 at +0xA6, owner u8 at +0xFF
"""
import sys
from pathlib import Path

import numpy as np

F = {  # name: (offset, dtype)
    "x": (0x6A, "<i4"), "alt": (0x6E, "<i4"), "z": (0x72, "<i4"),
    "tx": (0x64, "<u2"), "hd": (0x66, "<u2"), "tz": (0x68, "<u2"),
    "vx": (0x118 + 0x08, "<i4"), "vy": (0x118 + 0x0C, "<i4"), "vz": (0x118 + 0x10, "<i4"),
    "spd": (0x118 + 0x20, "<i4"), "w24": (0x118 + 0x24, "<i2"),
    "claim": (0x118 + 0x26, "<u4"), "moved": (0x118 + 0x2A, "<u4"), "layer": (0x118 + 0x2E, "u1"),
    "fx": (0x76, "<i2"), "fz": (0x78, "<i2"), "sx": (0x7A, "<i2"), "sz": (0x7C, "<i2"),
    "fw": (0x7E, "<i2"), "fh": (0x80, "<i2"),
    "sh": (0xF8, "u1"), "bucket": (0x82, "<u4"),
    "build": (0x104, "<f4"), "hp": (0x108, "<u2"), "water": (0x10A, "<u2"),
    "state": (0x110, "<u4"), "type": (0xA6, "<u2"), "owner": (0xFF, "u1"),
}


def main():
    d = Path(sys.argv[1])
    u = np.load(d / "units.npy", mmap_mode="r")
    T, S, D = u.shape
    b = u.view(np.uint8).reshape(T, S, D * 4)
    for name, (off, dt) in F.items():
        n = np.dtype(dt).itemsize
        out = np.empty((T, S), dtype=dt)
        step = 512
        for t0 in range(0, T, step):
            out[t0:t0 + step] = np.ascontiguousarray(b[t0:t0 + step, :, off:off + n]).view(dt)[..., 0]
        np.save(d / f"f_{name}.npy", out)
        print(name, out.dtype, out.shape)


if __name__ == "__main__":
    main()
