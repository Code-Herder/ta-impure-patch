#!/usr/bin/env python3
"""The unit frame's lane table: how a recording's unit codec cuts a unit image into lanes, and
which lanes its rules predict (tagpu_recfmt.h, the sectioned container and the unit frame).

  tools/replay-lanes-gen.py            regenerate tagpu/ddraw/inc/tagpu_reclanes.h
  tools/replay-lanes-gen.py --check    exit 0 current, 1 stale or invalid, 2 could not run

THE ONE COPY is tagpu/ddraw/replay-lanes.tsv. The capture writes the table into each recording's
header (RT_CODEC) from the generated header, and a reader cuts that recording's images by the
table it carries, so editing the file never orphans a recording made before. The table is a size
hint, never a correctness one: a lane's width only decides how its residuals are reckoned, and
every byte of the image is in exactly one lane, so a wrong width costs bytes and loses none. The
codec finds the lanes its rules read by ROLE, never by offset; ROLES below is the one copy of the
role numbers, which are format constants: a reader in Python loads this file for them rather than
copying them.

THE FILE, tab-separated, `#` comments and blank lines ignored, one row per lane, in image order:

  offset    the lane's first byte in the unit image (the unit record, then its mover at 0x118,
            0x148 bytes in all: REC_UNIT_IMAGE), hex
  size      its width in bytes: 1, 2 or 4
  role      PLAIN for a lane no rule predicts, else the field a rule computes (ROLES)
  name      a short name, unique, one word
  evidence  what the field is and how that was established (DISASSEMBLED, MEASURED, [INFERRED])

Checked here, so a malformed row fails the build by line: the rows tile the image from 0 with no
gap and no overlap and end at 0x148, every width is 1, 2 or 4, every role is known, each role
but PLAIN and COUNT is on exactly one row with its own width, at most 8 rows are COUNT, at most
128 rows in all, names are unique and every row has evidence. The header carries the SHA-256 of
the file and of this script, so a stale header says which of the two moved.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DDRAW = os.path.normpath(os.path.join(HERE, "..", "tagpu", "ddraw"))
TSV = os.path.join(DDRAW, "replay-lanes.tsv")
HEADER = os.path.join(DDRAW, "inc", "tagpu_reclanes.h")

# The roles, in role-number order, each with the width its rule computes in bytes; 0 is any
# width. A role's number is its index here and is written into every recording's RT_CODEC rows,
# so a role is only ever appended.
ROLES = [
    ("PLAIN", 0),                                       # no rule
    ("HEADING", 2),                                     # the unit's heading
    ("TURN", 2),                                        # the mover's turn rate +0x24
    ("SPEED", 4),                                       # the mover's speed +0x20
    ("VX", 4), ("VY", 4), ("VZ", 4),                    # the mover's velocity +0x08 +0x0C +0x10
    ("AX", 4), ("AY", 4), ("AZ", 4),                    # its smoothed acceleration +0x14 +0x18 +0x1C
    ("X", 4), ("Y", 4), ("Z", 4),                       # the position; Y is the altitude
    ("BANK", 2), ("ROLL", 2),                           # the bank and its pitch-scale twin
    ("FX", 2), ("FZ", 2), ("FW", 2), ("FH", 2),         # the footprint's cell and size
    ("SX", 2), ("SZ", 2),                               # the sight cell
    ("CLAIM", 4),                                       # the mover's claim stamp +0x26, a dword
    ("STAMP", 4),                                       # the mover's move stamp +0x2A
    ("LAYER", 1),                                       # the mover's +0x2E: the layer in bits 0-1
    ("TYPE", 2),                                        # the engine's type index
    ("COUNT", 0),                                       # a countdown: one less each iteration, down to 0
]
ROLE_NUMBER = {name: i for i, (name, _) in enumerate(ROLES)}
MULTI = ("PLAIN", "COUNT")      # the roles more than one row may take
IMAGE = 0x148                   # REC_UNIT_IMAGE; tagpu_recfmt.h asserts the two agree
LANES_MAX = 128                 # the most rows RT_CODEC may carry
COUNT_MAX = 8                   # the most COUNT rows
COLUMNS = ("offset", "size", "role", "name", "evidence")


class Invalid(Exception):
    pass


def parse(text):
    rows = []
    at = 0                      # the next byte the rows have to cover
    prev_line = None
    roles = {}
    names = {}
    counts = 0
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != len(COLUMNS):
            raise Invalid(f"line {n}: {len(cols)} columns, want {len(COLUMNS)} ({', '.join(COLUMNS)})")
        off, size, role, name, evidence = (c.strip() for c in cols)
        if not re.fullmatch(r"0x[0-9A-Fa-f]+", off):
            raise Invalid(f"line {n}: offset {off!r} is not hex")
        if size not in ("1", "2", "4"):
            raise Invalid(f"line {n}: size {size!r} is not 1, 2 or 4")
        off, size = int(off, 16), int(size)
        if off > at:
            raise Invalid(f"line {n}: offset {off:#x} leaves a gap: the rows before cover up to {at:#x}")
        if off < at:
            raise Invalid(f"line {n}: offset {off:#x} overlaps line {prev_line}, which covers up to {at:#x}")
        if off + size > IMAGE:
            raise Invalid(f"line {n}: {off:#x} size {size} runs past the image's {IMAGE:#x} bytes")
        if role not in ROLE_NUMBER:
            raise Invalid(f"line {n}: role {role!r} is not one of {', '.join(r for r, _ in ROLES)}")
        width = ROLES[ROLE_NUMBER[role]][1]
        if width and size != width:
            raise Invalid(f"line {n}: role {role} is {width} bytes wide, not {size}")
        if role not in MULTI:
            if role in roles:
                raise Invalid(f"line {n}: role {role} is also line {roles[role]}")
            roles[role] = n
        if role == "COUNT":
            counts += 1
            if counts > COUNT_MAX:
                raise Invalid(f"line {n}: COUNT row {counts}, at most {COUNT_MAX}")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
            raise Invalid(f"line {n}: name {name!r} must be one word")
        if name in names:
            raise Invalid(f"line {n}: name {name!r} is also line {names[name]}")
        names[name] = n
        if not evidence:
            raise Invalid(f"line {n}: no evidence")
        if len(rows) == LANES_MAX:
            raise Invalid(f"line {n}: row {len(rows) + 1}, at most {LANES_MAX}")
        rows.append(dict(line=n, offset=off, size=size, role=role, name=name))
        at = off + size
        prev_line = n
    if at != IMAGE:
        raise Invalid(f"the rows cover {at:#x} bytes, not the image's {IMAGE:#x}")
    missing = [r for r, _ in ROLES if r not in MULTI and r not in roles]
    if missing:
        raise Invalid(f"no row for role {', '.join(missing)}")
    return rows


def enum_name(role):
    """REC_LR_COUNT would read as the number of roles, so the COUNT role's constant is
    REC_LR_COUNT_ and the number of roles is REC_LR_N."""
    return "REC_LR_COUNT_" if role == "COUNT" else f"REC_LR_{role}"


def render(tsv_bytes, gen_bytes, rows):
    digest = hashlib.sha256(tsv_bytes).hexdigest()
    gen_digest = hashlib.sha256(gen_bytes).hexdigest()
    out = [
        "/* tagpu_reclanes.h -- GENERATED from tagpu/ddraw/replay-lanes.tsv by",
        "   tools/replay-lanes-gen.py, which says what the lanes and roles are. Edit the file and",
        "   regenerate, never this header: the build re-derives it and fails when the two disagree. */",
        "#ifndef TAGPU_RECLANES_H",
        "#define TAGPU_RECLANES_H",
        "",
        "#include <stdint.h>",
        "",
        f'#define TAGPU_RECLANES_SHA256 "{digest}"',
        f'#define TAGPU_RECLANES_GEN_SHA256 "{gen_digest}"',
        "",
        "enum RecLaneRole {      /* a format constant: RT_CODEC's role byte */",
    ]
    for role, width in ROLES:
        what = "any width" if not width else "1 byte" if width == 1 else f"{width} bytes"
        out.append(f"    {enum_name(role) + ',':<20}/* {what} */")
    out += [
        "    REC_LR_N",
        "};",
        "",
        "/* the width a role's rule computes, in bytes; 0 for any of 1, 2 and 4 */",
        "static const unsigned char rec_role_width[REC_LR_N] = { "
        + ", ".join(str(w) for _, w in ROLES) + " };",
        "",
        "typedef struct RecLaneRow { uint16_t off; uint8_t width, role; } RecLaneRow;",
        "",
        f"#define REC_LANES_IMAGE {IMAGE:#x}",
        f"#define REC_LANES_N {len(rows)}",
        "static const RecLaneRow rec_lanes[REC_LANES_N] = {",
    ]
    for r in rows:
        out.append(f"    {{ {r['offset']:#05x}, {r['size']}, {enum_name(r['role'])} }},"
                   f"{'':<{max(1, 18 - len(enum_name(r['role'])))}}/* {r['name']} */")
    out += ["};", "", "#endif", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="verify the header against the file")
    a = ap.parse_args()
    try:
        tsv = open(TSV, "rb").read()
    except OSError as e:
        print(f"replay-lanes: cannot read {os.path.relpath(TSV, DDRAW)}: {e.strerror}", file=sys.stderr)
        return 2
    try:
        rows = parse(tsv.decode("utf-8"))
    except (Invalid, UnicodeDecodeError) as e:
        print(f"replay-lanes: {os.path.relpath(TSV, DDRAW)}: {e}", file=sys.stderr)
        return 1
    gen = open(os.path.abspath(__file__), "rb").read()
    text = render(tsv, gen, rows)
    if not a.check:
        with open(HEADER, "w", newline="\n") as f:
            f.write(text)
        print(f"replay-lanes: {len(rows)} rows -> {os.path.relpath(HEADER, DDRAW)}")
        return 0
    try:
        have = open(HEADER, encoding="utf-8").read()
    except OSError:
        print(f"replay-lanes: {os.path.relpath(HEADER, DDRAW)} is missing -- run tools/replay-lanes-gen.py",
              file=sys.stderr)
        return 1
    if have == text:
        return 0
    def stamp(name, header):
        m = re.search(name + r' "([0-9a-f]+)"', header)
        return m.group(1) if m else None
    if stamp("TAGPU_RECLANES_SHA256", have) != stamp("TAGPU_RECLANES_SHA256", text):
        why = "replay-lanes.tsv changed since the header was generated"
    elif stamp("TAGPU_RECLANES_GEN_SHA256", have) != stamp("TAGPU_RECLANES_GEN_SHA256", text):
        why = "tools/replay-lanes-gen.py changed since the header was generated"
    else:
        why = "the header was edited by hand"
    print(f"replay-lanes: inc/tagpu_reclanes.h is stale ({why}) -- run tools/replay-lanes-gen.py",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
