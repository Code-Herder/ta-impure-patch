#!/usr/bin/env python3
"""The playback's perspective arithmetic, the DLL's against its mirror.

Builds tools/replay/persp_test.c with tagpu/ddraw/src/tagpu_persp.c as the DLL compiles it, with the
host's C compiler (the file is engine-free and Win32-free, and every value in it is a fixed-width
integer, so the host computes what the DLL does), runs its checks, then hands it random cases and
requires every answer to equal tarec.py's persp_sample and persp_follow, field for field. The
playback's Follow is compared with `tarec.py pov --eye` in a live game; this is what makes that
comparison mean the arithmetic and not the mirror.

    python3 tools/replay/persp_test.py [--cases N] [--seed S] [--out DIR]
"""
import argparse
import random
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(HERE))
import tarec  # noqa: E402

I32 = (-0x80000000, 0x7FFFFFFF)


def sample(rng):
    """One RecPov's fields as the C program reads them, inside rec_pov_ok's bounds, with
    coordinates anywhere in int32 now and then."""
    wide = rng.random() < 0.1
    coord = (lambda: rng.randint(*I32)) if wide else (lambda: rng.randint(-4096, 20000))
    return (rng.randint(1, tarec.REC_POV_EVERY_MAX), rng.randint(0, 3),
            rng.choice((640, 800, 1024, 1280, 1920, 3840, rng.randint(1, tarec.REC_POV_VIEW_MAX))),
            rng.choice((480, 600, 768, 720, 1080, 2160, rng.randint(1, tarec.REC_POV_VIEW_MAX))),
            rng.randint(tarec.REC_POV_ZOOM_MIN, tarec.REC_POV_ZOOM_MAX),
            coord(), coord(), coord(), rng.randint(-512, 512), coord())


def case(rng):
    count = rng.choice((0, 1, 2, 2, 2))
    t0 = rng.randint(0, 5000)
    t1 = t0 + rng.randint(1, 70)
    n = max(0, rng.choice((t0, t1)) + rng.randint(-40, 80))
    view = (rng.choice((0, -1, 640, 1024, 1920, 3840, rng.randint(1, 8000))),
            rng.choice((0, 480, 768, 1080, 2160, rng.randint(1, 8000))))
    return count, (t0, t1), n, view, sample(rng), sample(rng)


def mirror(c):
    """The answer line tarec's mirror gives for case c, as the C program prints it."""
    count, ticks, n, view, a, b = c
    pack = [tarec.REC_POV.pack(0, f[0], f[1], 0, *f[2:]) for f in (a, b)]
    held = [(ticks[i], pack[i]) for i in range(count)]
    how, f = tarec.persp_sample(held, n)
    fit = tarec.persp_follow(f, *view) if f else None
    fields = (f[1], f[2], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]) if f else (0,) * 10
    return " ".join(str(v) for v in (how, *fields, 1 if fit else 0, *(fit or (0, 0, 0))))


def run(out, n_cases, seed):
    exe = out / "persp-test"
    subprocess.run(["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I",
                    TREE / "tagpu/ddraw/inc", "-o", exe, HERE / "persp_test.c",
                    TREE / "tagpu/ddraw/src/tagpu_persp.c"], check=True)
    got = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    print(got.strip())
    if "every check holds" not in got:
        raise RuntimeError("the C checks did not finish")
    rng = random.Random(seed)
    cases = [case(rng) for _ in range(n_cases)]
    text = "\n".join(" ".join(str(v) for v in (c[0], *c[1], c[2], *c[3], *c[4], *c[5]))
                     for c in cases) + "\n"
    (out / "cases.txt").write_text(text)
    lines = subprocess.run([exe, "cases"], input=text, check=True, capture_output=True,
                           text=True).stdout.splitlines()
    if len(lines) != len(cases):
        raise RuntimeError(f"{len(lines)} answers to {len(cases)} cases")
    bad = [(c, line, mirror(c)) for c, line in zip(cases, lines) if line != mirror(c)]
    for c, line, want in bad[:10]:
        print(f"  DIFFER {c}\n    C      {line}\n    mirror {want}")
    hows = Counter(tarec.PERSP_HOW[int(line.split()[0])] for line in lines)
    print(f"{len(cases)} cases (seed {seed}): "
          + ", ".join(f"{k} {v}" for k, v in sorted(hows.items()))
          + f"; {len(bad)} differ from the mirror")
    return not bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=4)
    ap.add_argument("--out", type=Path, help="keep the program and the cases here")
    args = ap.parse_args()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="persp-test-"))
    out.mkdir(parents=True, exist_ok=True)
    try:
        ok = run(out, args.cases, args.seed)
    except (OSError, subprocess.SubprocessError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if not args.out:
            shutil.rmtree(out, ignore_errors=True)
    print("PASS: the DLL's perspective arithmetic equals tarec's mirror" if ok else
          "FAIL: the mirror and the DLL disagree")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
