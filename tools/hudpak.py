#!/usr/bin/env python3
"""hudpak.py -- pack the replay HUD (art/hud/) into the one archive the DLL embeds.

    python3 tools/hudpak.py <art/hud> <out.bin>

The DLL reads art/hud/ as the mockup does -- layout.json and kit.json as JSON, the parts as PNG,
the faces as TrueType -- so nothing here converts: the files go in as they are, behind a
directory, and res.rc embeds the result as the RCDATA resource HUDPAK (src/tagpu_hudart.c reads
it). The licence texts go in too, so the faces never travel without them.

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
KEEP = ("layout.json", "kit.json", "parts/*.png", "fonts/*.ttf", "fonts/*.txt")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out = Path(sys.argv[1]), Path(sys.argv[2])
    files = sorted({p for g in KEEP for p in root.glob(g) if p.is_file()})
    for need in ("layout.json", "kit.json"):
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
