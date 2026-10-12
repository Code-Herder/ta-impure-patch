#!/usr/bin/env python3
"""The hold table: the bytes a skirmish recording's stream holds between refreshes
(tagpu/ddraw/src/tagpu_reclossy.c; tagpu_recfmt.h, RT_HOLD).

  tools/replay-hold-gen.py            regenerate tagpu/ddraw/inc/tagpu_rechold.h
  tools/replay-hold-gen.py --check    exit 0 current, 1 stale or invalid, 2 could not run

THE ONE COPY is tagpu/ddraw/replay-hold.tsv. The writer puts the generated masks and the file's
SHA-256 into each skirmish recording's header (RT_HOLD), and every reader takes the masks from the
recording it reads, so editing the file never orphans a recording made before. A row is a
loosening and its addition the owner's decision: the file's header says what one means.

THE FILE, tab-separated, `#` comments and blank lines ignored, one row per held field:

  record    `unit` for the unit image (the unit record, then its mover at 0x118, 0x148 bytes in
            all: REC_UNIT_IMAGE), `order` for a node of a unit's main list (+0x5C), 0x58 bytes
            (REC_ORDER_IMAGE). The sub list (+0x60) is never held: the bottom panel reads it on a
            playback host (0x439D20, DISASSEMBLED)
  offset    the field's first byte in its image, hex
  width     its bytes: 1, 2 or 4
  mask      the bits held, hex, little-endian over the field's bytes; `-` for every bit
  name      a short name, unique, one word
  readers   the readers of the field that run on a playback host, `-` for none
  evidence  how the readers were established (DISASSEMBLED with addresses, MEASURED)

Checked here, so a malformed row fails the build by line: a known record, a field inside its
image, a width of 1, 2 or 4, a mask that is not zero and fits the width, no bit held by two rows,
no unit row touching the position +0x6A..+0x75 (a spray session that follows its unit reads it,
tagpu_recfmt.h REC_HOLD_POS_AT), names unique, and readers and evidence present. The header
carries the SHA-256 of the file and of this script, so a stale header says which of the two moved.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DDRAW = os.path.normpath(os.path.join(HERE, "..", "tagpu", "ddraw"))
TSV = os.path.join(DDRAW, "replay-hold.tsv")
HEADER = os.path.join(DDRAW, "inc", "tagpu_rechold.h")

IMAGES = {"unit": 0x148, "order": 0x58}     # REC_UNIT_IMAGE, REC_ORDER_IMAGE; the C asserts them
POS_AT, POS_END = 0x6A, 0x76                # REC_HOLD_POS_AT, REC_HOLD_POS_END
COLUMNS = ("record", "offset", "width", "mask", "name", "readers", "evidence")


class Invalid(Exception):
    pass


def parse(text):
    """The masks, {record: bytearray}, and the rows."""
    masks = {r: bytearray(n) for r, n in IMAGES.items()}
    owner = {r: [None] * n for r, n in IMAGES.items()}
    names = {}
    rows = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != len(COLUMNS):
            raise Invalid(f"line {n}: {len(cols)} columns, want {len(COLUMNS)} ({', '.join(COLUMNS)})")
        record, off, width, mask, name, readers, evidence = (c.strip() for c in cols)
        if record not in IMAGES:
            raise Invalid(f"line {n}: record {record!r} is not one of {', '.join(IMAGES)}")
        if not re.fullmatch(r"0x[0-9A-Fa-f]+", off):
            raise Invalid(f"line {n}: offset {off!r} is not hex")
        if width not in ("1", "2", "4"):
            raise Invalid(f"line {n}: width {width!r} is not 1, 2 or 4")
        off, width = int(off, 16), int(width)
        if off + width > IMAGES[record]:
            raise Invalid(f"line {n}: {off:#x} width {width} runs past the {record} image's "
                          f"{IMAGES[record]:#x} bytes")
        full = (1 << (8 * width)) - 1
        if mask == "-":
            bits = full
        elif re.fullmatch(r"0x[0-9A-Fa-f]+", mask):
            bits = int(mask, 16)
            if not bits or bits > full:
                raise Invalid(f"line {n}: mask {mask} does not fit a field of {width} bytes, or is 0")
        else:
            raise Invalid(f"line {n}: mask {mask!r} is not hex or -")
        if record == "unit" and off < POS_END and off + width > POS_AT:
            raise Invalid(f"line {n}: {off:#x} width {width} touches the position "
                          f"+{POS_AT:#x}..+{POS_END - 1:#x}, which a spray session reads")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
            raise Invalid(f"line {n}: name {name!r} must be one word")
        if name in names:
            raise Invalid(f"line {n}: name {name!r} is also line {names[name]}")
        names[name] = n
        if not readers:
            raise Invalid(f"line {n}: no readers (`-` for none)")
        if not evidence:
            raise Invalid(f"line {n}: no evidence")
        for k in range(width):
            b = (bits >> (8 * k)) & 0xFF
            if not b:
                continue
            if masks[record][off + k] & b:
                raise Invalid(f"line {n}: {record} byte {off + k:#x} is held by line "
                              f"{owner[record][off + k]} too")
            masks[record][off + k] |= b
            owner[record][off + k] = n
        rows.append(dict(line=n, record=record, offset=off, width=width, bits=bits, name=name))
    return masks, rows


def array(name, data):
    out = [f"static const unsigned char {name}[{len(data):#x}] = {{"]
    for at in range(0, len(data), 16):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[at:at + 16]) + ",")
    out.append("};")
    return out


def render(tsv_bytes, gen_bytes, masks, rows):
    digest = hashlib.sha256(tsv_bytes).hexdigest()
    gen_digest = hashlib.sha256(gen_bytes).hexdigest()
    out = [
        "/* tagpu_rechold.h -- GENERATED from tagpu/ddraw/replay-hold.tsv by",
        "   tools/replay-hold-gen.py, which says what a row is. Edit the file and regenerate, never",
        "   this header: the build re-derives it and fails when the two disagree. */",
        "#ifndef TAGPU_RECHOLD_H",
        "#define TAGPU_RECHOLD_H",
        "",
        f'#define TAGPU_REPLAY_HOLD_SHA256 "{digest}"',
        f'#define TAGPU_REPLAY_HOLD_GEN_SHA256 "{gen_digest}"',
        f"#define REC_HOLD_ROWS {len(rows)}",
        "",
        "/* a unit image's held bits (REC_UNIT_IMAGE bytes) */",
    ]
    out += array("REC_HOLD_UNIT", masks["unit"])
    out += ["", "/* a main list node's held bits (REC_ORDER_IMAGE bytes) */"]
    out += array("REC_HOLD_NODE", masks["order"])
    out += ["", "#endif", ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="verify the header against the file")
    a = ap.parse_args()
    try:
        tsv = open(TSV, "rb").read()
    except OSError as e:
        print(f"replay-hold: cannot read {os.path.relpath(TSV, DDRAW)}: {e.strerror}", file=sys.stderr)
        return 2
    try:
        masks, rows = parse(tsv.decode("utf-8"))
    except (Invalid, UnicodeDecodeError) as e:
        print(f"replay-hold: {os.path.relpath(TSV, DDRAW)}: {e}", file=sys.stderr)
        return 1
    gen = open(os.path.abspath(__file__), "rb").read()
    text = render(tsv, gen, masks, rows)
    if not a.check:
        with open(HEADER, "w", newline="\n") as f:
            f.write(text)
        print(f"replay-hold: {len(rows)} rows -> {os.path.relpath(HEADER, DDRAW)}")
        return 0
    try:
        have = open(HEADER, encoding="utf-8").read()
    except OSError:
        print(f"replay-hold: {os.path.relpath(HEADER, DDRAW)} is missing -- run tools/replay-hold-gen.py",
              file=sys.stderr)
        return 1
    if have == text:
        return 0

    def stamp(name, header):
        m = re.search(name + r' "([0-9a-f]+)"', header)
        return m.group(1) if m else None
    if stamp("TAGPU_REPLAY_HOLD_SHA256", have) != stamp("TAGPU_REPLAY_HOLD_SHA256", text):
        why = "replay-hold.tsv changed since the header was generated"
    elif stamp("TAGPU_REPLAY_HOLD_GEN_SHA256", have) != stamp("TAGPU_REPLAY_HOLD_GEN_SHA256", text):
        why = "tools/replay-hold-gen.py changed since the header was generated"
    else:
        why = "the header was edited by hand"
    print(f"replay-hold: inc/tagpu_rechold.h is stale ({why}) -- run tools/replay-hold-gen.py",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
