#!/usr/bin/env python3
"""cobscantest.py -- tagpu_cobscan.c over every stock unit's script, against tools/ta3do.

    tagpu/ddraw/tests/cobscantest.py [-v]

Builds tests/cobscantest.c natively with the host's gcc, AddressSanitizer and
UndefinedBehaviorSanitizer (tagpu_cobscan.c is engine-free and Win32-free), reads every
`units/*.fbi` of the install tools/ta3do reads (TA3DO_GAMEDIR, or the main checkout's
tagpu/gamedir), and pipes each unit's script -- `scripts/<UnitName>.cob`, the path the engine
opens (0x42D8E5) -- and its model's piece names in the engine's order to the test. Nothing read
from the game is written to disk.

Checks, each a failure by unit name:
  - hides: the COB pieces tagpu_cobscan_hides leaves hidden, by name, are hidden_at_create's;
    and the dont-caches tagpu_cobscan_create(TAGPU_COB_DONTCACHE) leaves are
    dontcached_at_create's, with their mask by the same rule as the hides' below;
  - relocated: the script relocated as the engine's loader leaves it gives the same answers;
  - mask: where the engine's swap must land every hidden piece on the node of its own name
    (the script's piece names up to the last hidden one distinct, and each carried by exactly
    one node of the model), the nodes the mask hides are hidden_at_create's pieces. Any other
    unit's mask follows the engine's by-index rule, which ta3do does not model: it is counted,
    not compared;
  - the fuzz pass (truncations and overwritten dwords) runs without a sanitizer error.
Exit 0 when nothing failed."""
import struct
import subprocess
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
TOOLS = DDRAW.parent.parent / "tools"
EXE = HERE / "cobscantest-native.exe"   # `.exe` so the tree's *.exe ignore covers it
SRCS = [HERE / "cobscantest.c", DDRAW / "src/tagpu_cobscan.c", DDRAW / "src/tagpu_cobscan.h"]


def build():
    if EXE.exists() and all(EXE.stat().st_mtime >= s.stat().st_mtime for s in SRCS):
        return
    print("cobscantest: building", EXE.name, flush=True)
    subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Isrc", "-O1", "-g",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-o", str(EXE), "tests/cobscantest.c", "src/tagpu_cobscan.c"],
                   cwd=DDRAW, check=True)


def engine_order(blob):
    """The 3DO's piece names in the engine's order for a new unit (0x45AEC0): a node, its
    child's whole subtree, then its sibling's, the root's siblings included. None for a tree
    the file's bounds or tagpu_model_engine_order's 256 refuse."""
    names, stack, seen = [], [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off < 0 or off + 52 > len(blob) or len(names) >= 256:
            return None
        seen.add(off)
        name_off, = struct.unpack_from("<i", blob, off + 0x1C)
        sib, child = struct.unpack_from("<ii", blob, off + 0x2C)
        end = blob.find(b"\0", name_off)
        names.append(blob[name_off:end if end >= 0 else len(blob)])
        if sib:
            stack.append(sib)
        if child:
            stack.append(child)
    return names


def u32(n):
    return struct.pack("<I", n)


def main(argv):
    verbose = "-v" in argv
    ta3do = SourceFileLoader("ta3do", str(TOOLS / "ta3do")).load_module()
    assets = ta3do.Assets()
    build()
    units, records = [], []
    for path in sorted(assets.index):
        if not (path.startswith("units/") and path.endswith(".fbi")):
            continue
        stem = Path(path).stem
        try:
            fields = ta3do.fbi_fields(assets, stem)
        except (ta3do.HpiError, ValueError):
            continue
        unit = fields.get("unitname", stem).lower()
        script = f"scripts/{unit}.cob"
        if not assets.has(script):
            continue
        cob = assets.read(script)
        obj = fields.get("objectname", unit).lower().removesuffix(".3do")
        model = f"objects3d/{obj}.3do"
        nodes = engine_order(assets.read(model)) if assets.has(model) else None
        units.append((unit, cob, nodes))
        rec = u32(len(unit)) + unit.encode() + u32(len(cob)) + cob
        rec += u32(len(nodes or []))
        for n in nodes or []:
            rec += u32(len(n)) + n
        records.append(rec)
    if not units:
        print("cobscantest: no unit scripts found in", assets.root, file=sys.stderr)
        return 2
    r = subprocess.run([str(EXE)], input=b"".join(records), capture_output=True)
    sys.stderr.write(r.stderr.decode("latin-1"))
    if r.returncode:
        print(f"cobscantest: the test exited {r.returncode}", file=sys.stderr)
        return 1
    lines = r.stdout.decode("latin-1").splitlines()
    if len(lines) != len(units):
        print(f"cobscantest: {len(lines)} answers for {len(units)} units", file=sys.stderr)
        return 1
    failed, no_model = [], 0
    # per pair of opcodes: (what, ta3do's reader, units marking a piece, compared by name, by index)
    kinds = [["hides", ta3do.hidden_at_create, 0, 0, 0],
             ["dont-caches", ta3do.dontcached_at_create, 0, 0, 0]]
    for (unit, cob, nodes), line in zip(units, lines):
        f = line.split("\t")
        name, reloc = f[0], f[9]
        if name != unit:
            failed.append(f"{unit}: answered as {name}")
        if reloc != "same":
            failed.append(f"{unit}: the relocated script differs ({reloc})")
        if nodes is None:
            no_model += 1
        for kind, (pieces, mask, masked) in zip(kinds, ((f[2], f[3], f[4]), (f[6], f[7], f[8]))):
            what, ref = kind[0], kind[1]
            want = set(ref(cob))
            got = set(p for p in pieces.split(",") if p)
            kind[2] += bool(want)
            if got != want:
                failed.append(f"{unit}: {what} {sorted(got)}, ta3do {sorted(want)}")
            if nodes is None:
                continue
            parsed = ta3do._cob_read(cob)
            model = [n.decode("latin-1").lower() for n in nodes]
            if parsed is None or not want:
                if not (mask == "0" and not masked):
                    failed.append(f"{unit}: no {what}, yet mask {mask} {masked}")
                kind[3] += 1
                continue
            cobnames = parsed[2]
            upto = max(i for i, p in enumerate(cobnames) if p in want)
            head = cobnames[:upto + 1]
            if len(set(head)) == len(head) and all(model.count(p) == 1 for p in head):
                kind[3] += 1
                got_m = set(p for p in masked.split(",") if p)
                if mask != "1" or got_m != want:
                    failed.append(f"{unit}: {what} mask {mask} marks nodes {sorted(got_m)}, ta3do {sorted(want)}")
            else:
                kind[4] += 1
                if verbose:
                    print(f"cobscantest: {unit}: {what} by index, mask {mask} marks {masked}")
    for f in failed:
        print("cobscantest: FAIL", f)
    for what, _ref, marking, by_name, by_index in kinds:
        print(f"cobscantest: {what}: {len(units)} unit scripts ({marking} mark a piece at Create); "
              f"mask compared by name for {by_name}, by the engine's index rule only for {by_index}")
    print(f"cobscantest: no model for {no_model}; {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
