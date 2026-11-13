#!/usr/bin/env python3
"""tasim_wire_test.py -- build tests/tasim_wire_test.c three ways and hold the scenario wire parser to its cases.

    tagpu/ddraw/tests/tasim_wire_test.py [--keep DIR] [--golden FILE]

The parser (src/tasim_wire.h) is built into the DLL and natively, so the test is built as tests/tasim_fixed_test.py
builds its own: `host`, gcc -O2; `asan`, the sanitizers; `wine`, the DLL's compiler i686-w64-mingw32-gcc with the
DLL's flags and the core's SSE2 ones, run under Wine in the wiki checks' prefix (tools/wiki/wineprefix.py).

The wires: every scenario in scenarios/ (calib/ too) compiled by tacli's own compiler and wire writer, as `tacli
scenario apply` sends it; the calibration runs' wires of every calibration scenario, runs 0 and 1 (the second swaps
the seats), as tools/tasim/perturb.py makes them for the engine runner, with every type taken as mobile so each
unit draws its jitter; and the cases below, which name what each must parse to or the error it must fail with.
Every build prints one canonical dump of every wire, and the three must be byte-equal. --keep DIR leaves the wires
in DIR; --golden FILE also requires the dump to equal FILE's.

Exit 0 when the builds agree and every case holds, 1 when not, 2 when a build cannot be made or run."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
TREE = DDRAW.parents[1]
SRCS = ["tests/tasim_wire_test.c", "src/tasim_wire.c"]
DEPS = [DDRAW / s for s in SRCS] + [DDRAW / "src/tasim_wire.h"]
SSE = ["-msse2", "-mfpmath=sse", "-mincoming-stack-boundary=2"]
BUILDS = {   # `.exe` so the tree's *.exe ignore covers each
    "host": (["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc"], [], "tasim_wire_test-host.exe"),
    "asan": (["gcc", "-std=c99", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Isrc", "-fsanitize=address,undefined",
              "-fno-sanitize-recover=all"], [], "tasim_wire_test-asan.exe"),
    "wine": (["i686-w64-mingw32-gcc", "-std=c99", "-O2", "-Wall", "-Isrc", *SSE], ["-static"],
             "tasim_wire_test-wine.exe"),
}

V1 = "v1 seed=1 clear=1 onerror=abort\n"
UNIT = "unit 0 ARMPW 0 100 200 - - - - - - - -\n"

# (name, wire, the error it must fail with, or None when it must parse)
CASES = [
    ("ok-every-line",
     "# a comment\r\n\r\nv1 seed=-7 clear=0 onerror=skip units=2 feats=1\r\nmap  Two Continents \r\n"
     "limit 1500\nsw noshake=1 shootall=1 clock=0 radar=0\nclearfeat 1\nobjective on\n"
     "player 3 metal=5000 energy=- ai=off mstore=1e3 estore=0.1\nplayer 9\n"
     "unit 0 armcom 1 775 1098 - 90 50 roam 40 7 at_will 5\nunit 1 CORAK 0 1 2 3 -4 - hold - 65536 hold -\n"
     "feat 0 ARMCOM_DEAD 1700 1200 - 45\n"
     "order 0 attack pos 2400 1200 after=7\norder sel stop\norder @12:ARMPW reclaim feat 0\n"
     "order @3 attack unit @44:CORAK after=255\norder 1 move pos 0 0\norder 1 stop after=0\n"
     "order 0 defend unit 1\norder 0 buildweapon count 5 after=3\norder @2 buildweapon count 200\n"
     "cam unit 1 pin=1 x=3\nend\n# after the end\n", None),
    ("ok-cam-pos", V1 + "cam pos 10 20\nend", None),
    ("ok-cam-feat", V1 + "feat 0 X 1 2 - -\ncam feat 0 pin=0\nend\n", None),
    ("ok-out-of-range", V1 + "unit 0 A 0 99999999999 5 -99999999999 -2147483647 - - - - - -\nend\n", None),
    ("ok-long-names", V1 + "map " + "M" * 200 + "\nunit 0 " + "T" * 40 + " 0 1 2 - - - - - - - -\n"
     "order @1:" + "U" * 40 + " stop\nend\n", None),
    ("ok-storage-spellings", V1 + "player 0 mstore=0 estore=2.9999999999999997e38\nplayer 1 mstore=0.30000001 estore=16777217\n"
     "player 2 mstore=+5 estore=1.5e-45\nplayer 3 mstore=0x10 estore=1e-400\nend\n", None),
    ("no-version", "unit 0 A 0 1 2 - - - - - -\nend\n", "line 1: expected the version line `v1 ...`, got unit"),
    ("v1-not-kv", "v1 seed\nend\n", "line 1: `seed` is not name=value"),
    ("v1-unknown-key", "v1 colour=3\nend\n", "line 1: unknown v1 key `colour`"),
    ("v1-bad-number", "v1 seed=12x\nend\n", "line 1: `seed` is not name=value"),
    ("no-end", V1 + UNIT, "the file has no `end` marker -- it is truncated, nothing was applied"),
    ("after-end", V1 + "end\nunit 0 A 0 1 2 - - - - - -\n", "line 3: unit after the `end` marker"),
    ("unknown-verb", V1 + "spawn 0\nend\n", "line 2: unknown wire verb `spawn`"),
    ("limit-missing", V1 + "limit\nend\n", "line 2: limit wants one number"),
    ("limit-unset", V1 + "limit -\nend\n", "line 2: limit wants one number"),
    ("sw-not-kv", V1 + "sw noshake\nend\n", "line 2: `noshake` is not name=0|1"),
    ("sw-unknown", V1 + "sw godmode=1\nend\n", "line 2: no engine switch named `godmode`"),
    ("player-slot", V1 + "player 10\nend\n", "line 2: player wants a slot 0..9"),
    ("player-eleven", V1 + "".join(f"player {i % 10}\n" for i in range(11)) + "end\n",
     "line 12: more than ten player lines"),
    ("player-storage-negative", V1 + "player 0 mstore=-1 estore=1\nend\n",
     "line 2: `mstore=-1` wants a finite storage of 0 or more"),
    ("player-storage-huge", V1 + "player 0 mstore=1 estore=4e38\nend\n",
     "line 2: `estore=4e38` wants a finite storage of 0 or more"),
    ("player-storage-bound", V1 + "player 0 mstore=1 estore=3e38\nend\n",
     "line 2: `estore=3e38` wants a finite storage of 0 or more"),
    ("player-storage-nan", V1 + "player 0 mstore=nan estore=1\nend\n",
     "line 2: `mstore=nan` wants a finite storage of 0 or more"),
    ("player-storage-half", V1 + "player 0 mstore=5\nend\n", "line 2: a player's storage wants both mstore= and estore="),
    ("player-not-kv", V1 + "player 0 ai\nend\n", "line 2: `ai` is not name=value"),
    ("player-unknown", V1 + "player 0 colour=2\nend\n", "line 2: unknown player key `colour`"),
    ("clearfeat-two", V1 + "clearfeat 2\nend\n", "line 2: clearfeat wants 0 or 1"),
    ("clearfeat-extra", V1 + "clearfeat 1 1\nend\n", "line 2: clearfeat wants 0 or 1"),
    ("objective-bad", V1 + "objective maybe\nend\n", "line 2: objective wants off or on"),
    ("unit-ordinal", V1 + "unit 1 A 0 1 2 - - - - - -\nend\n", "line 2: unit ordinals must run 0,1,2... (expected 0)"),
    ("unit-no-type", V1 + "unit 0\nend\n", "line 2: unit 0 has no type"),
    ("unit-short", V1 + "unit 0 A 0 1 2 - - - -\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-unset-owner", V1 + "unit 0 A - 1 2 - - - - - - - -\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-stance", V1 + "unit 0 A 0 1 2 - - - 2 - - - -\nend\n", "line 2: unit 0 has a malformed column"),
    ("ok-fire-stock", V1 + "unit 0 A 0 1 2 - - - - - - return 200\nunit 1 A 0 1 2 - - - - - - hold 0\nend\n",
     None),
    ("unit-no-fire", V1 + "unit 0 A 0 1 2 - - - - - -\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-fire-word", V1 + "unit 0 A 0 1 2 - - - - - - fire -\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-fire-number", V1 + "unit 0 A 0 1 2 - - - - - - 2 -\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-stock-high", V1 + "unit 0 A 0 1 2 - - - - - - - 201\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-stock-negative", V1 + "unit 0 A 0 1 2 - - - - - - - -1\nend\n",
     "line 2: unit 0 has a malformed column"),
    ("unit-past-end", V1 + "unit 0 A 0 1 2 - - - - - - - - x\nend\n", "line 2: unit 0 has a malformed column"),
    ("unit-count", "v1 units=2\n" + UNIT + "end\n", "the header promised 2 units and the file carries 1"),
    ("feat-ordinal", V1 + "feat 3 X 1 2 - -\nend\n", "line 2: feature ordinals must run 0,1,2... (expected 0)"),
    ("feat-no-type", V1 + "feat 0\nend\n", "line 2: feature 0 has no type"),
    ("feat-short", V1 + "feat 0 X 1 2 -\nend\n", "line 2: feature 0 has a malformed column"),
    ("feat-count", "v1 feats=1\nend\n", "the header promised 1 features and the file carries 0"),
    ("order-no-subject", V1 + "order\nend\n", "line 2: order wants a subject (an ordinal, sel, or @index)"),
    ("order-bad-live", V1 + "order @x stop\nend\n", "line 2: order subject `@x` is not @<index> or @<index>:<TYPE>"),
    ("order-live-colon", V1 + "order @4: stop\nend\n",
     "line 2: order subject `@4:` is not @<index> or @<index>:<TYPE>"),
    ("order-subject", V1 + "order x stop\nend\n", "line 2: order wants a unit ordinal"),
    ("order-no-command", V1 + "order 0\nend\n", "line 2: order 0 has no command"),
    ("order-command", V1 + "order 0 fight\nend\n", "line 2: no engine order named `fight`"),
    ("order-after", V1 + "order 0 stop after=256\nend\n", "line 2: order 0 `after=256` wants after=0..255"),
    ("order-pos", V1 + "order 0 move pos 1\nend\n", "line 2: order 0 pos wants x and y"),
    ("order-unit-target", V1 + "order 0 attack unit @y\nend\n",
     "line 2: order 0 target `@y` is not @<index> or @<index>:<TYPE>"),
    ("order-feat-target", V1 + "order 0 reclaim feat @1\nend\n", "line 2: order 0 feat wants an ordinal"),
    ("order-target-kind", V1 + "order 0 move here 1 2\nend\n",
     "line 2: order target must be pos/unit/feat/count, got `here`"),
    ("order-buildweapon-alone", V1 + "order 0 buildweapon\nend\n",
     "line 2: order 0: buildweapon takes a count and nothing else, and only buildweapon takes one"),
    ("order-buildweapon-pos", V1 + "order 0 buildweapon pos 1 2\nend\n",
     "line 2: order 0: buildweapon takes a count and nothing else, and only buildweapon takes one"),
    ("order-count-move", V1 + "order 0 move count 3\nend\n",
     "line 2: order 0: buildweapon takes a count and nothing else, and only buildweapon takes one"),
    ("order-count-zero", V1 + "order 0 buildweapon count 0\nend\n", "line 2: order 0 count wants 1..200"),
    ("order-count-high", V1 + "order 0 buildweapon count 201\nend\n", "line 2: order 0 count wants 1..200"),
    ("order-count-unset", V1 + "order 0 buildweapon count -\nend\n", "line 2: order 0 count wants 1..200"),
    ("order-target-after", V1 + "order 0 move pos 1 2 later\nend\n", "line 2: order 0 `later` wants after=0..255"),
    ("order-past-end", V1 + "order 0 move pos 1 2 after=1 x\nend\n", "line 2: order 0 has a column past its end"),
    ("cam-none", V1 + "cam\nend\n", "line 2: cam wants pos/unit/feat"),
    ("cam-pos", V1 + "cam pos 1\nend\n", "line 2: cam pos wants x and y"),
    ("cam-unit", V1 + "cam unit -\nend\n", "line 2: cam unit wants an ordinal"),
    ("cam-kind", V1 + "cam eye 1 2\nend\n", "line 2: cam target must be pos/unit/feat"),
    ("cap-units", V1 + "".join(f"unit {i} A 0 1 2 - - - - - - - -\n" for i in range(15001)) + "end\n",
     "more than 15000 units -- the fork's own ceiling"),
    ("cap-feats", V1 + "".join(f"feat {i} X 1 2 - -\n" for i in range(1025)) + "end\n",
     "more than 1024 features -- the fork's own ceiling"),
    ("cap-orders", V1 + "order 0 stop\n" * 15001 + "end\n", "more than 15000 orders -- the fork's own ceiling"),
]


def build(name):
    flags, libs, exe = BUILDS[name]
    out = HERE / exe
    if out.exists() and all(out.stat().st_mtime >= d.stat().st_mtime for d in DEPS):
        return out
    print(f"tasim_wire_test: building {exe}", flush=True)
    if name == "wine":   # the core's file on SSE2, as the Makefile compiles every src/tasim_*.c
        obj = HERE / "tasim_wire-wine.o"
        subprocess.run([*flags, "-c", "-o", str(obj), "src/tasim_wire.c"], cwd=DDRAW, check=True)
        subprocess.run([*flags[:-len(SSE)], "-o", str(out), SRCS[0], str(obj), *libs], cwd=DDRAW, check=True)
        obj.unlink()
    else:
        subprocess.run([*flags, "-o", str(out), *SRCS, *libs], cwd=DDRAW, check=True)
    return out


def wine_env():
    r = subprocess.run([sys.executable, str(TREE / "tools/wiki/wineprefix.py")], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"no Wine prefix: {r.stderr.strip()}")
    return {**os.environ, "WINEPREFIX": r.stdout.strip().splitlines()[-1], "WINEDEBUG": "-all"}


def run(name, exe, files):
    if name == "wine":
        cmd, env, paths = ["wine", str(exe)], wine_env(), [f"Z:{f}" for f in files]
    else:
        cmd, env, paths = [str(exe)], None, list(map(str, files))
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, input="".join(p + "\n" for p in paths))
    return r.returncode, r.stdout.replace("\r\n", "\n"), r.stderr


def tree_wires():
    """(name, wire) of every scenario and of the calibration runs; the names that tacli refused, each with why."""
    sys.path.insert(0, str(TREE / "tools" / "tasim"))
    import perturb  # noqa: E402  (loads tools/tacli as perturb.CLI)
    import calibref  # noqa: E402
    cli, wires, refused = perturb.CLI, [], []
    for path in sorted((TREE / "scenarios").glob("**/*.json")):
        rel = path.relative_to(TREE / "scenarios").as_posix()
        try:
            wires.append((rel.replace("/", "-")[:-5] + ".wire", cli._scn_wire(cli._scn_compile(str(path)))))
        except SystemExit as e:
            refused.append(f"{rel}: {e}")
    arena = calibref.arena()
    for path in sorted((TREE / "scenarios" / "calib").glob("*.json")):
        doc = json.loads(path.read_text())
        if "calib" not in doc or path.resolve() == (TREE / arena["scenario"]).resolve():
            continue
        try:   # the lanes' own clear, end and probe wires carry a block that is not a calibration's
            calibref.calib_block(doc, path.name, arena)
        except calibref.CalibError:
            continue
        types = [u["type"] for u in cli._scn_compile(str(path))["units"]]
        values = {t.upper(): {"bmcode": 1} for t in types}
        for index in (0, 1):
            _exp, _rec, wire, _clauses = perturb.run_wire(path, index, values, arena)
            wires.append((f"calib-run-{path.stem}-{index}.wire", wire))
    return wires, refused


def main(argv):
    keep = Path(argv[argv.index("--keep") + 1]) if "--keep" in argv else None
    golden = Path(argv[argv.index("--golden") + 1]) if "--golden" in argv else None
    try:
        exes = {n: build(n) for n in BUILDS}
    except (subprocess.CalledProcessError, OSError) as e:
        print(f"tasim_wire_test: a build failed: {e}")
        return 2
    wires, refused = tree_wires()
    for r in refused:
        print(f"tasim_wire_test: tacli refused {r}")
    wires += [(f"case-{n}.wire", w) for n, w, _ in CASES]
    with tempfile.TemporaryDirectory() as tmp:
        d = keep or Path(tmp)
        d.mkdir(parents=True, exist_ok=True)
        files = []
        for name, text in wires:
            (d / name).write_bytes(text.encode())
            files.append(d / name)
        dumps, bad = {}, 0
        try:
            for name, exe in exes.items():
                code, out, err = run(name, exe, files)
                dumps[name] = out
                if code != 0:
                    print(f"{name}: exit {code}\n{err.strip()}")
                    bad = 1
        except (RuntimeError, OSError) as e:
            print(f"tasim_wire_test: a build cannot run: {e}")
            return 2
    ref = dumps["host"]
    for name, out in dumps.items():
        if out != ref:
            a, b = ref.splitlines(), out.splitlines()
            i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
            print(f"tasim_wire_test: {name} differs from host at dump line {i + 1}:\n  host: "
                  f"{a[i] if i < len(a) else '<end>'}\n  {name}: {b[i] if i < len(b) else '<end>'}")
            bad = 1
    if golden and golden.read_text() != ref:
        print(f"tasim_wire_test: the dump differs from {golden}")
        bad = 1
    blocks = {}
    for blk in ref.split("== ")[1:]:
        head, _, rest = blk.partition("\n")
        blocks[head] = rest
    for name, _wire, want in CASES:
        rest = blocks.get(f"case-{name}.wire", "")
        ok = rest.startswith("ok 1 errors 0 dropped 0\n")
        errs = [ln[4:] for ln in rest.splitlines() if ln.startswith("err ")]
        if want is None and not ok:
            print(f"tasim_wire_test: case {name} must parse; it gave {errs}")
            bad = 1
        elif want is not None and (ok or errs != [want]):
            print(f"tasim_wire_test: case {name} must fail with {want!r}; it gave {errs}")
            bad = 1
    for name, _w in wires:
        if not name.startswith("case-") and not blocks.get(name, "").startswith("ok 1 errors 0 "):
            print(f"tasim_wire_test: {name} did not parse: "
                  f"{[ln for ln in blocks.get(name, '').splitlines() if ln.startswith('err ')]}")
            bad = 1
    print(f"tasim_wire_test: {len(wires)} wires ({len(CASES)} cases), {len(refused)} scenarios tacli refused")
    print("tasim_wire_test: FAILED" if bad else "tasim_wire_test: ok, the three builds agree and every case holds")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
