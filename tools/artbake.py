#!/usr/bin/env python3
"""artbake.py — bake Impure's own UI art (art/ui/) into the DLL.

    python3 tools/artbake.py <art/ui> <ta-palette.json> <out.h>
    python3 tools/artbake.py --dump <art/ui> <ta-palette.json>     # what the DLL will carry

THE STANDARD LIBRARY ONLY. The build's Python is the bare `python3` CI installs
(.github/workflows/build.yml), so the PNG reader below is ours: 8-bit RGB, RGBA or
indexed, not interlaced -- what every image editor and Pillow write by default.

THE LAYOUT OF art/ui/. One folder per style (`retail`, `escalation`, `twilight`);
in it, `<entry>.png` is a one-frame entry and `<entry>/<frame>.png` a many-frame
one. A frame whose name is one character (a font's `A.png`, `0.png`) carries that
character's code; any other frame name sorts and carries its position.

WHAT IS REFUSED, because the DLL draws these frames in palette indices and every
known palette must show them the same:
  - a colour that is not exactly an entry of TA's palette;
  - a colour whose palette entry a mod changes: Total Mayhem's palette differs from
    retail's at 11-15, 144-159 and 240-244, ProTA's at 11-15 and 240-244
    (research/notes/tadr-port/demo-recorder-s5-ui.md, *Drawing it*), and 10-15 are
    black duplicates of 0;
  - partial alpha: a pixel is drawn or it is not.
A colour TA's palette holds twice maps to its first index outside that set.

WHAT IS WRITTEN: one zlib stream (the DLL inflates it with lodepng) of
    "TART" | u32 version | u32 entries
    per entry:  char style[16] | char name[32] | u16 frames | u16 0
    per frame:  u16 code | u16 w | u16 h | i16 hotx | i16 hoty | u8 key | u8 0 | w*h indices
`key` is the frame's transparent index -- 9 unless the frame draws 9, else the
first index it does not draw. A font frame's hotspot is its baseline, the bottom
row, as in the engine's GAF fonts (tools/guifont.py). """
import json
import struct
import sys
import zlib
from pathlib import Path

VERSION = 1
VARIES = set(range(10, 16)) | set(range(144, 160)) | set(range(240, 245))
STYLES = ("retail", "escalation", "twilight")


def fail(msg):
    sys.exit(f"artbake: {msg}")


def read_png(path):
    """(w, h, rows of (r, g, b, a) tuples) from an 8-bit, non-interlaced PNG."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path}: not a PNG")
    at, idat, plte, trns = 8, bytearray(), None, b""
    w = h = ctype = None
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        at += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, lace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype not in (2, 3, 6) or lace:
                fail(f"{path}: only 8-bit RGB, RGBA or indexed, not interlaced")
        elif kind == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"tRNS":
            trns = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    bpp = {2: 3, 3: 1, 6: 4}[ctype]
    raw = zlib.decompress(bytes(idat))
    stride = w * bpp
    prev = bytearray(stride)
    rows = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            elif f != 0:
                fail(f"{path}: bad filter {f}")
        prev = cur
        if ctype == 2:
            rows.append([(cur[i], cur[i + 1], cur[i + 2], 255) for i in range(0, stride, 3)])
        elif ctype == 6:
            rows.append([tuple(cur[i:i + 4]) for i in range(0, stride, 4)])
        else:
            rows.append([plte[v] + ((trns[v] if v < len(trns) else 255),) for v in cur])
    return w, h, rows


def palette_map(pal_path):
    pal = [tuple(c[:3]) for c in json.loads(Path(pal_path).read_text())]
    if len(pal) != 256:
        fail(f"{pal_path}: {len(pal)} entries, not 256")
    first = {}
    for i, c in enumerate(pal):
        first.setdefault(c, i)
    out = {}
    for i, c in enumerate(pal):
        if i not in VARIES and c not in out:
            out[c] = i
    # a colour only a varying entry holds would change colour in a mod
    return out, first


def bake_frame(path, code, cmap, first):
    w, h, rows = read_png(path)
    idx = bytearray()
    used = set()
    for y, row in enumerate(rows):
        for x, (r, g, b, a) in enumerate(row):
            if a == 0:
                idx.append(0)           # rewritten with the key below
                continue
            if a != 255:
                fail(f"{path}: partial alpha {a} at ({x},{y})")
            c = (r, g, b)
            if c not in cmap:
                why = (f"palette index {first[c]}, which a mod changes" if c in first
                       else "not a colour of TA's palette")
                fail(f"{path}: colour {c} at ({x},{y}) is {why}")
            idx.append(cmap[c])
            used.add(cmap[c])
    key = 9 if 9 not in used else next(i for i in range(256) if i not in used)
    mask = [a == 0 for row in rows for (_, _, _, a) in row]
    for i, m in enumerate(mask):
        if m:
            idx[i] = key
    hotx, hoty = 0, (h - 1 if len(code[1]) == 1 else 0)
    return struct.pack("<HHHhhBB", code[0], w, h, hotx, hoty, key, 0) + bytes(idx)


def bake(root, pal_path):
    root = Path(root)
    cmap, first = palette_map(pal_path)
    entries = []
    for style in sorted(p for p in root.iterdir() if p.is_dir()):
        if style.name not in STYLES:
            fail(f"{style}: not a style ({', '.join(STYLES)})")
        for item in sorted(style.iterdir()):
            if item.is_dir():
                files = sorted(item.glob("*.png"))
                if not files:
                    fail(f"{item}: no frames")
                frames = [bake_frame(f, (ord(f.stem) if len(f.stem) == 1 else n, f.stem), cmap, first)
                          for n, f in enumerate(files)]
                name = item.name
            elif item.suffix == ".png":
                frames = [bake_frame(item, (0, item.stem), cmap, first)]
                name = item.stem
            else:
                continue
            if len(style.name) > 15 or len(name) > 31:
                fail(f"{item}: name too long")
            entries.append((style.name, name, frames))
    blob = bytearray(b"TART" + struct.pack("<II", VERSION, len(entries)))
    for style, name, frames in entries:
        blob += style.encode().ljust(16, b"\0") + name.encode().ljust(32, b"\0")
        blob += struct.pack("<HH", len(frames), 0)
        for f in frames:
            blob += f
    return entries, bytes(blob)


def main(argv):
    if argv[:1] == ["--dump"]:
        entries, blob = bake(argv[1], argv[2])
        for style, name, frames in entries:
            print(f"{style:10s} {name:24s} {len(frames):3d} frame(s)")
        print(f"{len(blob)} bytes raw, {len(zlib.compress(blob, 9))} deflated")
        return
    if len(argv) != 3:
        fail("usage: artbake.py <art/ui> <ta-palette.json> <out.h>")
    _, blob = bake(argv[0], argv[1])
    z = zlib.compress(blob, 9)
    lines = [f"/* GENERATED by tools/artbake.py from art/ui/ -- do not edit, do not commit. */",
             "#ifndef TAGPU_ART_GEN_H", "#define TAGPU_ART_GEN_H",
             f"#define TAGPU_ART_RAW_SIZE {len(blob)}u",
             f"#define TAGPU_ART_ZLIB_SIZE {len(z)}u",
             "static const unsigned char tagpu_art_zlib[TAGPU_ART_ZLIB_SIZE] = {"]
    for i in range(0, len(z), 24):
        lines.append("    " + ",".join(str(b) for b in z[i:i + 24]) + ",")
    lines += ["};", "#endif", ""]
    out = Path(argv[2])
    tmp = out.with_suffix(".tmp")
    tmp.write_text("\n".join(lines))
    tmp.replace(out)


if __name__ == "__main__":
    main(sys.argv[1:])
