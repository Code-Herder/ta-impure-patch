#!/usr/bin/env python3
"""The replay layout: which bytes of the engine's unit and player records the demo recorder does
not record as plain values (research/notes/tadr-port/demo-recorder-completeness.md).

  tools/replay-layout-gen.py            regenerate tagpu/ddraw/inc/tagpu_replay_layout.h
  tools/replay-layout-gen.py --check    exit 0 current, 1 stale or invalid, 2 could not run

THE ONE COPY is tagpu/ddraw/replay-layout.tsv. Every byte of a unit or player record that no row
of it covers is recorded whole and must replay exactly, so a row is a loosening, and adding one is
the owner's decision (CLAUDE.md, *Simulation changes stay replayable*). The recorder compiles the
table from the generated header; the build re-derives the header from the file and fails when the
two disagree, so a change to what is excluded is always a visible diff of the file, and the header
cannot be edited around it.

THE FILE, tab-separated, `#` comments and blank lines ignored, one row per field or bit range:

  record    unit (stride 0x118) or player (stride 0x14B)
  offset    the field's first byte, hex
  size      its width in bytes, decimal
  mask      `-` for every bit, or a hex mask over the field read as a little-endian integer
            (fields of at most 4 bytes); a bit belongs to one row at most
  class     id:<table>     a pointer, recorded and compared as its target's ID
            object         a pointer to an engine object recorded by a record of its own, which
                           the row's name names
            cache          not recorded; the engine recomputes it from recorded state, and it is
                           compared after that recompute
            timer          not recorded; on a clock a replay does not reproduce
            drop           not recorded and not restored: the viewing machine's own state
  name      a short name, used in the harness's failure lines
  writers   the engine code that writes the field: comma-separated addresses in .text
  evidence  how each fact was established (DISASSEMBLED, MEASURED, [INFERRED]) and where

Checked here, so a malformed row fails the build by line: known records, classes and ID tables,
the field inside its record, the mask inside the field, no bit in two rows, rows in record and
offset order, and every writer an address inside the executable's .text.
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
RECORDS = {"unit": 0x118, "player": 0x14B}
CLASSES = ("id", "object", "cache", "timer", "drop")
# The tables an id row may name. Every ID is taken from the pointer by its offset in the table,
# never from a field of the target, so a pointer to a dead unit or an empty slot keeps its ID.
ID_TABLES = {
    "unit": "the slot: (pointer - the unit array's begin) / 0x118",
    "type": "(pointer - *(main+0x1439B)) / 0x249, the unit-type definitions",
    "weapon": "the index in the weapon definitions, stride 0x115",
    "player": "(pointer - (main+0x1B63)) / 0x14B, eleven records",
    "bucket": "(pointer - *(main+0x1429F)) / 10; NULL, off-map *(main+0x142B7): own IDs",
}
# The executable's .text (the pristine TotalA.exe's section table).
TEXT_BEGIN, TEXT_END = 0x401000, 0x4FBA00
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
        kind, _, target = cls.partition(":")
        if kind not in CLASSES:
            raise Invalid(f"line {n}: class {cls!r} is not one of {', '.join(CLASSES)}")
        if kind == "id" and target not in ID_TABLES:
            raise Invalid(f"line {n}: id table {target!r} is not one of {', '.join(ID_TABLES)}")
        if kind != "id" and target:
            raise Invalid(f"line {n}: class {kind} takes no target")
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


def render(tsv_bytes, rows):
    digest = hashlib.sha256(tsv_bytes).hexdigest()
    out = [
        "/* tagpu_replay_layout.h -- GENERATED from tagpu/ddraw/replay-layout.tsv by",
        "   tools/replay-layout-gen.py, which says what each class means. Edit the file and",
        "   regenerate, never this header: the build re-derives it and fails when the two disagree. */",
        "#ifndef TAGPU_REPLAY_LAYOUT_H",
        "#define TAGPU_REPLAY_LAYOUT_H",
        "",
        f'#define TAGPU_REPLAY_LAYOUT_SHA256 "{digest}"',
        "",
        "enum { " + ", ".join(f"RL_{r.upper()}" for r in RECORDS) + " };",
        "enum { " + ", ".join(f"RL_{c.upper()}" for c in CLASSES) + " };",
        "enum {              /* an id row's table: the ID is the pointer's offset in it */",
        "    RL_T_NONE,",
    ]
    for t, how in ID_TABLES.items():
        out.append(f"    RL_T_{t.upper() + ',':<10}/* {how} */")
    out += [
        "};",
        "",
        "typedef struct ReplayLayoutRow {",
        "    unsigned char  record, cls, target, size;",
        "    unsigned short offset;",
        "    unsigned int   mask;      /* over the field as a little-endian integer; 0 = every byte */",
        "    const char*    name;",
        "} ReplayLayoutRow;",
        "",
        "static const ReplayLayoutRow tagpu_replay_layout[] = {",
    ]
    for r in rows:
        target = f"RL_T_{r['target'].upper()}" if r["kind"] == "id" else "RL_T_NONE"
        mask = 0 if r["mask"] is None or r["mask"] == (1 << (8 * r["size"])) - 1 else r["mask"]
        out.append(f"    {{ RL_{r['record'].upper()}, RL_{r['kind'].upper()}, {target}, {r['size']}, "
                   f"{r['offset']:#05x}, {mask:#x}u, \"{r['name']}\" }},")
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


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
    text = render(tsv, rows)
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
    want_hash = re.search(r'SHA256 "([0-9a-f]+)"', text).group(1)
    got = re.search(r'SHA256 "([0-9a-f]+)"', have)
    if not got or got.group(1) != want_hash:
        why = "replay-layout.tsv changed since the header was generated"
    else:
        why = "the header was edited by hand"
    print(f"replay-layout: inc/tagpu_replay_layout.h is stale ({why}) -- run tools/replay-layout-gen.py",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
