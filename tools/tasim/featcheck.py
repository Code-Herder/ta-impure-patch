#!/usr/bin/env python3
"""featcheck.py -- every SIM_FEATURE of Impure's engine has a row in tools/tasim/engine-features.tsv, and every row
names one.

    python3 tools/tasim/featcheck.py [DDRAW]          DDRAW: tagpu/ddraw by default, as the DLL's build passes `.`
    python3 tools/tasim/featcheck.py --self-test      plants an unclassified feature and a stale row in a copy and
                                                      requires both refused

The features are tools/replay/tafeat.py's, the sim files' SIM_FEATURE lines, which the build already checks for
shape. A feature with no row is a simulation rule tasim has not been told about (the shared core models the engine as
Impure runs it, research/notes/impai/tasim.md); a row with no feature is a classification of something gone. Either
fails the build, named. The file's columns and values are its own header's.

Exit 0 clean, 1 a finding, 2 could not run."""
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools" / "replay"))
import tafeat  # noqa: E402

TSV = HERE / "engine-features.tsv"
COLUMNS = ["name", "tasim", "where", "evidence"]
CLASSES = {"modelled", "T1a", "T1b", "T1c", "T1d", "T2", "T3", "T4", "outside"}


def read_rows(path):
    rows, problems, header = {}, [], None
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cells = line.split("\t")
        if header is None:
            header = cells
            if header != COLUMNS:
                problems.append(f"HEADER {path.name}:{n}: the columns are {header}, not {COLUMNS}")
            continue
        if len(cells) != len(COLUMNS) or not all(c.strip() for c in cells):
            problems.append(f"MALFORMED {path.name}:{n}: {len(cells)} cells, every one of the {len(COLUMNS)} "
                            f"needs a value")
            continue
        name, cls = cells[0], cells[1]
        if cls not in CLASSES:
            problems.append(f"CLASS {path.name}:{n}: {name}'s tasim is {cls!r}, not one of {sorted(CLASSES)}")
        if name in rows:
            problems.append(f"DUPLICATE {path.name}:{n}: {name} is also row {rows[name]}")
        rows.setdefault(name, n)
    if header is None:
        problems.append(f"EMPTY {path.name}: no header")
    return rows, problems


def check(ddraw, tsv):
    feats, problems = tafeat.all_features(ddraw)
    if problems:
        for p in problems:
            print(f"featcheck: {p}")
        print("featcheck: tafeat could not read the features")
        return 2
    rows, findings = read_rows(tsv)
    names = {f["name"]: f for f in feats}
    for name, f in names.items():
        if name not in rows:
            findings.append(f"UNCLASSIFIED {f['file']}:{f['line']}: SIM_FEATURE {name} has no row in "
                            f"{tsv.name}: say whether tasim models it, which landing will, or why it is outside")
    for name, n in rows.items():
        if name not in names:
            findings.append(f"STALE {tsv.name}:{n}: {name} names no SIM_FEATURE")
    for p in findings:
        print(f"featcheck: {p}")
    if findings:
        return 1
    print(f"featcheck: clean -- {len(names)} features, each classified")
    return 0


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        ddraw = Path(tmp) / "ddraw"
        shutil.copytree(TREE / "tagpu" / "ddraw" / "src", ddraw / "src",
                        ignore=shutil.ignore_patterns("*.o", "*.d"))
        shutil.copy(TREE / "tagpu" / "ddraw" / "thread-split.allow", ddraw / "thread-split.allow")
        tsv = Path(tmp) / "engine-features.tsv"
        shutil.copy(TSV, tsv)
        if check(ddraw, tsv) != 0:
            print("featcheck self-test: the copy is not clean to begin with")
            return 1
        sim = ddraw / tafeat.sim_files(ddraw)[0]
        sim.write_text(sim.read_text() + '\nSIM_FEATURE(planted_feature, SIM_PUPPET_RUNS, "battle");\n'
                       'static void planted(void) {}\n')
        planted = check(ddraw, tsv)
        tsv.write_text(tsv.read_text() + "planted_feature\toutside\ta test\ta test\nstale_row\toutside\tgone\tgone\n")
        stale = check(ddraw, tsv)
        if planted == 1 and stale == 1:
            print("featcheck self-test: ok, the planted feature and the stale row are both refused")
            return 0
        print(f"featcheck self-test: FAILED (planted {planted}, stale {stale}; both must be 1)")
        return 1


def main(argv):
    if argv and argv[0] == "--self-test":
        return self_test()
    ddraw = Path(argv[0]).resolve() if argv else TREE / "tagpu" / "ddraw"
    try:
        return check(ddraw, TSV)
    except OSError as e:
        print(f"featcheck: could not run: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
