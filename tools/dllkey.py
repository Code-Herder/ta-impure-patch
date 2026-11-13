#!/usr/bin/env python3
"""dllkey -- a built DLL's identity for a resumed run: its bytes, less the time it was linked.

A resumed run (tareplay.py `--resume`, wiki_regression.py `gate --resume`) keeps a result only
under the DLL it was graded on. The link writes its own time into three fields of the PE file, and
`make` relinks the DLL at every run -- inc/git.h is rewritten at parse, so every object that
includes it is compiled again -- so a hash of the whole file changes with any `make` between two
sittings, and a resume then plays everything again. digest() hashes the file with those three
fields zeroed:

  * the file header's TimeDateStamp (PE signature + 8),
  * the optional header's CheckSum (optional header + 64), a sum over the file, stamp included,
  * the export directory's TimeDateStamp (its table + 4).

Every other byte is the same for the same tree. MEASURED 2026-11-13 at d782be3e: two `make` runs
of an unchanged tree (git.h rewritten with the same commit and branch, the six objects that include
it recompiled, the DLL relinked) gave ddraw.dll files that differ in exactly these three fields; so
did a `make clean` rebuild against the first, and two `make COVER=1` runs. No source uses __DATE__,
__TIME__ or __TIMESTAMP__. git.h holds the commit -- with `-dirty` when the tree has changes -- and
the branch, so a DLL of another commit, of a dirty tree or of another branch differs outside the
three fields and is refused, as a whole-file hash would refuse it.

A file that is not a PE image, or whose fields lie outside it, is hashed whole: it has no link
time to leave out.
"""
import hashlib
import struct
import sys
from pathlib import Path


def stamp_fields(data: bytes) -> list:
    """The file offsets of the three link-time fields (module docstring) that `data` holds, or []
    when it is not a PE image whose headers and export directory lie inside it."""
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[:2] != b"MZ" or data[pe:pe + 4] != b"PE\0\0":
            return []
        sections, = struct.unpack_from("<H", data, pe + 6)
        opt_size, = struct.unpack_from("<H", data, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", data, opt)
        if magic not in (0x10B, 0x20B):
            return []
        directory = opt + (96 if magic == 0x10B else 112)
        export_rva, _ = struct.unpack_from("<II", data, directory)
        fields = [pe + 8, opt + 64]
        table = opt + opt_size
        for k in range(sections):
            vsize, va, raw_size, raw_at = struct.unpack_from("<IIII", data, table + 40 * k + 8)
            if export_rva and va <= export_rva < va + max(vsize, raw_size):
                fields.append(export_rva - va + raw_at + 4)
                break
    except struct.error:
        return []
    return fields if all(f + 4 <= len(data) for f in fields) else []


def digest(path) -> "str | None":
    """The MD5 of the file at `path` with its link-time fields zeroed (stamp_fields); None when
    there is no such file."""
    try:
        data = bytearray(Path(path).read_bytes())
    except FileNotFoundError:
        return None
    for f in stamp_fields(bytes(data)):
        data[f:f + 4] = bytes(4)
    return hashlib.md5(data).hexdigest()


if __name__ == "__main__":
    for p in sys.argv[1:]:
        print(f"{digest(p)}  {p}")
