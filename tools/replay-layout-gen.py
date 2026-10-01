#!/usr/bin/env python3
"""The replay layout: which bytes of the engine records a demo recording carries it does not record
as plain values (research/notes/tadr-port/demo-recorder-completeness.md).

  tools/replay-layout-gen.py            regenerate tagpu/ddraw/inc/tagpu_replay_layout.h
  tools/replay-layout-gen.py --check    exit 0 current, 1 stale or invalid, 2 could not run

THE ONE COPY is tagpu/ddraw/replay-layout.tsv. Every byte of a carried record that no row of it
covers is recorded whole and must replay exactly, so a row is a loosening, and adding one is
the owner's decision (CLAUDE.md, *Simulation changes stay replayable*). The recorder compiles the
table from the generated header; the build re-derives the header from the file and fails when the
two disagree, so a change to what is excluded is always a visible diff of the file, and the header
cannot be edited around it.

THE FILE, tab-separated, `#` comments and blank lines ignored, one row per field or bit range:

  record    unit (0x118 bytes), player (0x14B), mover (0x2F: a unit's movement state),
            order (0x56: a node of a unit's order lists), info (0xB9: a player's PlayerInfo),
            cell (0x0D: a cell of the feature grid), wreck3do or wreckgaf (0x30: a record of the
            wreck pool, as a 3DO or a GAF feature's def lays it out)
  offset    the field's first byte, hex
  size      its width in bytes, decimal
  mask      `-` for every bit, or a hex mask over the field read as a little-endian integer
            (fields of at most 4 bytes); a bit belongs to one row at most
  class     id:<table>     a pointer, recorded and compared as its target's ID
            object         a pointer to an engine object recorded by a record of its own, which
                           the row's name names
                           (both are dwords: four bytes, no mask)
            cache          not recorded; the engine recomputes it from recorded state, and it is
                           compared after that recompute
            timer          not recorded; on a clock a replay does not reproduce
            drop           not recorded and not restored: the viewing machine's own state
            puppet0        recorded and compared, but held at 0 on a puppet host, and restored by
                           a takeover: owner state that would run owner code on a puppet
            puppet1        the same, held set on a puppet host
                           (either takes a table, `puppet0:<table>`, for a pointer recorded as
                           its ID: a dword, no mask)
  name      a short name, unique within its record, used in the harness's failure lines
  writers   the engine code that writes the field: comma-separated addresses in .text
  evidence  how each fact was established (DISASSEMBLED, MEASURED, [INFERRED]) and where

Checked here, so a malformed row fails the build by line: known records, classes and ID tables,
the field inside its record, the mask inside the field, a pointer row four bytes wide with no
mask, no bit in two rows, rows in record and offset order, names unique within a record, and
every writer an address inside the executable's .text. The header carries the SHA-256 of the
file and of this script, so a stale header says which of the two moved.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DDRAW = os.path.normpath(os.path.join(HERE, "..", "tagpu", "ddraw"))
TSV = os.path.join(DDRAW, "replay-layout.tsv")
HEADER = os.path.join(DDRAW, "inc", "tagpu_replay_layout.h")

# The records the retail executable fixes, and their strides.
RECORDS = {"unit": 0x118, "player": 0x14B, "mover": 0x2F, "order": 0x56, "info": 0xB9,
           "cell": 0x0D, "wreck3do": 0x30, "wreckgaf": 0x30}
CLASSES = ("id", "object", "cache", "timer", "drop", "puppet0", "puppet1")
# The tables an id row may name. Every ID is taken from the pointer by its offset in the table,
# never from a field of the target, so a pointer to a dead unit or an empty slot keeps its ID.
ID_TABLES = {
    "unit": "the slot: (pointer - the unit array's begin) / 0x118",
    "type": "(pointer - *(main+0x1439B)) / 0x249, the unit-type definitions",
    "weapon": "(pointer - the weapon array's base) / 0x115, not the ID byte +0x10A, which holds IDs below 256 only",
    "player": "(pointer - (main+0x1B63)) / 0x14B, eleven records",
    "bucket": "(pointer - *(main+0x1429F)) / 10; NULL, off-map *(main+0x142B7): own IDs",
    "owned": "not a row: the class code of an order node's own object, by its vtable (the restore's table 0x43A918): 0x4FD3B8 2, 0x4FD3F8 3, 0x4FD328 4, 0x4FD358 5, 0x4FD388 6",
    "gafseq": "not a row: the record's own def's field that holds the pointer, (field - 0xAC) / 4 over FeatureDef +0xAC..+0xC8, the first that does",
}
# The executable's .text: its virtual address and size in the pristine TotalA.exe's section table.
TEXT_BEGIN, TEXT_END = 0x401000, 0x4FB92A
COLUMNS = ("record", "offset", "size", "mask", "class", "name", "writers", "evidence")


class Invalid(Exception):
    pass


def parse(text):
    rows = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != len(COLUMNS):
            raise Invalid(f"line {n}: {len(cols)} columns, want {len(COLUMNS)} ({', '.join(COLUMNS)})")
        rec, off, size, mask, cls, name, writers, evidence = (c.strip() for c in cols)
        if rec not in RECORDS:
            raise Invalid(f"line {n}: record {rec!r} is not one of {', '.join(RECORDS)}")
        if not re.fullmatch(r"0x[0-9A-Fa-f]+", off):
            raise Invalid(f"line {n}: offset {off!r} is not hex")
        if not re.fullmatch(r"[1-9][0-9]*", size):
            raise Invalid(f"line {n}: size {size!r} is not a positive decimal")
        off, size = int(off, 16), int(size)
        if off + size > RECORDS[rec]:
            raise Invalid(f"line {n}: {rec} +{off:#x} size {size} runs past the record's {RECORDS[rec]:#x} bytes")
        full = (1 << (8 * size)) - 1 if size <= 4 else None
        if mask == "-":
            maskv = full
        else:
            if not re.fullmatch(r"0x[0-9A-Fa-f]+", mask):
                raise Invalid(f"line {n}: mask {mask!r} is neither - nor hex")
            if size > 4:
                raise Invalid(f"line {n}: a mask needs a field of at most 4 bytes")
            maskv = int(mask, 16)
            if maskv == 0 or maskv & ~full:
                raise Invalid(f"line {n}: mask {mask} is empty or wider than {size} bytes")
        kind, colon, target = cls.partition(":")
        if kind not in CLASSES:
            raise Invalid(f"line {n}: class {cls!r} is not one of {', '.join(CLASSES)}")
        if (kind == "id" or colon) and target not in ID_TABLES:
            raise Invalid(f"line {n}: id table {target!r} is not one of {', '.join(ID_TABLES)}")
        if colon and kind not in ("id", "puppet0", "puppet1"):
            raise Invalid(f"line {n}: class {kind} takes no table")
        if (kind in ("id", "object") or colon) and (size != 4 or mask != "-"):
            raise Invalid(f"line {n}: {cls} is a pointer: size 4, mask -")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.+-]*", name):
            raise Invalid(f"line {n}: name {name!r} must be one word")
        vas = [w.strip() for w in writers.split(",")]
        for w in vas:
            if not re.fullmatch(r"0x[0-9A-Fa-f]{6}", w) or not TEXT_BEGIN <= int(w, 16) < TEXT_END:
                raise Invalid(f"line {n}: writer {w!r} is not an address in .text")
        if not evidence:
            raise Invalid(f"line {n}: no evidence")
        rows.append(dict(line=n, record=rec, offset=off, size=size, mask=maskv, kind=kind,
                         target=target, name=name))
    if not rows:
        raise Invalid("no rows")
    names = {}
    for r in rows:
        prev = names.setdefault((r["record"], r["name"]), r["line"])
        if prev != r["line"]:
            raise Invalid(f"line {r['line']}: {r['record']} name {r['name']!r} is also line {prev}")
    order = [(list(RECORDS).index(r["record"]), r["offset"]) for r in rows]
    for a, b, r in zip(order, order[1:], rows[1:]):
        if b < a:
            raise Invalid(f"line {r['line']}: rows must be in record and offset order")
    # No bit in two rows: expand each row to (record, byte) -> bits.
    claimed = {}
    for r in rows:
        for i in range(r["size"]):
            bits = 0xFF if r["mask"] is None else (r["mask"] >> (8 * i)) & 0xFF
            key = (r["record"], r["offset"] + i)
            prev = claimed.get(key)
            if prev and prev[0] & bits:
                raise Invalid(f"line {r['line']}: {r['record']} +{key[1]:#x} is also in line {prev[1]}")
            claimed[key] = ((prev[0] if prev else 0) | bits, r["line"])
    return rows


def render(tsv_bytes, gen_bytes, rows):
    digest = hashlib.sha256(tsv_bytes).hexdigest()
    gen_digest = hashlib.sha256(gen_bytes).hexdigest()
    out = [
        "/* tagpu_replay_layout.h -- GENERATED from tagpu/ddraw/replay-layout.tsv by",
        "   tools/replay-layout-gen.py, which says what each class means. Edit the file and",
        "   regenerate, never this header: the build re-derives it and fails when the two disagree. */",
        "#ifndef TAGPU_REPLAY_LAYOUT_H",
        "#define TAGPU_REPLAY_LAYOUT_H",
        "",
        f'#define TAGPU_REPLAY_LAYOUT_SHA256 "{digest}"',
        f'#define TAGPU_REPLAY_LAYOUT_GEN_SHA256 "{gen_digest}"',
        "",
        "enum { " + ", ".join(f"RL_{r.upper()}" for r in RECORDS) + " };",
        "enum { " + ", ".join(f"RL_{c.upper()}" for c in CLASSES) + " };",
        "enum {              /* an id row's table and what its pointer names; a row's ID is one more than the row */",
        "    RL_T_NONE,",
    ]
    for t, how in ID_TABLES.items():
        out.append(f"    RL_T_{t.upper() + ',':<10}/* {how} */")
    out += [
        "};",
        "",
        "typedef struct ReplayLayoutRow {",
        "    unsigned char  record, cls, target;",
        "    unsigned short offset, size;",
        "    unsigned int   mask;      /* over the field as a little-endian integer; 0 = every byte */",
        "    const char*    name;",
        "} ReplayLayoutRow;",
        "",
        "static const ReplayLayoutRow tagpu_replay_layout[] = {",
    ]
    for r in rows:
        target = f"RL_T_{r['target'].upper()}" if r["target"] else "RL_T_NONE"
        mask = 0 if r["mask"] is None or r["mask"] == (1 << (8 * r["size"])) - 1 else r["mask"]
        out.append(f"    {{ RL_{r['record'].upper()}, RL_{r['kind'].upper()}, {target}, "
                   f"{r['offset']:#05x}, {r['size']}, {mask_c(mask)}, \"{r['name']}\" }},")
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def mask_c(m):
    """A mask as C: each run of set bits shifted into place. A run's value is 2^k - 1, never
    between 0x400000 and 0x52FFFF, so no literal of the header reads as an engine address to
    tools/thread-split-check.sh (bit 22 alone is 0x400000)."""
    runs = []
    shift = 0
    while m:
        if not m & 1:
            m >>= 1
            shift += 1
            continue
        n = 0
        while m >> n & 1:
            n += 1
        run = (1 << n) - 1
        runs.append(f"(0x{run:x}u << {shift})" if shift else f"0x{run:x}u")
        m >>= n
        shift += n
    return " | ".join(runs) if runs else "0x0u"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="verify the header against the file")
    a = ap.parse_args()
    try:
        tsv = open(TSV, "rb").read()
    except OSError as e:
        print(f"replay-layout: cannot read {os.path.relpath(TSV, DDRAW)}: {e.strerror}", file=sys.stderr)
        return 2
    try:
        rows = parse(tsv.decode("utf-8"))
    except (Invalid, UnicodeDecodeError) as e:
        print(f"replay-layout: {os.path.relpath(TSV, DDRAW)}: {e}", file=sys.stderr)
        return 1
    gen = open(os.path.abspath(__file__), "rb").read()
    text = render(tsv, gen, rows)
    if not a.check:
        with open(HEADER, "w", newline="\n") as f:
            f.write(text)
        print(f"replay-layout: {len(rows)} rows -> {os.path.relpath(HEADER, DDRAW)}")
        return 0
    try:
        have = open(HEADER, encoding="utf-8").read()
    except OSError:
        print(f"replay-layout: {os.path.relpath(HEADER, DDRAW)} is missing -- run tools/replay-layout-gen.py",
              file=sys.stderr)
        return 1
    if have == text:
        return 0
    def stamp(name, header):
        m = re.search(name + r' "([0-9a-f]+)"', header)
        return m.group(1) if m else None
    if stamp("LAYOUT_SHA256", have) != stamp("LAYOUT_SHA256", text):
        why = "replay-layout.tsv changed since the header was generated"
    elif stamp("GEN_SHA256", have) != stamp("GEN_SHA256", text):
        why = "tools/replay-layout-gen.py changed since the header was generated"
    else:
        why = "the header was edited by hand"
    print(f"replay-layout: inc/tagpu_replay_layout.h is stale ({why}) -- run tools/replay-layout-gen.py",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
