#!/usr/bin/env python3
"""cobalias — add script-name aliases to a compiled TA COB.

The extra-weapons module (research/notes/extra-weapons.md) looks up slot N's
scripts as AimWeaponN / FireWeaponN / QueryWeaponN / AimFromWeaponN (Spring's
names). A test unit does not need a rewritten COB for that: an alias entry that
points a new name at an existing script's code makes weapon 4 aim and fire
through the primary turret's code, which is exactly the "does the loop reach
slot 3" experiment.

    tools/cobalias.py in.cob out.cob AimWeapon4=AimPrimary FireWeapon4=FirePrimary ...
    tools/cobalias.py in.cob --list

COB layout (all offsets absolute, little-endian dwords):
    0  version (4)          1  script count      2  piece count
    3  code length (words)  4  static var count  5  0
    6  -> code offset table (per script, in words)
    7  -> script name offset table   8  -> piece name offset table
    9  -> code               10 -> strings
The file is rebuilt from parsed parts, so every offset is recomputed.
"""

import struct
import sys
from pathlib import Path


def parse(data: bytes):
    h = list(struct.unpack_from("<11I", data, 0))
    ver, nscr, npc, codelen, nstat, _z, o_cidx, o_snames, o_pnames, o_code, o_str = h
    code = data[o_code:o_code + codelen * 4]
    cidx = list(struct.unpack_from(f"<{nscr}I", data, o_cidx))
    snames = [cstr(data, o) for o in struct.unpack_from(f"<{nscr}I", data, o_snames)]
    pnames = [cstr(data, o) for o in struct.unpack_from(f"<{npc}I", data, o_pnames)]
    return {"version": ver, "statics": nstat, "code": code, "scripts": list(zip(snames, cidx)),
            "pieces": pnames}


def cstr(data: bytes, off: int) -> str:
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


def build(cob) -> bytes:
    nscr, npc = len(cob["scripts"]), len(cob["pieces"])
    codelen = len(cob["code"]) // 4
    o_code = 11 * 4
    o_cidx = o_code + len(cob["code"])
    o_snames = o_cidx + nscr * 4
    o_pnames = o_snames + nscr * 4
    o_str = o_pnames + npc * 4
    strings = bytearray()
    soffs, poffs = [], []
    for name, _ in cob["scripts"]:
        soffs.append(o_str + len(strings))
        strings += name.encode("latin-1") + b"\0"
    for name in cob["pieces"]:
        poffs.append(o_str + len(strings))
        strings += name.encode("latin-1") + b"\0"
    out = bytearray()
    out += struct.pack("<11I", cob["version"], nscr, npc, codelen, cob["statics"], 0,
                       o_cidx, o_snames, o_pnames, o_code, o_str)
    out += cob["code"]
    out += struct.pack(f"<{nscr}I", *[c for _, c in cob["scripts"]])
    out += struct.pack(f"<{nscr}I", *soffs)
    out += struct.pack(f"<{npc}I", *poffs)
    out += strings
    return bytes(out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src = Path(argv[1]).read_bytes()
    cob = parse(src)
    if argv[2] == "--list":
        for name, off in cob["scripts"]:
            print(f"{name:<24} code@{off}")
        print("pieces:", " ".join(cob["pieces"]))
        return 0
    dst = Path(argv[2])
    have = {n.lower(): c for n, c in cob["scripts"]}
    for spec in argv[3:]:
        new, _, old = spec.partition("=")
        if old.lower() not in have:
            sys.exit(f"cobalias: no script {old!r} in {argv[1]} (have {', '.join(n for n, _ in cob['scripts'])})")
        if new.lower() in have:
            sys.exit(f"cobalias: {new!r} already exists")
        cob["scripts"].append((new, have[old.lower()]))
        have[new.lower()] = have[old.lower()]
    out = build(cob)
    check = parse(out)
    assert check["scripts"] == cob["scripts"] and check["pieces"] == cob["pieces"] and check["code"] == cob["code"]
    dst.write_bytes(out)
    print(f"{dst}: {len(cob['scripts'])} scripts ({len(argv) - 3} added), {len(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
