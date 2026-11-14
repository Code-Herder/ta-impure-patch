#!/usr/bin/env python3
"""nodraweq.py -- whether NO DRAW changes the simulation: takeover twins, drawn and not, against a null twin.

    tools/replay/nodraweq.py --rec FILE --at T0 --out DIR [--player P] [--dll PATH]

WHAT IT TESTS. NO DRAW (tagpu_nodraw.c) runs an undrawn frame in DrawGameScreen's place on a test's
GDI frames between two turbo batches, on the claim that it runs every piece of the draw that reaches simulation
state -- DrawUnit's repose of the pose cache for the units the draw would reach -- and that the rest of the draw
and its observers write nothing the simulation reads. Four lanes take
the recording FILE over at T0 as player P, each a fresh process on GDI at turbo k = 1: A and B drawn, C and D under
no draw (tareplay.NODRAW_LEVER). A and B are the null twin: two drawn runs of the same takeover, whose differences
are what separate processes make with no change at all (heap addresses, the CRT rand stream's draws: engine/sim.md,
*The batch is part of the result*). Each lane, on the held frame at T0 - 1:

  - peeks the meteor switch 0x51232C: a shower on makes the meteor points differ between any two processes, so
    the run is INCONCLUSIVE;
  - arms `dump d<T> at <T>` for T in T0 + 300, T0 + 3000, T0 + 9000 and T0 + 18000;
  - turns the turbo on (and, C and D, requires `nodraw: on at 0x4969CD`), arms `pause at T0 + 18000` and resumes;
  - once paused, ends the level (the end scenario), so its account line is written; a level that ends first
    skips its later dumps and says so.
Every lane also launches with the pose check (POSE_LEVER, tagpu_nodraw.c, THE POSE CHECK): every POSE_EVERY GameTime,
a digest of every model's pose cache and of the SweetSpot point 0x43E0B0 each piece gives, which is what the next
step's AutoAim reads.

THE VERDICT, at every T all four dumps hold, each pair compared as `tarec compare-dumps FILE X Y --takeover` does
(fork_view's policy), every difference kept rather than the first twenty, as a set of fields (the line less its
two values): PASS when (A,B)'s fields are at most the meteor points (globals meteorp1, meteorp2), each mixed pair's
-- (A,C), (A,D), (B,C) -- and (C,D)'s fields are a subset of (A,B)'s, and C's and D's undrawn share is at least
0.95 (tareplay.nodraw_share). (A,B) differing beyond the meteor points is INCONCLUSIVE: this map's twins are not
equal even drawn. THE POSES, at every GameTime all four lanes logged a pose check: where A's and B's digests agree,
C's and D's must equal them, else FAIL (an undrawn frame reposed other units, or none); where A and B differ the
GameTime is not judged, and a run where they never agree is INCONCLUSIVE. Exit 0 PASS, 1 FAIL, 2 could not run or INCONCLUSIVE. DIR gets each lane's folder (its dumps and
log) and nodraweq.json.

WHERE IT STOPS. Two drawn takeovers of one recording are not equal for long: MEASURED 2026-11-13 on two of the
bar's Pincushion recordings at T0 near 9000, k = 1, A and B already differed within 300 ticks of T0 (projectile
0, unit +0x140/+0x144 and the meteor points, the shower off) and in nearly every field by T0 + 3000. Past that
point the verdict is INCONCLUSIVE, and a mixed pair differing in a record (A,B) already differs in is not
evidence that no-draw changes anything. Its exactness is proved in one process instead, by THE SHADOW
(tagpu_nodraw.c; the replay test group nodraw-shadow); what is left here is the pose check at the GameTimes
the drawn twins still agree on.

The run lock is held for four GDI lanes and no GPU memory (tools/runlock.py)."""
import argparse
import json
import re
import shutil
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]
sys.path.insert(0, str(TREE / "tools"))
sys.path.insert(0, str(HERE))
import tareplay as R  # noqa: E402
import tarec  # noqa: E402

LANES = (("A", False), ("B", False), ("C", True), ("D", True))
OFFSETS = (300, 3000, 9000, 18000)
PAIRS = (("A", "B"), ("C", "D"), ("A", "C"), ("A", "D"), ("B", "C"))
NULL = ("A", "B")
METEOR_ON = "0x51232C:4"
METEOR_POINTS = {"globals meteorp1", "globals meteorp2"}
SHARE_MIN = 0.95
VALUES = re.compile(r": recording [0-9a-f]+, dump [0-9a-f]+$")
POSE_LEVER = "tagpu_posecheck.on"
POSE_EVERY = 300
POSE_ACK = r"posecheck: (every \d+ GameTime|tagpu_posecheck\.on refused.*)"
POSE_LINE = re.compile(r"posecheck: GameTime (\d+) units (\d+) skipped (\d+) pose ([0-9a-f]{16}) "
                       r"aim ([0-9a-f]{16})")


def field(line: str) -> str:
    """A difference line of tarec's World.differences less its two values: the field it names."""
    return VALUES.sub("", line)


def fields(rec: Path, a: Path, b: Path, T0: int) -> set:
    """Every field that differs between two dumps of takeovers of `rec` at T0, at one GameTime, by
    tarec.cmd_compare_dumps's `--takeover` comparison with no limit on the lines. Its rules are the
    header of the segment the takeovers start from, the one holding T0: the dumps' GameTime may lie
    past the recording's end, where only each twin's own timeline goes on."""
    h = tarec.header_only(rec)
    ah, wa = tarec.dump_world(a, h)
    bh, wb = tarec.dump_world(b, h)
    if wa.tick != wb.tick:
        raise R.CouldNotRun(f"{a.name} is at GameTime {wa.tick}, {b.name} at {wb.tick}")
    hs = tarec.segment_at(rec, 0, T0)["header"]
    tarec.fork_view(wa, wb, hs, hs)
    lines, n = wa.differences(wb, ah["tables"], bh["tables"], limit=1 << 30,
                              gnames=tarec.known_names(h), sight=True)
    if len(lines) != n:
        raise R.CouldNotRun(f"{a.name} against {b.name}: {n} differences, {len(lines)} lines")
    return {field(ln) for ln in lines}


def poses(lines) -> dict:
    """The pose checks a lane's log holds: {GameTime: [units, skipped, pose, aim]}, the last line of a GameTime
    winning (a level's GameTime runs once)."""
    got = {}
    for ln in lines:
        m = POSE_LINE.search(ln)
        if m:
            got[m.group(1)] = [int(m.group(2)), int(m.group(3)), m.group(4), m.group(5)]
    return got


def play_lane(name: str, nodraw: bool, rec: Path, T0: int, P: int, out: Path, dll: Path) -> dict:
    """One lane's takeover, dumps and account (the module's docstring); its folder is out/<name>."""
    lane_out = out / name
    lane_out.mkdir(parents=True, exist_ok=True)
    copy = lane_out / rec.name
    shutil.copy2(rec, copy)
    end = T0 + OFFSETS[-1]
    res = {"lane": name, "nodraw": nodraw, "dumps": {}, "skipped": []}
    w = R.Worker(lane_out)
    try:
        w.gdi, w.held, w.turbo, w.nodraw = True, True, 1, nodraw
        w.place({"name": "nodraweq"}, dll)
        (w.gamedir / POSE_LEVER).write_text(str(POSE_EVERY))
        log = R.Log(w)
        rc, o = R.tacli("replay", "takeover", w.name, copy, "--at", T0, "--player", P,
                        "--defaults", "--restart", "--keep-dll", "--timeout", R.ACK_S, "--json",
                        timeout=R.LAUNCH_TIMEOUT + R.ACK_S)
        try:
            got = json.loads(o[o.index("{"):])
        except ValueError:
            raise R.CouldNotRun(f"{name}: replay takeover: {R.last(o)}")
        if rc or not got.get("taken") or not got.get("held"):
            raise R.CouldNotRun(f"{name}: replay takeover: "
                                f"{got.get('line') or got.get('not_recorded') or R.last(o)}")
        held = R.held_start(w, log)
        ack = log.wait(POSE_ACK, R.ACK_S, 0)
        if not ack or "every" not in ack:
            raise R.CouldNotRun(f"{name}: the launch did not acknowledge {POSE_LEVER}: {ack or 'no answer'}")
        if held != T0 - 1:
            raise R.CouldNotRun(f"{name}: the takeover's level is held at GameTime {held}, not {T0 - 1}")
        res["meteor"] = R.peek(w, METEOR_ON)
        for k in OFFSETS:
            R.arm_dump(w, log, f"d{T0 + k}", T0 + k)
        R.hold_at(w, log, end)
        R.turbo_engage(w, log)
        R.resume(w, log)
        rx = rf"pause: (paused at GameTime {end}\s*$|at {end} not taken.*)|{R.LEVEL_ENDED.pattern}"
        got = log.wait_running(rx, log.hold_from)
        if "paused at" in got:
            log.pending, log.held, log.at = None, True, end
            n = len(log.lines)
            rc, o = R.tacli("scenario", "apply", w.name, R.END_SCENARIO, timeout=300)
            if rc:
                raise R.CouldNotRun(f"{name}: apply {R.END_SCENARIO.name}: {R.last(o)}")
            R.resume(w, log)
            log.wait_end(R.LEVEL_ENDED.pattern, n)
        else:
            res["ended_early"] = got.strip()
        if nodraw and not log.wait(R.NODRAW_ACCOUNT.pattern, R.ACK_S, 0):
            raise R.CouldNotRun(f"{name}: the level ended with no `nodraw: from GameTime` account")
        log.read()
        for k in OFFSETS:
            dname = f"d{T0 + k}"
            if any(re.search(R._CLI_API.dump_written_rx(dname), ln) for ln in log.lines):
                res["dumps"][str(T0 + k)] = str(lane_out / f"dump-{dname}.bin")
            else:
                res["skipped"].append(T0 + k)
        res["account"] = log.nodraw
        res["share"] = R.nodraw_share(log.nodraw)
        res["pose"] = poses(log.lines)
        w.collect(lane_out)
        w.stop(hard=True)
    finally:
        (w.gamedir / POSE_LEVER).unlink(missing_ok=True)
        w.close()
    if res["skipped"]:
        R.say(f"{name}: the level ended before GameTime {res['skipped'][0]}: dumps "
              f"{', '.join(map(str, res['skipped']))} skipped")
    return res


def judge(rec: Path, lanes: dict, T0: int) -> dict:
    """The verdict over the lanes' dumps (the module's docstring)."""
    report = {"at": {}, "verdict": "PASS", "why": []}
    if any(lanes[n].get("meteor") for n in lanes):
        report["verdict"] = "INCONCLUSIVE"
        report["why"].append("the meteor shower is switched on (0x51232C): the meteor points differ by process")
    for n, nodraw in LANES:
        share = lanes[n].get("share")
        if nodraw and (share is None or share < SHARE_MIN):
            report["verdict"] = "FAIL" if report["verdict"] == "PASS" else report["verdict"]
            report["why"].append(f"{n}: undrawn share {share} below {SHARE_MIN}")
    judged = 0
    for k in OFFSETS:
        T = str(T0 + k)
        if not all(T in lanes[n]["dumps"] for n, _ in LANES):
            continue
        judged += 1
        got = {f"{x},{y}": sorted(fields(rec, Path(lanes[x]["dumps"][T]), Path(lanes[y]["dumps"][T]), T0))
               for x, y in PAIRS}
        report["at"][T] = got
        null = set(got[",".join(NULL)])
        if not null <= METEOR_POINTS:
            if report["verdict"] != "FAIL":
                report["verdict"] = "INCONCLUSIVE"
            report["why"].append(f"T {T}: the null twin (A,B) differs beyond the meteor points: "
                                 f"{sorted(null - METEOR_POINTS)[:8]}")
        for x, y in PAIRS[1:]:
            extra = set(got[f"{x},{y}"]) - null
            if extra:
                report["verdict"] = "FAIL"
                report["why"].append(f"T {T}: ({x},{y}) differs beyond (A,B) in {len(extra)} fields: "
                                     f"{sorted(extra)[:8]}")
    if not judged:
        report["verdict"] = "INCONCLUSIVE" if report["verdict"] == "PASS" else report["verdict"]
        report["why"].append("no GameTime has all four lanes' dumps")
    judge_poses(lanes, report)
    return report


def judge_poses(lanes: dict, report: dict):
    """THE POSES (the module's docstring), into `report`: its "poses" {judged, null_differs} and its verdict."""
    common = set.intersection(*(set(lanes[n].get("pose") or {}) for n, _ in LANES))
    judged, null_differs = 0, []
    for G in sorted(common, key=int):
        a = lanes["A"]["pose"][G]
        if lanes["B"]["pose"][G] != a:
            null_differs.append(G)
            continue
        judged += 1
        for n in ("C", "D"):
            if lanes[n]["pose"][G] != a:
                report["verdict"] = "FAIL"
                report["why"].append(f"GameTime {G}: {n}'s pose check {lanes[n]['pose'][G]} is not the drawn "
                                     f"twins' {a}")
    report["poses"] = {"judged": judged, "null_differs": null_differs}
    if null_differs:
        report["why"].append(f"the drawn twins' pose checks differ at {len(null_differs)} GameTime(s), "
                             f"first {null_differs[0]}: not judged there")
    if not judged:
        report["verdict"] = "INCONCLUSIVE" if report["verdict"] == "PASS" else report["verdict"]
        report["why"].append("no GameTime has four pose checks on which the drawn twins agree")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rec", type=Path, required=True, help="the recording to take over")
    ap.add_argument("--at", type=int, required=True, metavar="T0",
                    help="a GameTime the takeover accepts in it (tarec info / segments)")
    ap.add_argument("--player", type=int, default=0)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--dll", type=Path, default=None, help="default: this tree's build, current")
    a = ap.parse_args(argv)
    import runlock
    try:
        dll = a.dll.resolve() if a.dll else R.chosen_dll("nodraweq", argparse.Namespace(dll=None), False, False)
        rec = a.rec.resolve()
        if not rec.is_file():
            raise R.CouldNotRun(f"no recording at {rec}")
        a.out.mkdir(parents=True, exist_ok=True)
    except R.CouldNotRun as e:
        print(f"nodraweq: COULD NOT RUN: {e}")
        return 2
    runlock.hold(TREE, "nodraweq " + " ".join(sys.argv[1:] if argv is None else argv), lanes=len(LANES),
                 gpu=False)
    lanes, errors, lock = {}, [], threading.Lock()

    def run(name, nodraw):
        try:
            r = play_lane(name, nodraw, rec, a.at, a.player, a.out, dll)
            with lock:
                lanes[name] = r
        except (R.CouldNotRun, R.Failed) as e:
            with lock:
                errors.append(f"{name}: {e}")
    t0 = time.monotonic()
    try:
        threads = [threading.Thread(target=run, args=lane, name=f"nodraweq-{lane[0]}") for lane in LANES]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        runlock.release()
    if errors:
        for e in errors:
            print(f"nodraweq: COULD NOT RUN: {e}")
        return 2
    try:
        report = judge(rec, lanes, a.at)
    except (R.CouldNotRun, tarec.Unreadable) as e:
        print(f"nodraweq: COULD NOT RUN: {e}")
        return 2
    report.update({"rec": str(rec), "T0": a.at, "player": a.player, "lanes": lanes,
                   "wall_s": round(time.monotonic() - t0, 1)})
    (a.out / "nodraweq.json").write_text(json.dumps(report, indent=1) + "\n")
    for n, _ in LANES:
        acc = lanes[n].get("account")
        print(f"nodraweq: {n}: " + (f"{acc['skipped']} frames not drawn, {acc['board']} for the scoreboard, "
                                    f"{acc['drawn']} drawn otherwise (share {lanes[n]['share']:.3f})"
                                    if acc else "drawn"))
    for T, got in report["at"].items():
        print(f"nodraweq: T {T}: " + ", ".join(f"({p}) {len(v)}" for p, v in got.items()) + " fields differ")
    print(f"nodraweq: poses: {report['poses']['judged']} GameTime(s) judged, "
          f"{len(report['poses']['null_differs'])} where the drawn twins differ")
    for why in report["why"]:
        print(f"nodraweq: {why}")
    print(f"nodraweq: {report['verdict']} ({a.out / 'nodraweq.json'})")
    return {"PASS": 0, "FAIL": 1}.get(report["verdict"], 2)


if __name__ == "__main__":
    sys.exit(main())
