#!/usr/bin/env python3
"""codectest.py -- the stream codec's tests: tests/codectest.c (the C codec, under Wine or built
natively with the sanitizers) against tools/replay/reccodec.py (the Python reader).

    make -C tagpu/ddraw codectest && tagpu/ddraw/tests/codectest.py outcomes
    tagpu/ddraw/tests/codectest.py --native outcomes

    outcomes                 SECTIONS, DECLINED and MISMATCH from synthetic payloads, each followed
                             by a block that restarts (codectest.c)
    transcode IN OUT         every stream block decoded and written again through the writer's
                             codec: 0 declined and 0 mismatched expected
    verify IN                each stream block's records from the C decoder against the Python
                             reader's, which decodes twice -- skipping the resting units and
                             evaluating every candidate -- and the C sine table against its formula
    fuzz IN SEED N [CASES]   N mutated sectioned blocks decoded by C from the state each starts from
    fuzzdiff IN CASES        those cases decoded by the Python reader: the same accept or refuse
                             and the same records for every case C accepted
    sizes IN                 the sectioned blocks' bytes by section: raw, each section packed alone
                             with zstd 3 and with the format-7 writer's deflate (a 2 KB window), and
                             the blocks as written

--native builds tests/codectest-asan.exe with the host's gcc and -fsanitize=address,undefined and
runs that instead of tests/codectest.exe under Wine (WINEPREFIX: the main checkout's wineprefix/).
IN is a recording of format 8. Exit 0 when every check holds, 1 when one fails, 2 when it cannot
run. Needs numpy, numba and zstandard (devpy.ensure() re-executes under the shared venv).
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DDRAW = HERE.parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "replay"))
import devpy  # noqa: E402

NATIVE = HERE / "codectest-asan.exe"


def build_native():
    cmd = ["gcc", "-std=c99", "-Wall", "-Iinc", "-Isrc", "-O1", "-g", "-fsanitize=address,undefined",
           "-fno-sanitize-recover=all", "-o", str(NATIVE), "tests/codectest.c", "src/zstd.c"]
    srcs = [DDRAW / "tests/codectest.c", DDRAW / "src/tagpu_reccodec.c", DDRAW / "inc/tagpu_reccodec.h",
            DDRAW / "inc/tagpu_recfmt.h", DDRAW / "inc/tagpu_reclanes.h", DDRAW / "src/zstd.c"]
    if NATIVE.exists() and all(NATIVE.stat().st_mtime >= s.stat().st_mtime for s in srcs):
        return
    print("codectest: building", NATIVE.name, flush=True)
    subprocess.run(cmd, cwd=DDRAW, check=True)


def runner(native):
    if native:
        build_native()
        return [str(NATIVE)], dict(os.environ)
    exe = HERE / "codectest.exe"
    if not exe.exists():
        sys.exit("codectest: no tests/codectest.exe -- make -C tagpu/ddraw codectest")
    env = dict(os.environ)
    common = Path(subprocess.check_output(["git", "-C", str(REPO), "rev-parse", "--git-common-dir"],
                                          text=True).strip())
    env.setdefault("WINEPREFIX", str((REPO / common).resolve().parent / "wineprefix"))
    env.setdefault("WINEDEBUG", "-all")
    return ["wine", str(exe)], env


def run(native, *args, capture=False):
    cmd, env = runner(native)
    r = subprocess.run(cmd + [str(a) for a in args], env=env, text=True,
                       stdout=subprocess.PIPE if capture else None)
    return r.returncode, (r.stdout or "").replace("\r", "")


def fnv64(b):
    h = 14695981039346656037
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def framed(path):
    """codectest.c `decode`'s output: {ordinal: records}"""
    data, at, out = open(path, "rb").read(), 0, {}
    while at < len(data):
        o, _, _, _, n, _ = struct.unpack_from("<6I", data, at)
        out[o] = data[at + 24:at + 24 + n]
        at += 24 + n
    return out


def cmd_verify(native, src):
    import math
    import reccodec
    ok = True
    rc, out = run(native, "sin", capture=True)
    sin = [int(v) for v in out.split()]
    want = [round(8192 * math.sin(2 * math.pi * i / 512)) for i in range(512)]
    print(f"sine table: C {'==' if sin == want else '!='} round(8192 sin(2 pi i / 512))")
    ok &= rc == 0 and sin == want and list(reccodec.SIN) == want
    with tempfile.TemporaryDirectory() as tmp:
        t0 = time.time()
        rc, out = run(native, "decode", src, Path(tmp) / "c.bin", capture=True)
        tc = time.time() - t0
        if rc:
            print(out)
            return 2
        print(out.strip().splitlines()[-1], f"(wall {tc:.1f} s)")
        cmap = framed(Path(tmp) / "c.bin")
    for rest in (True, False):
        t0 = time.time()
        n = bad = 0
        for blk in reccodec.iter_blocks(src, rest=rest):
            if blk.kind != 2:
                continue
            n += 1
            if cmap.get(blk.ordinal) != blk.payload:
                bad += 1
                if bad <= 5:
                    print(f"  block {blk.ordinal} (GameTime {blk.first}..{blk.last}) differs")
        n_c = len(cmap)
        print(f"Python ({'skipping resting units' if rest else 'every candidate'}): {n} stream blocks, "
              f"{bad} differ from C's {n_c}, {time.time() - t0:.1f} s")
        ok &= bad == 0 and n == n_c
    return 0 if ok else 1


def cmd_fuzzdiff(src, cases_path):
    import reccodec
    data = open(cases_path, "rb").read()
    cases, at = {}, 0
    while at < len(data):
        o, typ, flags, n, ok, outn, h0, h1 = struct.unpack_from("<8I", data, at)
        at += 32
        if typ == 0:
            body = data[at:at + n]
            at += n
            ents = None
        else:
            ents = []
            for _ in range(3):
                has, ne = struct.unpack_from("<II", data, at)
                ents.append(data[at + 8:at + 8 + ne] if has else None)
                at += 8 + ne
            body = None
        cases.setdefault(o, []).append((typ, flags, body, ents, ok, outn, h0 | h1 << 32))
    stats = {"cases": 0, "agree": 0, "accepted": 0}
    bad = []

    def before(ordinal, flags, sec, frame):
        for typ, cflags, body, ents, ok, outn, h in cases.get(ordinal, ()):
            f = frame.clone()
            try:
                if typ == 0:
                    got = f.decode(cflags, body)
                else:
                    es = [(t, e) for t, e in zip((reccodec.RT_CODEC, reccodec.RT_PHYSICS, reccodec.RT_AIR), ents)
                          if e is not None]
                    prm = reccodec.params(es, frame.prm.nslots)
                    if not prm.usable:
                        raise reccodec.Unreadable("not usable")
                    f.restart(prm)
                    got = f.decode(reccodec.RBF_SECTIONS, sec)
                pok, pn, ph = 1, len(got), fnv64(got)
            except reccodec.Unreadable:
                pok, pn, ph = 0, 0, 0
            except Exception as e:                      # a Python reader bug, never an answer
                pok, pn, ph = -1, 0, repr(e)
            stats["cases"] += 1
            stats["accepted"] += ok
            same = pok == ok and (not ok or (pn == outn and ph == h))
            stats["agree"] += same
            if not same and len(bad) < 10:
                bad.append(f"block {ordinal} case type {typ} flags {cflags:#x}: C {'accepts' if ok else 'refuses'}, "
                           f"Python {['refuses', 'accepts', 'fails: ' + str(ph)][pok]}")
    for _ in reccodec.iter_blocks(src, before_stream=before):
        pass
    for b in bad:
        print(" ", b)
    print(f"fuzzdiff: {stats['cases']} cases, {stats['accepted']} accepted by C, "
          f"{stats['agree']} agree")
    return 0 if stats["agree"] == stats["cases"] and stats["cases"] else 1


def cmd_sizes(src):
    import reccodec
    import zstandard
    names = ["kinds", "units", "proj", "cells", "orders", "events", "other"]
    raw = [0] * 7
    z3 = [0] * 7
    defl = [0] * 7
    written = records = blocks = 0
    zc = zstandard.ZstdCompressor(level=3)

    def deflate(b):
        c = zlib.compressobj(6, zlib.DEFLATED, 11)
        return len(c.compress(b) + c.flush())

    def seen(ordinal, flags, sec, frame):
        nonlocal written
        if not flags & reccodec.RBF_SECTIONS:
            return
        lens = struct.unpack_from("<7I", sec, 8)
        at = 36
        for i, n in enumerate(lens):
            part = sec[at:at + n]
            raw[i] += n
            if n:
                z3[i] += len(zc.compress(part))
                defl[i] += deflate(part)
            at += n

    for blk in reccodec.iter_blocks(src, before_stream=seen):
        if blk.kind == 2:
            blocks += 1
            written += blk.packed
            records += len(blk.payload)
    MiB = 1 << 20
    print(f"{src}: {blocks} stream blocks, records {records / MiB:.3f} MiB written as {written / MiB:.3f} MiB")
    print(f"{'section':<8} {'raw MiB':>9} {'zstd3 MiB':>10} {'deflate MiB':>12}")
    for i, nm in enumerate(names):
        print(f"{nm:<8} {raw[i] / MiB:9.3f} {z3[i] / MiB:10.3f} {defl[i] / MiB:12.3f}")
    print(f"{'all':<8} {sum(raw) / MiB:9.3f} {sum(z3) / MiB:10.3f} {sum(defl) / MiB:12.3f}")
    return 0


def main(argv):
    native = "--native" in argv
    argv = [a for a in argv if a != "--native"]
    if not argv:
        print(__doc__)
        return 2
    cmd, args = argv[0], argv[1:]
    if cmd in ("outcomes", "transcode", "fuzz", "decode"):
        return run(native, cmd, *args)[0]
    devpy.ensure()
    if cmd == "verify" and len(args) == 1:
        return cmd_verify(native, args[0])
    if cmd == "fuzzdiff" and len(args) == 2:
        return cmd_fuzzdiff(*args)
    if cmd == "sizes" and len(args) == 1:
        return cmd_sizes(args[0])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
