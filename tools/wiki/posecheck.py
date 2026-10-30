#!/usr/bin/env python3
"""posecheck.py -- the mockup's script pose (tools/wiki/mockup/pose.js) against tacob's composition.

    .venv-undither/bin/python tools/wiki/posecheck.py [--pack DIR] [--poses N] [--seed S] [TYPE ...]

For every unit type of a gfx pack (tools/wiki/gfx.py; default tools/wiki/mockup/gfx/, every entry's types, or the
TYPEs named), N random poses -- a MOVE of up to 8 units and any TURN on every piece the script names, a random facing
-- are composed twice: by pose.js `model` under node, over the pack's own piece table and vertices, exactly as gfx.js
draws a posed unit; and by tools/tacob `_compose`, over the model read again from the install with tacob's own
PieceFrame, the composition `tacob pose-check` holds to the engine's posed vertex buffer. Every body vertex must agree
within TOL model units: a wrong axis, sign, rotation order or piece assignment is off by whole units. Then, on the
pack's arrays alone:
  - the pose the host starts a unit in (no MOVE, no TURN, Create()'s flags) composes to the rest pose emitUnits draws
    (the held yaw on the file's axes), within the 16.16 grid pose.js rounds onto;
  - a piece whose visible flag is clear leaves the body and the wire, and one not also cached leaves the slant.

Exit 0 when every check holds, 1 on a failure, 2 when it cannot run (no node, no pack, no install)."""
import argparse
import json
import math
import random
import struct
import subprocess
import sys
from importlib.machinery import SourceFileLoader
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
TOL = 1.0 / 1024           # model units: tacob rounds to 16.16 at every rotation and level, pose.js once at the end
MOVE = 8 * 65536           # the largest random MOVE, 16.16
FIXED = 65536

NODE = r"""
const fs = require('fs'), path = require('path');
const P = require(process.argv[1]);
const job = JSON.parse(fs.readFileSync(0, 'utf8'));
const f32 = b => new Float32Array(b.buffer, b.byteOffset, b.byteLength >> 2);
const rd = f => fs.readFileSync(path.join(job.dir, f));
const m = job.rec, ids = rd(m.pieceFile), u16 = new Uint16Array(ids.buffer, ids.byteOffset, ids.byteLength >> 1);
const nb = m.verts, ns = m.slantVerts;
const full = { mesh: f32(rd(m.files['0'])), quad: f32(rd(m.quadFiles['0'])), normals: f32(rd(m.normalsFile)),
               slant: f32(rd(m.slantFile)), wire: f32(rd(m.wireFile)),
               bp: u16.subarray(0, nb), sp: u16.subarray(nb, nb + ns), wp: u16.subarray(nb + ns) };
const out = job.poses.map(q => {
  const p = { n: q.flags.length, pos: Int32Array.from(q.pos), turn: Uint16Array.from(q.turn), flags: Uint16Array.from(q.flags) };
  const r = P.model(full, m.pieces, p, q.cs, q.sn);
  const pick = (a, st) => Array.from({ length: a.length / st }, (_, i) => [a[st * i], a[st * i + 1], a[st * i + 2]]);
  return { mesh: pick(r.mesh, 7), slant: pick(r.slant, 3), wire: pick(r.wire, 3) };
});
const rest = P.rest(full, m.pieces);
process.stdout.write(JSON.stringify({ out, rest: Array.from(rest.mesh), restSlant: rest.slant.length / 3,
  restWire: rest.wire.length / 3, bp: Array.from(full.bp), sp: Array.from(full.sp), wp: Array.from(full.wp),
  mesh: Array.from(full.mesh) }));
"""


class CannotRun(Exception):
    pass


def node_model(pack_dir, rec, poses):
    try:
        got = subprocess.run(["node", "-e", NODE, str(HERE / "mockup/pose.js")], input=json.dumps(
            {"dir": str(pack_dir), "rec": rec, "poses": poses}), capture_output=True, text=True)
    except OSError as e:
        raise CannotRun(f"node: {e}")
    if got.returncode:
        raise CannotRun(f"pose.js under node: {got.stderr.strip()}")
    return json.loads(got.stdout)


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def check_type(eng, tacob, ta3do, gfx, pack_dir, name, rec, npose, rng):
    """[(what, ok, detail)] for one type."""
    script = rec.get("script")
    if not script:
        return [("composition", True, "no script: drawn at rest only, nothing to pose")]
    table, order = rec["pieces"], script["order"]
    _label, root, _hidden, _uncached, _facts = gfx.resolve_unit(eng, name, [])
    cob = eng.open(eng.dirs["scripts"], name, "COB")[0]
    cob_names = (ta3do._cob_read(cob) or (None, None, []))[2]
    frame = tacob.PieceFrame.from_3do(cob_names, root)
    nodes = [p for p, _ in root.walk()]
    if len(nodes) != len(table) or sorted(order) != list(range(len(nodes))):
        return [("composition", False, f"the pack's table has {len(table)} pieces, the model {len(nodes)}")]
    # the posed index of a node the script names is the script's piece index (0x45A950's swap): the host's MOVE and
    # TURN of piece c land on primitive c
    named = {}
    for c, node in enumerate(frame.of_cob):
        if node >= 0:
            if c >= len(order) or order[c] != node:
                return [("composition", False, f"piece {cob_names[c]}: posed index {order.index(node)}, script {c}")]
            named[c] = node
    cum = {}

    def offset(node):
        if node not in cum:
            par = frame.parent[node]
            base = offset(par) if par >= 0 else [0, 0, 0]
            cum[node] = [base[k] + frame.offset[node][k] for k in range(3)]
        return cum[node]

    poses, refs = [], []
    for _ in range(npose):
        pos = [[0, 0, 0] for _ in range(max(len(table), len(cob_names)))]
        ang = [[0, 0, 0] for _ in range(max(len(table), len(cob_names)))]
        for c in named:
            pos[c] = [rng.randint(-MOVE, MOVE) for _ in range(3)]
            ang[c] = [rng.randrange(65536) for _ in range(3)]
        yaw = rng.randrange(65536)
        th = s16(yaw) * 2 * math.pi / 65536
        # tacob turns the root's (x, z) by +yaw (rotate2: x' = x cos - z sin); the stage's facing is
        # [[cs, 0, sn], [0, 1, 0], [-sn, 0, cs]], so the same turn is cs = cos, sn = -sin
        poses.append({"pos": [v for p in pos[:len(table)] for v in p], "turn": [v for a in ang[:len(table)] for v in a],
                      "flags": [7] * len(table), "cs": math.cos(th), "sn": -math.sin(th)})
        refs.append((pos, ang, (0, yaw, 0)))
    # the start pose: no MOVE, no TURN, the flags the host gives (visible from the vertex count) less Create()'s hides
    start = [(1 if script["visible"][p] and not table[p]["hidden"] else 0) | (0 if table[p]["uncached"] else 2) | 4
             for p in range(len(table))]
    poses.append({"pos": [0] * 3 * len(table), "turn": [0] * 3 * len(table), "flags": start, "cs": -1.0, "sn": 0.0})
    # one piece hidden and one dont-cache, of the ones with faces
    faced = sorted({p for p in range(len(table)) if script["visible"][p]})
    hide, uncache = (faced[0], faced[-1]) if faced else (-1, -1)
    poses.append({"pos": [0] * 3 * len(table), "turn": [0] * 3 * len(table), "cs": 1.0, "sn": 0.0,
                  "flags": [(5 if p == uncache else 7) & (~1 if p == hide else 7) for p in range(len(table))]})
    js = node_model(pack_dir, rec, poses)
    bp, mesh = js["bp"], js["mesh"]
    out = []

    worst = 0.0
    for k in range(npose):
        pos, ang, body = refs[k]
        got = js["out"][k]["mesh"]
        if len(got) != len(bp):
            out.append(("composition", False, f"pose {k}: {len(got)} vertices posed of {len(bp)}"))
            break
        for v, p in enumerate(bp):
            node = order[p]
            held = [-mesh[7 * v], mesh[7 * v + 1], -mesh[7 * v + 2]]
            local = [round(held[a] * FIXED) - offset(node)[a] for a in range(3)]
            ref = tacob._compose(frame, pos, ang, node, local, body)
            worst = max(worst, max(abs(got[v][a] - ref[a] / FIXED) for a in range(3)))
    else:
        out.append(("composition", worst <= TOL, f"{npose} poses x {len(bp)} vertices, worst {worst:.6f} units"))

    # the start pose against emitUnits' rest: P = (x cs + z sn, y, -x sn + z cs) of the file's axes with the held yaw
    # (-cs, -sn) = (1, 0) for the record's (-1, 0), the default facing
    got, rest = js["out"][npose]["mesh"], js["rest"]
    worst = 0.0 if len(got) * 7 == len(rest) else math.inf
    if worst == 0.0:
        for v in range(len(got)):
            worst = max(worst, *(abs(got[v][a] - rest[7 * v + a]) for a in range(3)))
    out.append(("start pose = rest", worst <= 1.0 / FIXED,
                f"{len(got)} vertices, worst {worst:.7f} units" if worst != math.inf
                else f"{len(got)} vertices posed, {len(rest) // 7} at rest"))
    # the flags: the hidden piece's vertices gone from the body and the wire, the dont-cache one's from the slant
    if hide >= 0:
        o = js["out"][npose + 1]
        want_b = sum(1 for p in bp if p != hide)
        want_s = sum(1 for p in js["sp"] if p not in (hide, uncache))
        want_w = sum(1 for p in js["wp"] if p != hide)
        ok = (len(o["mesh"]), len(o["slant"]), len(o["wire"])) == (want_b, want_s, want_w)
        out.append(("flags", ok, f"body {len(o['mesh'])}/{want_b}, slant {len(o['slant'])}/{want_s}, "
                                 f"wire {len(o['wire'])}/{want_w}"))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", default=str(HERE / "mockup/gfx"), help="a gfx pack (default: tools/wiki/mockup/gfx/)")
    ap.add_argument("--poses", type=int, default=6, help="random poses a type (default 6)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--gamedir", help="the install (default as gfx.py finds it)")
    ap.add_argument("types", nargs="*")
    a = ap.parse_args(argv)
    pack = Path(a.pack)
    try:
        if not (pack / "index.json").is_file():
            raise CannotRun(f"no pack at {pack}: run tools/wiki/gfx.py")
        import gfx
        import tavfs
        tacob = SourceFileLoader("tacob", str(ROOT / "tools/tacob")).load_module()
        try:
            eng = gfx.Engine(tavfs.find_install(a.gamedir))
        except (tavfs.VfsError, OSError) as e:
            raise CannotRun(f"the install: {e}")
        index = json.loads((pack / "index.json").read_text())
        seen, fails = set(), 0
        rng = random.Random(a.seed)
        for entry in sorted(index["entries"]):
            d = pack / entry
            meshes = json.loads((d / "scene.json").read_text())["units"]["meshes"]
            for name in sorted(meshes):
                if name in seen or (a.types and name not in a.types):
                    continue
                seen.add(name)
                if "pieceFile" not in meshes[name]:
                    raise CannotRun(f"{d}: {name} has no piece table: the pack predates it, run tools/wiki/gfx.py")
                for what, ok, detail in check_type(eng, tacob, gfx.ta3do, gfx, d, name, meshes[name], a.poses, rng):
                    fails += not ok
                    print(f"posecheck: {name:<10} {what:<18} {'ok ' if ok else 'BAD'} {detail}")
        if not seen:
            raise CannotRun("no unit type to check")
    except CannotRun as e:
        print(f"posecheck: cannot run: {e}", file=sys.stderr)
        return 2
    print(f"posecheck: {len(seen)} types, {'all agree' if not fails else f'{fails} FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
