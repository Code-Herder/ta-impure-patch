#!/usr/bin/env python3
"""The replay globals: the positive list of the main block and .data, every byte of the two images
a demo recording carries and how (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S3b).

  tools/replay-globals-gen.py            regenerate tagpu/ddraw/inc/tagpu_replay_globals.h
  tools/replay-globals-gen.py --check    exit 0 current, 1 stale or invalid, 2 could not run

THE ONE COPY is tagpu/ddraw/replay-globals.tsv. Neither image can be recorded or applied whole, so
the recording carries exactly the fields of its `globals` rows, as one image: the globals image. A
dump carries both images whole, and tools/replay/tarec.py `compare --images` names every differing
byte that no row of the file covers. Removing a row, or making one `hold`, is a loosening, and the
owner's decision (CLAUDE.md, *Simulation changes stay replayable*). The recorder and the replay
compile the image from the generated header; the build re-derives the header from the file and
fails when the two disagree.

THE FILE, tab-separated, `#` comments and blank lines ignored, one row per field or bit range:

  where     main    the main block: `offset` is from main (*(0x511DE8)), the field inside its
                    0x3924D bytes
            data    .data: `offset` is the absolute address, the field inside 0x501000..0x52B65C
  offset    the field's first byte, hex
  size      its width in bytes, decimal
  mask      `-` for every bit, or a hex mask over the field read as a little-endian integer
            (fields of at most 4 bytes); a bit belongs to one row at most
  carried   globals     in the globals image
            tick        RK_TICK carries it (GameTime)
            player      RK_PLAYER carries it (the player records)
            viewer      RK_VIEWER carries it (the controlled and the viewed player)
            features    RK_FEATURES carries it (the wreck pool's heads, the reproduction cursor)
            proj        RK_PROJ_HEAD carries it (the projectile count)
            Only `globals` rows make up the image; the others are coverage for `--images`.
  puppet    globals rows only, `-` for the rest:
            write       the replay writes it on a puppet host at every capture point, at the frame
                        visit after a step and at a seek
            hold        never written on a puppet host; recorded, and compared off a puppet only
                        (the puppet compare masks it)
            react       written when it differs, followed by the engine's own reaction
  name      a short name, unique in the file, used in the harness's lines
  writers   the engine's stores to the field: comma-separated instruction addresses in .text, or
            `-` when none was found (the evidence says how the search was made)
  evidence  how each fact was established (DISASSEMBLED, MEASURED, [INFERRED]) and where

THE IMAGE. Rows on one field (the same where, offset and size) are one slot of the image, and their
masks are disjoint. Each slot, in file order, starts at the next dword of the image and takes its
size rounded up to whole dwords; its bytes are the field's, less the bits no row of it names, and
its padding is zero.

Checked here, so a malformed row fails the build by line: the field inside its image, the mask
inside the field, no bit in two rows, rows in (where, offset) order, a name unique in the file, the
`puppet` column as `carried` allows, and every writer an address inside the executable's .text. The
header names no engine address: main rows are offsets from main and data rows are emitted as
offsets from .data's start, so tools/thread-split-check.sh finds nothing in it; the two files that
read and write the fields form the addresses. It carries the SHA-256 of the file and of this
script, so a stale header says which of the two moved.
"""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DDRAW = os.path.normpath(os.path.join(HERE, "..", "tagpu", "ddraw"))
TSV = os.path.join(DDRAW, "replay-globals.tsv")
HEADER = os.path.join(DDRAW, "inc", "tagpu_replay_globals.h")

# The main block's bytes, DISASSEMBLED 0x41D93A (`lea edi,[esi+0x3924D]`, the allocation less its
# pad); .data's extent, from the pristine TotalA.exe's section header (VirtualSize 0x2A65C).
MAIN_BYTES = 0x3924D
DATA_BEGIN, DATA_END = 0x501000, 0x52B65C
WHERE = ("main", "data")
CARRIED = ("globals", "tick", "player", "viewer", "features", "proj")
PUPPET = ("write", "hold", "react")
# The executable's .text: its virtual address and size in the pristine TotalA.exe's section table.
TEXT_BEGIN, TEXT_END = 0x401000, 0x4FB92A
COLUMNS = ("where", "offset", "size", "mask", "carried", "puppet", "name", "writers", "evidence")


class Invalid(Exception):
    pass


def parse(text):
    """The rows: where, offset (from main, or the absolute address), size, mask (None for every
    byte of a field wider than four), carried, puppet (None outside `globals`) and name."""
    rows = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) != len(COLUMNS):
            raise Invalid(f"line {n}: {len(cols)} columns, want {len(COLUMNS)} ({', '.join(COLUMNS)})")
        where, off, size, mask, carried, puppet, name, writers, evidence = (c.strip() for c in cols)
        if where not in WHERE:
            raise Invalid(f"line {n}: where {where!r} is not one of {', '.join(WHERE)}")
        if not re.fullmatch(r"0x[0-9A-Fa-f]+", off):
            raise Invalid(f"line {n}: offset {off!r} is not hex")
        if not re.fullmatch(r"[1-9][0-9]*", size):
            raise Invalid(f"line {n}: size {size!r} is not a positive decimal")
        off, size = int(off, 16), int(size)
        if where == "main" and off + size > MAIN_BYTES:
            raise Invalid(f"line {n}: main +{off:#x} size {size} runs past the main block's "
                          f"{MAIN_BYTES:#x} bytes")
        if where == "data" and not (DATA_BEGIN <= off and off + size <= DATA_END):
            raise Invalid(f"line {n}: {off:#x} size {size} is not inside .data "
                          f"{DATA_BEGIN:#x}..{DATA_END:#x}")
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
        if carried not in CARRIED:
            raise Invalid(f"line {n}: carried {carried!r} is not one of {', '.join(CARRIED)}")
        if carried == "globals" and puppet not in PUPPET:
            raise Invalid(f"line {n}: a globals row's puppet is one of {', '.join(PUPPET)}, "
                          f"not {puppet!r}")
        if carried != "globals" and puppet != "-":
            raise Invalid(f"line {n}: a {carried} row's puppet is -, not {puppet!r}")
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", name):
            raise Invalid(f"line {n}: name {name!r} must be one word")
        if writers != "-":
            for w in (w.strip() for w in writers.split(",")):
                if not re.fullmatch(r"0x[0-9A-Fa-f]{6}", w) or not TEXT_BEGIN <= int(w, 16) < TEXT_END:
                    raise Invalid(f"line {n}: writer {w!r} is not an address in .text")
        if not evidence:
            raise Invalid(f"line {n}: no evidence")
        rows.append(dict(line=n, where=where, offset=off, size=size, mask=maskv, carried=carried,
                         puppet=None if puppet == "-" else puppet, name=name))
    if not rows:
        raise Invalid("no rows")
    names = {}
    for r in rows:
        prev = names.setdefault(r["name"], r["line"])
        if prev != r["line"]:
            raise Invalid(f"line {r['line']}: name {r['name']!r} is also line {prev}")
    order = [(WHERE.index(r["where"]), r["offset"]) for r in rows]
    for a, b, r in zip(order, order[1:], rows[1:]):
        if b < a:
            raise Invalid(f"line {r['line']}: rows must be in where and offset order")
    claimed = {}
    for r in rows:
        for key, bits in row_bytes(r):
            prev = claimed.get(key)
            if prev and prev[0] & bits:
                raise Invalid(f"line {r['line']}: {r['where']} +{key[1]:#x} is also in line {prev[1]}")
            claimed[key] = ((prev[0] if prev else 0) | bits, r["line"])
    return rows


def row_bytes(r):
    """((where, offset), bits) for each byte of the row's field the row names."""
    for i in range(r["size"]):
        bits = 0xFF if r["mask"] is None else (r["mask"] >> (8 * i)) & 0xFF
        if bits:
            yield (r["where"], r["offset"] + i), bits


def coverage(rows):
    """{(where, offset): bits} of every byte any row names, from main or from .data's start."""
    out = {}
    for r in rows:
        for (where, at), bits in row_bytes(r):
            key = (where, at - DATA_BEGIN if where == "data" else at)
            out[key] = out.get(key, 0) | bits
    return out


def slots(rows):
    """The image's slots in order: where, offset (from main, or from .data's start), size, dword
    (the first in the image), the three puppet classes' masks over the field (every bit of a field
    wider than four bytes is its one row's: that class's mask is then 0xFFFFFFFF) and the rows'
    names joined by '+'."""
    out, index, dword = [], {}, 0
    for r in rows:
        if r["carried"] != "globals":
            continue
        key = (r["where"], r["offset"], r["size"])
        if key not in index:
            for s in out:
                a = s["offset"] + (DATA_BEGIN if s["where"] == "data" else 0)
                if s["where"] == r["where"] and a < r["offset"] + r["size"] and r["offset"] < a + s["size"]:
                    raise Invalid(f"line {r['line']}: its field overlaps the slot of "
                                  f"{s['names']} with another extent")
            index[key] = len(out)
            out.append(dict(where=r["where"],
                            offset=r["offset"] - (DATA_BEGIN if r["where"] == "data" else 0),
                            size=r["size"], dword=dword, write=0, hold=0, react=0, names=r["name"]))
            dword += (r["size"] + 3) // 4
        else:
            out[index[key]]["names"] += "+" + r["name"]
        s = out[index[key]]
        s[r["puppet"]] |= 0xFFFFFFFF if r["mask"] is None else r["mask"]
    return out, dword


def mask_c(m):
    """A mask as C: each run of set bits shifted into place, so no literal of the header reads as
    an engine address to tools/thread-split-check.sh."""
    if m == 0xFFFFFFFF:
        return "0xFFFFFFFFu"
    runs, shift = [], 0
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


def render(tsv_bytes, gen_bytes, rows):
    digest = hashlib.sha256(tsv_bytes).hexdigest()
    gen_digest = hashlib.sha256(gen_bytes).hexdigest()
    sl, dwords = slots(rows)
    out = [
        "/* tagpu_replay_globals.h -- GENERATED from tagpu/ddraw/replay-globals.tsv by",
        "   tools/replay-globals-gen.py, which says what each column means. Edit the file and",
        "   regenerate, never this header: the build re-derives it and fails when the two disagree. */",
        "#ifndef TAGPU_REPLAY_GLOBALS_H",
        "#define TAGPU_REPLAY_GLOBALS_H",
        "",
        f'#define TAGPU_REPLAY_GLOBALS_SHA256 "{digest}"',
        f'#define TAGPU_REPLAY_GLOBALS_GEN_SHA256 "{gen_digest}"',
        "",
        f"#define REC_GLOBALS_DWORDS {dwords}    /* the globals image */",
        f"#define REC_GLOBALS_SLOTS  {len(sl)}",
        "",
        "enum { RG_MAIN, RG_DATA };   /* a slot's offset: from main, or from .data's start */",
        "",
        "/* One field of the globals image. Its bits are the union of the three masks, each over the",
        "   field as a little-endian integer; a field wider than four bytes is one row's, and that",
        "   row's class's mask is then 0xFFFFFFFF, every byte. */",
        "typedef struct ReplayGlobalSlot {",
        "    unsigned char  where;",
        "    unsigned char  dword;     /* its first dword in the image */",
        "    unsigned short size;      /* bytes */",
        "    unsigned int   offset;",
        "    unsigned int   write, hold, react;   /* what a puppet host does with each bit */",
        "    const char*    name;",
        "} ReplayGlobalSlot;",
        "",
        "static const ReplayGlobalSlot tagpu_replay_globals[REC_GLOBALS_SLOTS] = {",
    ]
    for s in sl:
        out.append(f"    {{ RG_{s['where'].upper()}, {s['dword']}, {s['size']}, {s['offset']:#07x}, "
                   f"{mask_c(s['write'])}, {mask_c(s['hold'])}, {mask_c(s['react'])}, "
                   f"\"{s['names']}\" }},")
    out += ["};", "", "#endif", ""]
    if sl and sl[-1]["dword"] > 0xFF:
        raise Invalid(f"a slot starts at dword {sl[-1]['dword']}, past the 255 its index holds")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="verify the header against the file")
    a = ap.parse_args()
    try:
        tsv = open(TSV, "rb").read()
    except OSError as e:
        print(f"replay-globals: cannot read {os.path.relpath(TSV, DDRAW)}: {e.strerror}", file=sys.stderr)
        return 2
    try:
        rows = parse(tsv.decode("utf-8"))
        gen = open(os.path.abspath(__file__), "rb").read()
        text = render(tsv, gen, rows)
    except (Invalid, UnicodeDecodeError) as e:
        print(f"replay-globals: {os.path.relpath(TSV, DDRAW)}: {e}", file=sys.stderr)
        return 1
    if not a.check:
        with open(HEADER, "w", newline="\n") as f:
            f.write(text)
        print(f"replay-globals: {len(rows)} rows, {slots(rows)[1]} dwords -> "
              f"{os.path.relpath(HEADER, DDRAW)}")
        return 0
    try:
        have = open(HEADER, encoding="utf-8").read()
    except OSError:
        print(f"replay-globals: {os.path.relpath(HEADER, DDRAW)} is missing -- run "
              f"tools/replay-globals-gen.py", file=sys.stderr)
        return 1
    if have == text:
        return 0
    def stamp(name, header):
        m = re.search(name + r' "([0-9a-f]+)"', header)
        return m.group(1) if m else None
    if stamp("GLOBALS_SHA256", have) != stamp("GLOBALS_SHA256", text):
        why = "replay-globals.tsv changed since the header was generated"
    elif stamp("GEN_SHA256", have) != stamp("GEN_SHA256", text):
        why = "tools/replay-globals-gen.py changed since the header was generated"
    else:
        why = "the header was edited by hand"
    print(f"replay-globals: inc/tagpu_replay_globals.h is stale ({why}) -- run "
          f"tools/replay-globals-gen.py", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
