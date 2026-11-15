#!/usr/bin/env python3
"""Allied sight and radar in a two-peer network game, an AI ally on the other peer among them.

    tools/mp_allysight.py allied|control --out DIR [--dll DLL] [--port N]

H hosts a human (its record 0) and AI2; J joins as a human and hosts AI3, everyone's enemy.
`allied`: H and J allied by the battle room's Ally buttons (mp_lobby.sh --allies), AI2 and J by
one team (TEAMICONS), H and AI2 not allied. `control`: no alliance. On H an AI2 radar tower stands
in the map's middle; on J an AI3 unit E stands inside its radar and outside its sight. Then, on
the peers that hold them:

    H's counters at J's commander       shared when H and J are allied (human and human)
    J's counters at AI2's commander     shared when AI2 and J are allied (an AI to a remote human)
    AI2's counters on H at J's commander  the same alliance the other way, on the AI's own peer
    H's counters at AI2's commander     never: no ally of H sees there, and sharing is not transitive
    J's view: E's +0x110 bit 8          a contact only through AI2's shared radar

and in `allied`, an alliance changed in the middle of the game: J opens the in-game ALLIES dialog
(Tab, then ALLIES: a network game's only, 0x496144, 0x4950FA) and clears its row toward H
(LIVEALLY<H's record on J>, 0x447225), so the pair is no longer allied both ways and H's counters
at J's commander fall to zero while AI2 and J still share; J then allies H again and they come back.

Each is checked against what the variant expects; the exit status is 0 when all hold, 1 when one
fails. research/notes/modules/allied-sharing.md, *In a network game*, holds the measured run.
The tool holds the run lock (tools/runlock.py) and owns two fresh instances, a private display
and its DirectPlay port.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

TREE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TREE / "tools" / "replay"))
import tareplay  # noqa: E402  the run lock, the private display, the repository's runtime

CLI = TREE / "tools" / "tacli"
REC0, REC_STRIDE = 0x1B63, 0x14B      # player record p = main + 0x1B63 + p*0x14B
REC_COUNTERS, REC_COLS, REC_ROW = 0x7C, 0x80, 0x108
UNITS, U_STRIDE, U_CONTACT = 0x14357, 0x118, 0x110
COMMANDERS = ("ARMCOM", "CORCOM")
TOWER_AT = (5376, 6400)               # the middle of Two Continents, away from every start
E_OFFSET = 1100                        # inside an ARMRAD's radar, outside its sight


class Run:
    def __init__(self, out):
        self.log = (out / "driver.log").open("w")

    def __call__(self, *args, timeout=240, env=None, check=True):
        r = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                           timeout=timeout, env=env)
        self.log.write(f"$ {' '.join(map(str, args))}\n{r.stdout}{r.stderr}\n")
        self.log.flush()
        if check and r.returncode:
            raise RuntimeError(f"failed ({r.returncode}): {' '.join(map(str, args))}\n"
                               f"{r.stderr[-800:]}")
        return r.stdout

    def tacli(self, *args, **kw):
        return self(sys.executable, CLI, *args, **kw)

    def peek(self, name, *specs):
        """the unsigned values, None where the game could not read"""
        rows = json.loads(self.tacli("peek", name, *specs, "--json"))["peeks"]
        return [int(m.group(1)) if (m := re.match(r"^(\d+)", r["value"] or "")) else None
                for r in rows]

    def roster(self, name):
        return json.loads(self.tacli("roster", name, "--json"))["units"]

    def counter(self, name, record, x, z):
        """record's sight counter over world (x, z): 32-wu cells, row-major by the record's +0x80"""
        base = REC0 + record * REC_STRIDE
        grid, cols = self.peek(name, f"*0x511DE8+{base + REC_COUNTERS:X}:4",
                               f"*0x511DE8+{base + REC_COLS:X}:4")
        if not grid or not cols:
            return None
        return self.peek(name, f"{grid + (z >> 5) * cols + (x >> 5):X}:1")[0]

    def contact(self, name, slot):
        (units,) = self.peek(name, f"*0x511DE8+{UNITS:X}:4")
        (flags,) = self.peek(name, f"{units + slot * U_STRIDE + U_CONTACT:X}:4")
        return None if flags is None else bool(flags >> 8 & 1)


def toggle_ally(run, name, record):
    """the in-game ALLIES dialog's toggle of `name`'s row toward `record`, then OK"""
    run.tacli("keys", name, "tab")
    run.tacli("ui", name, "wait", "--gui", "TABMENU", "--timeout", "20")
    run.tacli("ui", name, "click", "ALLIES")
    run.tacli("ui", name, "wait", "--gui", "ALLIES", "--timeout", "20")
    run.tacli("ui", name, "click", f"LIVEALLY{record}")
    run.tacli("ui", name, "click", "OK")
    time.sleep(10)                         # the 0x23 to H, then H's next players phase


def commanders(units):
    return {u["owner"]: u for u in units if u["type"].upper() in COMMANDERS}


def scenario(path, units):
    path.write_text(json.dumps(dict(format="ta-scenario/1", seed=1, on_error="abort",
                                    setup=dict(clear_existing=False), units=units)))
    return path


def play(run, variant, out, dll, port, display):
    token = "mpa" + os.urandom(3).hex()
    names = [token + "H", token + "J"]
    try:
        for name in names:
            run.tacli("create", name, "--private-registry", "--display", display.display,
                      "--res", "1024x768")
            game = tareplay.INSTANCES / name / "gamedir"
            (game / "ddraw.dll").write_bytes(dll.read_bytes())
            run.tacli("registry", name, "MultiCommanderDeath=0", "gamespeed=20")
            run.tacli("launch", name, "--keep-dll", "--dplay", "--dplay-port", port)
            run.tacli("ui", name, "wait", "--gui", "MAINMENU", "--timeout", "60")
        H, J = names
        run("bash", TREE / "tools" / "mp_lobby.sh", "--map", "Two Continents",
            *(["--allies"] if variant == "allied" else []), H, J,
            env=dict(os.environ, MP_NO_START="1"), timeout=400)
        for _ in range(2):                 # the host's row 2: closed, open, AI
            run.tacli("ui", H, "click", "PLAYER2")
        run.tacli("ui", J, "click", "PLAYER3")
        if variant == "allied":            # AI2 and J on team 0; H stays on none
            run.tacli("ui", H, "click", "TEAMICONS2")
            run.tacli("ui", J, "click", "TEAMICONS0")
        for name in names:
            run.tacli("ui", name, "click", "READY0")
        run.tacli("ui", H, "click", "START", "--timeout", "30")
        for name in names:
            run.tacli("wait", name, "alive=[1-9]", "--timeout", "180")

        on_h, on_j = commanders(run.roster(H)), commanders(run.roster(J))
        j_own = on_j[0]["world"][:2]
        j_on_h = next(o for o, u in on_h.items() if u["world"][:2] == j_own)
        h_on_j = next(o for o, u in on_j.items() if u["world"][:2] == on_h[0]["world"][:2])
        jx, jz = j_own
        ax, az = on_h[2]["world"][:2]
        tx, tz = TOWER_AT
        run.tacli("scenario", "apply", H, scenario(out / "tower.json", [
            dict(id="tower", type="ARMRAD", owner=2, pos=[tx, tz])]), "--json")
        run.tacli("scenario", "apply", J, scenario(out / "enemy.json", [
            dict(id="e", type="ARMSOLAR", owner=3, pos=[tx + E_OFFSET, tz])]), "--json")
        time.sleep(25)                     # a radar pass is 30 ticks; the stamps are at once
        e = next(u for u in run.roster(J) if u["type"].upper() == "ARMSOLAR" and
                 abs(u["world"][0] - (tx + E_OFFSET)) < 64 and abs(u["world"][1] - tz) < 64)

        shared = variant == "allied"
        checks = [
            ("H's counters at J's commander", run.counter(H, 0, jx, jz), shared),
            ("J's counters at AI2's commander", run.counter(J, 0, ax, az), shared),
            ("AI2's counters on H at J's commander", run.counter(H, 2, jx, jz), shared),
            ("H's counters at AI2's commander", run.counter(H, 0, ax, az), False),
            ("J's view: E a contact", run.contact(J, e["engine_index"]), shared),
        ]
        if shared:
            toggle_ally(run, J, h_on_j)
            checks += [("after J breaks: H's counters at J's commander", run.counter(H, 0, jx, jz),
                        False),
                       ("after J breaks: J's counters at AI2's commander",
                        run.counter(J, 0, ax, az), True)]
            toggle_ally(run, J, h_on_j)
            checks.append(("after J allies again: H's counters at J's commander",
                           run.counter(H, 0, jx, jz), True))
        rows = {n: [run.peek(n, f"*0x511DE8+{REC0 + i * REC_STRIDE + REC_ROW + k:X}:1")[0]
                    for i in range(4) for k in range(4)] for n in names}
        return {"variant": variant, "J on H": j_on_h, "rows": rows,
                "checks": [{"what": w, "read": v, "want": "non-zero" if want else "zero",
                            "ok": v is not None and bool(v) == want} for w, v, want in checks]}
    finally:
        for name in names:
            try:
                (out / f"{name}.log").write_text(
                    run.tacli("log", name, "--tail", "100000", check=False))
            except (OSError, subprocess.SubprocessError):
                pass
            run.tacli("stop", name, check=False, timeout=60)
            run.tacli("rm", name, check=False, timeout=60)


def main():
    import devpy
    devpy.ensure()
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("variant", choices=("allied", "control"))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dll", type=Path, default=TREE / "tagpu" / "ddraw" / "ddraw.dll")
    ap.add_argument("--port", type=int, default=47853)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tareplay.runlock.hold(TREE, "mp_allysight " + args.variant)
    run = Run(out)
    display = tareplay.start_xvfb()
    try:
        result = play(run, args.variant, out, args.dll.resolve(), args.port, display)
    finally:
        tareplay.stop_xvfb(display)
    (out / "result.json").write_text(json.dumps(result, indent=1))
    for c in result["checks"]:
        print(f"{'pass' if c['ok'] else 'FAIL'}  {c['what']}: {c['read']} (want {c['want']})")
    return 0 if all(c["ok"] for c in result["checks"]) else 1


if __name__ == "__main__":
    sys.exit(main())
