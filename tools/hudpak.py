#!/usr/bin/env python3
"""hudpak.py -- pack a folder of art/ into an archive the DLL embeds.

    python3 tools/hudpak.py [--set hud|wiki] <folder> <out.bin>

The DLL reads its art as the mockups do, so nothing here converts: the files go in as they are,
behind a directory, and res.rc embeds the result as an RCDATA resource (src/tagpu_pak.h reads it).
A set names the files of one folder:

    hud   art/hud/, the resource HUDPAK (src/tagpu_hudart.c): layout.json and kit.json as JSON,
          the parts as PNG, the faces as TrueType, and the licence texts, so the faces never
          travel without them. The default.
    wiki  art/wiki/, the resource WIKIPAK: tree.json and the entries (src/tagpu_wikidata.c),
          the stage descriptions stage/<entry>.<scene>.json (src/tagpu_wikiasset.c, which resolves them
          against the player's archives), the sidebar layouts side/<name>.json, fixture.json
          the one the wiki falls back to (src/tagpu_wikidata.c), layout.json, the window's
          numbers (src/tagpu_wikilay.c), and the window's kit,
          kit/kit.json and its parts as PNG (src/tagpu_hudart.c), as tools/hudart/fit.py --set
          wiki fits them from art/wiki-full/kit/.

THE STANDARD LIBRARY ONLY, like tools/artbake.py: the build's Python is the bare python3 CI has.

THE FORMAT, little-endian:
    "HPAK" | u32 version | u32 entries
    per entry: u32 name offset | u32 name length | u32 data offset | u32 data length
    the names (UTF-8, no terminator), then each file's bytes at a 4-aligned offset.
Offsets are from the archive's first byte. The entries are sorted by name.
"""
import struct
import sys
from pathlib import Path

VERSION = 1
# per set: the globs it keeps, and the files it cannot do without
SETS = {
    "hud": (("layout.json", "kit.json", "parts/*.png", "fonts/*.ttf", "fonts/*.txt"), ("layout.json", "kit.json")),
    "wiki": (("tree.json", "layout.json", "entries/*.json", "stage/*.json", "side/*.json", "kit/kit.json", "kit/parts/*.png"),
             ("tree.json", "layout.json", "side/fixture.json", "kit/kit.json")),
}


def main():
    args = sys.argv[1:]
    name = "hud"
    if args[:1] == ["--set"] and len(args) >= 2:
        name, args = args[1], args[2:]
    if len(args) != 2 or name not in SETS:
        sys.exit(__doc__)
    keep, needs = SETS[name]
    root, out = Path(args[0]), Path(args[1])
    files = sorted({p for g in keep for p in root.glob(g) if p.is_file()})
    for need in needs:
        if not (root / need).is_file():
            sys.exit(f"hudpak: {root / need} is missing")
    names = [p.relative_to(root).as_posix().encode() for p in files]
    head = 12 + 16 * len(files)
    at = head + sum(len(n) for n in names)
    table, blobs, noff = [], [], head
    for p, n in zip(files, names):
        at = (at + 3) & ~3
        data = p.read_bytes()
        table.append(struct.pack("<4I", noff, len(n), at, len(data)))
        blobs.append((at, data))
        noff += len(n)
        at += len(data)
    buf = bytearray(at)
    buf[0:12] = struct.pack("<4sII", b"HPAK", VERSION, len(files))
    buf[12:head] = b"".join(table)
    buf[head:head + sum(len(n) for n in names)] = b"".join(names)
    for off, data in blobs:
        buf[off:off + len(data)] = data
    tmp = out.with_suffix(out.suffix + ".tmp")
    tmp.write_bytes(buf)
    tmp.replace(out)


if __name__ == "__main__":
    main()
