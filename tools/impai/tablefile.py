#!/usr/bin/env python3
"""tablefile.py -- ImpAI's response table as a file: the 132 bytes of IMPAI_TABLE, little-endian, then their
FNV-1a 64 as 8 little-endian bytes (tagpu/ddraw/src/impai_table.h, THE FILE).

    tablefile.py show FILE          its hash and numbers

The C loader (impai_table_load) is the authority on what a table may hold -- it bounds the rows and the numbers
and refuses the whole file otherwise; this module reads and writes the bytes, checking only the size and the
trailer. tagpu/ddraw/tests/impai_table_test.py holds the two to one format. The names below are the C enums'
order, which is the format."""
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

CLASSES = ["lraider", "lassault", "lartillery", "lantiair", "lscout", "sraider", "sassault", "sartillery",
           "santiair", "fighter", "bomber", "gunship", "ascout"]                       # IMPAI_K_*
FEATURES = ["lraider", "lassault", "lartillery", "sea", "fighter", "bomber", "gunship", "defence", "antiair",
            "income", "time", "tech", "air", "lowaa"]                                  # IMPAI_F_*, then derived
GROUPS = ["aa", "sea"]                                                                 # IMPAI_KG_*
ROWS = ["aa_air", "lartillery_defence", "lraider_lartillery", "lassault_lraider", "fighter_bomber",
        "fighter_gunship", "bomber_lowaa", "sea_sea"]                                  # IMPAI_R_*
AIR = {"fighter", "bomber", "gunship", "ascout"}
SURFACE_LAND = {"lraider", "lassault", "lartillery", "lantiair", "lscout"}
SURFACE_SEA = {"sraider", "sassault", "sartillery", "santiair"}

KS = len(CLASSES)
RESPONSES = len(ROWS)
FMT = "<" + "f" * KS + "BBHf" * RESPONSES + "ffff"
TABLE_BYTES = struct.calcsize(FMT)
FILE_BYTES = TABLE_BYTES + 8
assert TABLE_BYTES == 132


class TableError(ValueError):
    pass


def fnv64(b: bytes) -> int:
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


@dataclass
class Table:
    base: list
    rows: list                  # (feature, klass) per slot: the shape, never tuned
    gains: list
    counter: float
    air_from_t: float
    aa_cap: float
    tech_m: float
    pads: list = field(default_factory=lambda: [0] * RESPONSES)

    def pack(self) -> bytes:
        vals = list(self.base)
        for (f, k), p, g in zip(self.rows, self.pads, self.gains):
            vals += [f, k, p, g]
        vals += [self.counter, self.air_from_t, self.aa_cap, self.tech_m]
        return struct.pack(FMT, *vals)

    def hash(self) -> int:
        return fnv64(self.pack())

    def hash_hex(self) -> str:
        return f"{self.hash():016x}"

    def file_bytes(self) -> bytes:
        b = self.pack()
        return b + struct.pack("<Q", fnv64(b))

    def copy(self) -> "Table":
        return Table(list(self.base), list(self.rows), list(self.gains), self.counter, self.air_from_t,
                     self.aa_cap, self.tech_m, list(self.pads))

    def describe(self) -> dict:
        def klass(k):
            return CLASSES[k] if k < KS else ("group:" + GROUPS[k - KS] if k - KS < len(GROUPS) else str(k))
        return {
            "hash": self.hash_hex(),
            "base": dict(zip(CLASSES, self.base)),
            "rows": [{"slot": ROWS[i], "feature": FEATURES[f] if f < len(FEATURES) else str(f), "class": klass(k),
                      "gain": g} for i, ((f, k), g) in enumerate(zip(self.rows, self.gains))],
            "counter": self.counter, "air_from_t": self.air_from_t, "aa_cap": self.aa_cap, "tech_m": self.tech_m,
        }


def unpack(b: bytes) -> Table:
    if len(b) != FILE_BYTES:
        raise TableError(f"a table file is {FILE_BYTES} bytes, not {len(b)}")
    body, (want,) = b[:TABLE_BYTES], struct.unpack("<Q", b[TABLE_BYTES:])
    if fnv64(body) != want:
        raise TableError("the FNV-1a 64 trailer does not match the table")
    v = struct.unpack(FMT, body)
    base = list(v[:KS])
    rows, pads, gains = [], [], []
    for i in range(RESPONSES):
        f, k, p, g = v[KS + 4 * i: KS + 4 * i + 4]
        rows.append((f, k))
        pads.append(p)
        gains.append(g)
    c, a, cap, t = v[KS + 4 * RESPONSES:]
    return Table(base, rows, gains, c, a, cap, t, pads)


def read(path) -> Table:
    return unpack(Path(path).read_bytes())


def write(path, t: Table) -> str:
    """t into path, written whole or not at all; its hash."""
    path = Path(path)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(t.file_bytes())
    tmp.replace(path)
    return t.hash_hex()


def main(argv) -> int:
    import json
    if len(argv) != 2 or argv[0] != "show":
        print(__doc__)
        return 2
    try:
        print(json.dumps(read(argv[1]).describe(), indent=1))
    except (OSError, TableError) as e:
        print(f"tablefile: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
