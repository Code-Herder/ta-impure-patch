#!/usr/bin/env python3
"""A unit reclaim's step across stock's 32-bit wrap (B10) -- one scripted run, no hand driving.

    tools/b10-reclaim-wrap.py [out-dir]     (instance $B10_INSTANCE, default b10rc, on the
                                             private Xvfb $B10_DISPLAY, default :173)

Loads scenarios/b10-reclaim-wrap.json three times -- with the main checkout's ddraw.dll (the
build before B10), this checkout's ddraw.dll and this checkout's ddraw-stocklimits.dll -- and
each time samples the four CORKROG targets' HP (+0x108) as their ARMCOMs (0, 150, 155 and 1000
kills) reclaim them. A reclaim takes one step every 15 ticks, so the differences between a
target's successive HP values are its step. Each step is compared with 0x438650's formula,
floor(workertime * (kills + 5)/5 * MaxHitPoints * 15 / (max(def +0x18A, 10) * 300)), exact (B10)
and with the product taken modulo 2^32 (stock), from values read out of the running game.

Verdicts: on the build before B10 the steps at 0 and 150 kills are the formula's and the step at
155 is the wrapped one; on both B10 builds the steps at 0 and 150 equal the build before's, and
the steps at 155 and 1000 are the exact formula's. Prints the wall time it measured.
"""
import json
import os
import struct
import subprocess
import sys
import time

REPO = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(__file__)), "rev-parse",
                       "--show-toplevel"], capture_output=True, text=True).stdout.strip()
COMMON = subprocess.run(["git", "-C", REPO, "rev-parse", "--git-common-dir"],
                        capture_output=True, text=True).stdout.strip()
MAIN = os.path.normpath(os.path.join(REPO, COMMON, ".."))
T = os.path.join(REPO, "tools", "tacli")
INST = os.environ.get("B10_INSTANCE", "b10rc")
DISP = os.environ.get("B10_DISPLAY", ":173")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TMPDIR", "/tmp"),
                                                          "b10-reclaim-wrap")
KILLS = [0, 150, 155, 1000]
SAMPLE_S = 9.0
RUNS = [("before B10 (main checkout)", os.path.join(MAIN, "tagpu", "ddraw", "ddraw.dll")),
        ("B10", os.path.join(REPO, "tagpu", "ddraw", "ddraw.dll")),
        ("B10 stock limits", os.path.join(REPO, "tagpu", "ddraw", "ddraw-stocklimits.dll"))]


def tacli(*args, timeout=180):
    r = subprocess.run([T, *args], capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def peek(*specs):
    """Values of each spec, as unsigned ints, in order."""
    rc, out = tacli("peek", INST, *specs, timeout=30)
    vals = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] in specs:
            vals.append(int(parts[2]))
    if rc != 0 or len(vals) != len(specs):
        raise RuntimeError("peek failed: " + out.strip()[-200:])
    return vals


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def instance_gamedir():
    rc, out = tacli("ls", "--json")
    for r in json.loads(out[out.index("["):]) if rc == 0 else []:
        if r["name"] == INST:
            return r["gamedir"]
    return None


def one_run(label, dll):
    t0 = time.time()
    gd = instance_gamedir()
    subprocess.run(["cp", dll, os.path.join(gd, "ddraw.dll")], check=True)
    rc, out = tacli("scenario", "load", INST, os.path.join(REPO, "scenarios", "b10-reclaim-wrap.json"),
                    "--defaults", "--los", "0", "--restart", "--keep-dll", timeout=300)
    open(os.path.join(OUT, label.replace(" ", "_") + "-load.txt"), "w").write(out)
    if rc != 0:
        return {"label": label, "error": "load failed: " + out.strip()[-300:]}
    log = open(os.path.join(gd, "log", "tagpu.log"), errors="replace").read()
    b10_line = [l for l in log.splitlines() if l.startswith("enginefix: B10")]
    limits = [l for l in log.splitlines() if l.startswith("limits: ")]
    base = peek("*0x511DE8+0x14357:4")[0]

    # the scenario's order: the player's block from engine index 2, a reclaimer then its target
    pairs = []
    for n, k in enumerate(KILLS):
        r = 2 + 2 * n
        pairs.append((k, r, r + 1))
    unit = lambda i, off: "0x%X" % (base + i * 0x118 + off)
    facts = {}
    for k, r, t in pairs:
        rdef = peek(unit(r, 0x92) + ":4")[0]
        tdef = peek(unit(t, 0x92) + ":4")[0]
        wt, kills = peek("0x%X:2" % (rdef + 0x1FE), unit(r, 0xB8) + ":2")
        cost_u, maxhp = peek("0x%X:4" % (tdef + 0x18A), "0x%X:4" % (tdef + 0x1FA))
        facts[k] = {"workertime": wt, "kills": kills, "cost": f32(cost_u), "maxhp": maxhp}

    series = {k: [] for k in KILLS}
    end = time.time() + SAMPLE_S
    specs = [unit(t, 0x108) + ":2" for _, _, t in pairs]
    while time.time() < end:
        for (k, _, _), hp in zip(pairs, peek(*specs)):
            s = series[k]
            if not s or s[-1] != hp:
                s.append(hp)
        time.sleep(0.05)
    tacli("stop", INST)

    res = {"label": label, "dll": dll, "b10_line": b10_line[:1], "limits": limits[:1], "kills": {}}
    for k in KILLS:
        f = facts[k]
        factor = (f["kills"] + 5) // 5
        prod = f["workertime"] * factor * f["maxhp"] * 15
        den = max(f["cost"], 10.0) * 300.0
        exact = max(int(prod / den), 1)
        wrapped = max(int((prod % 2**32) / den), 1)
        s = series[k]
        drops = [a - b for a, b in zip(s, s[1:]) if 0 < a - b and b > 0]
        step = max(set(drops), key=drops.count) if drops else None
        res["kills"][k] = {**f, "factor": factor, "product": prod, "exact": exact,
                           "wrapped": wrapped, "step": step, "drops": drops[:12], "hp": s[:6]}
    res["wall_s"] = round(time.time() - t0, 1)
    return res


def near(a, b):
    return a is not None and abs(a - b) <= 1


def main():
    start = time.time()
    os.makedirs(OUT, exist_ok=True)
    xvfb = None
    if subprocess.run(["xdpyinfo", "-display", DISP], capture_output=True).returncode != 0:
        xvfb = subprocess.Popen(["Xvfb", DISP, "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(40):
            if subprocess.run(["xdpyinfo", "-display", DISP], capture_output=True).returncode == 0:
                break
            time.sleep(0.25)
    try:
        if instance_gamedir() is None:
            rc, out = tacli("create", INST, "--display", DISP)
            if rc != 0:
                print("FAIL: create:", out.strip()[-300:])
                return 1
        results = [one_run(label, dll) for label, dll in RUNS]
    finally:
        tacli("stop", INST)
        if xvfb:
            xvfb.terminate()
    json.dump(results, open(os.path.join(OUT, "results.json"), "w"), indent=1)

    ok = True
    for r in results:
        print("== %s (%s s)" % (r["label"], r.get("wall_s")))
        if "error" in r:
            print("  FAIL:", r["error"])
            ok = False
            continue
        print("  " + (r["b10_line"][0] if r["b10_line"] else "no 'enginefix: B10' line"))
        print("  " + (r["limits"][0][:90] if r["limits"] else "no limits line"))
        for k in KILLS:
            e = r["kills"][k]
            print("  kills %4d factor %3d: step %s  exact %d  wrapped %d  (wt %d, maxhp %d, cost %.1f; "
                  "drops %s)" % (k, e["factor"], e["step"], e["exact"], e["wrapped"], e["workertime"],
                                 e["maxhp"], e["cost"], e["drops"][:6]))
    before, b10, stock = results
    if all("error" not in r for r in results):
        def verdict(name, cond):
            nonlocal ok
            ok = ok and cond
            print("%s  %s" % ("PASS" if cond else "FAIL", name))
        kb, k1, ks = before["kills"], b10["kills"], stock["kills"]
        verdict("before B10: 0 and 150 kills take the formula's step",
                near(kb[0]["step"], kb[0]["exact"]) and near(kb[150]["step"], kb[150]["exact"]))
        verdict("before B10: 155 kills takes the wrapped step (%s, not %d)"
                % (kb[155]["step"], kb[155]["exact"]),
                near(kb[155]["step"], kb[155]["wrapped"]) and kb[155]["wrapped"] < kb[155]["exact"])
        for name, kr, r in (("B10", k1, b10), ("B10 stock limits", ks, stock)):
            line = r["b10_line"][0] if r["b10_line"] else ""
            lim = r["limits"][0] if r["limits"] else ""
            verdict("%s: B10 is in the fail-closed table and the table installed" % name,
                    "fail-closed table" in line and "FAILED" not in lim and lim != "")
            verdict("%s: 0 and 150 kills identical to the build before (%s, %s)"
                    % (name, kr[0]["step"], kr[150]["step"]),
                    kr[0]["step"] == kb[0]["step"] and kr[150]["step"] == kb[150]["step"])
            verdict("%s: 155 and 1000 kills take the exact formula's step (%s ~ %d, %s ~ %d)"
                    % (name, kr[155]["step"], kr[155]["exact"], kr[1000]["step"], kr[1000]["exact"]),
                    near(kr[155]["step"], kr[155]["exact"]) and near(kr[1000]["step"], kr[1000]["exact"]))
    print("b10 wall %d s" % (time.time() - start))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
