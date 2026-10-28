/* pose.js -- a unit's script pose on the wiki's stage: the posed model the COB host holds (cob.js `model`) composed over
   the type's piece table (tools/wiki/gfx.py writes it), and the pack's vertices carried through it. The arithmetic is
   tagpu/ddraw/src/tagpu_posemath.h's and tagpu_wikiworld.c `pose_compose`'s, in float32 step for step, so the mockup
   draws the pose the game's stage draws; tools/wiki/posecheck.py holds it equal to tools/tacob's composition, which
   `tacob pose-check` holds to the engine's posed vertex buffer.

   THE AXES. The pack is in the 3DO file's axes; the engine holds a model with X and Z negated (the loader, 0x4CB590),
   and MOVE and TURN act in those held axes: turn[0] about X, turn[1] about Y, turn[2] about Z, 65536 a whole turn,
   rotated Z first, then X, then Y (tagpu_native.c, "COB pose"). So every point and normal is taken into the held axes
   (x, y, z) -> (-x, y, -z) before the pose, and what comes out is the held model turned onto the facing: the space
   tascene-render.js `emitUnits` projects once its own yaw is the identity.

   Runs in a page (the global WikiPose) and under node (module.exports). */
const WikiPose = (() => {
  const F = Math.fround;
  const K = F(6.2831853 / 65536);                 // tagpu_posemath.h piece_local's K, a float
  const PRIM_BASE = 0x22, PRIM_BYTES = 0x36;      // tagpu_cobvm.h: the first PrimitiveStruct, one's size
  const PRIM_POS = 0x04, PRIM_TURN = 0x10, PRIM_FLAGS = 0x28;
  const VISIBLE = 1, CACHED = 2, SHADED = 4;      // TAGPU_COBVM_VISIBLE, _CACHED, _SHADED

  /* A posed model block (TAGPU_COBVM_MODEL_BYTES(n) bytes, the host's tagpu_wikicob_model) as the three fields the
     stage copies of each primitive (tagpu_wikistage.c `tagpu_wikistage_pose`): {n, pos (int32 16.16 x3), turn (uint16
     x3), flags}. null for a block whose length is not a whole number of primitives. */
  function parse(bytes) {
    const n = (bytes.length - PRIM_BASE) / PRIM_BYTES;
    if (!Number.isInteger(n) || n < 0) return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const pos = new Int32Array(3 * n), turn = new Uint16Array(3 * n), flags = new Uint16Array(n);
    for (let k = 0; k < n; k++) {
      const b = PRIM_BASE + k * PRIM_BYTES;
      for (let a = 0; a < 3; a++) {
        pos[3 * k + a] = dv.getInt32(b + PRIM_POS + 4 * a, true);
        turn[3 * k + a] = dv.getUint16(b + PRIM_TURN + 2 * a, true);
      }
      flags[k] = dv.getUint16(b + PRIM_FLAGS, true);
    }
    return { n, pos, turn, flags };
  }

  // tagpu_posemath.h rot2: a' = a*c - b*s, b' = a*s + b*c, each product and sum a float
  const rot2 = (c, s, a, b) => [F(F(a * c) - F(b * s)), F(F(a * s) + F(b * c))];

  // tagpu_posemath.h piece_local: T(d) * R(turn) as a 4x3 row-major, R from the basis vectors through rot2
  function local(turn, t, d, o) {
    const cs = a => { const r = F(turn[t + a] * K); return [F(Math.cos(r)), F(Math.sin(r))]; };
    const [c0, s0] = cs(0), [c1, s1] = cs(1), [c2, s2] = cs(2);
    for (let j = 0; j < 3; j++) {
      let x = j === 0 ? 1 : 0, y = j === 1 ? 1 : 0, z = j === 2 ? 1 : 0;
      if (turn[t + 2]) [x, y] = rot2(c2, s2, x, y);       // about Z
      if (turn[t]) [y, z] = rot2(c0, s0, y, z);           // about X
      if (turn[t + 1]) [x, z] = rot2(c1, s1, x, z);       // about Y
      o[j] = x; o[4 + j] = y; o[8 + j] = z;
    }
    o[3] = d[0]; o[7] = d[1]; o[11] = d[2];
  }

  // tagpu_posemath.h m43_mul: o = a * b, the sums left to right as the C writes them
  function mul(a, b, o, at) {
    for (let r = 0; r < 3; r++) {
      for (let c = 0; c < 3; c++)
        o[at + r * 4 + c] = F(F(F(a[r * 4] * b[c]) + F(a[r * 4 + 1] * b[4 + c])) + F(a[r * 4 + 2] * b[8 + c]));
      o[at + r * 4 + 3] = F(F(F(F(a[r * 4] * b[3]) + F(a[r * 4 + 1] * b[7])) + F(a[r * 4 + 2] * b[11])) + a[r * 4 + 3]);
    }
  }

  // a piece's cumulative rest offset in the held axes
  const held = off => [F(-off[0]), F(off[1]), F(-off[2])];

  /* THE PIECES' MATRICES: `table` the type's pieces (gfx.py: {parent, off} a row, in the posed order), `p` a parsed
     posed model of as many primitives, (cs, sn) the record's yaw (scene.js), the facing about the up axis
     [[cs, 0, sn], [0, 1, 0], [-sn, 0, cs]] the game's stage turns a unit by. pose_compose's composition: P = parent *
     T(rest offset from the parent + MOVE) * R(TURN) down the tree; at the root its rest point r0, its MOVE unturned, its
     rotation the facing times R(TURN), so the facing turns the model about r0. The pack's tree has one root, the row
     without a parent (ta3do reads no sibling of the root): the stage's walk node 0. Returns {m: a 4x3 per piece, the
     held axes in and the faced model out, vis: 0 hidden, 1 visible dont-cache, 3 visible and cached, shaded: 1 when the
     piece takes the engine's shade rows}; a hidden piece's matrix is all zeros, as the stage's. */
  function compose(table, p, cs, sn) {
    const n = table.length, m = new Float32Array(12 * n), vis = new Uint8Array(n), shaded = new Uint8Array(n);
    const done = new Uint8Array(n), acc = new Float32Array(12 * n), loc = new Float32Array(12), face = new Float32Array(12);
    const rot = [F(cs), 0, F(sn), 0, 0, 1, 0, 0, F(-sn), 0, F(cs), 0];
    const r0 = held(table[Math.max(0, table.findIndex(r => r.parent < 0))].off);
    let anyShade = false;
    for (let i = 0; i < n; i++) if ((p.flags[i] & VISIBLE) && (p.flags[i] & SHADED)) anyShade = true;
    for (let pass = 0, left = n; pass < n && left > 0; pass++)
      for (let i = 0; i < n; i++) {
        const par = table[i].parent;
        if (done[i] || (par >= 0 && (par >= n || !done[par]))) continue;
        const ro = held(table[i].off), rp = par >= 0 ? held(table[par].off) : null;
        const d = [0, 1, 2].map(k => F((par >= 0 ? F(ro[k] - rp[k]) : 0) + F(p.pos[3 * i + k] / 65536)));
        local(p.turn, 3 * i, d, loc);
        if (par >= 0) mul(acc.subarray(12 * par, 12 * par + 12), loc, acc, 12 * i);
        else {
          const rel = [0, 1, 2].map(k => F(ro[k] - r0[k]));
          for (let k = 0; k < 3; k++) {
            face[k * 4] = rot[k * 4]; face[k * 4 + 1] = rot[k * 4 + 1]; face[k * 4 + 2] = rot[k * 4 + 2];
            face[k * 4 + 3] = F(F(F(F(F(rot[k * 4] * rel[0]) + F(rot[k * 4 + 1] * rel[1])) + F(rot[k * 4 + 2] * rel[2]))
                                  + r0[k]) + d[k]);
          }
          mul(face, loc, acc, 12 * i);
          for (let k = 0; k < 3; k++) acc[12 * i + k * 4 + 3] = face[k * 4 + 3];
        }
        done[i] = 1;
        left--;
      }
    for (let i = 0; i < n; i++) {
      const fl = p.flags[i];
      if (!done[i]) {
        // a piece whose parent never resolved keeps the rest pose turned onto the facing, as the stage's does
        const ro = held(table[i].off), rel = [0, 1, 2].map(k => F(ro[k] - r0[k]));
        for (let k = 0; k < 3; k++) {
          acc[12 * i + k * 4] = rot[k * 4]; acc[12 * i + k * 4 + 1] = rot[k * 4 + 1]; acc[12 * i + k * 4 + 2] = rot[k * 4 + 2];
          acc[12 * i + k * 4 + 3] = F(F(F(F(rot[k * 4] * rel[0]) + F(rot[k * 4 + 1] * rel[1])) + F(rot[k * 4 + 2] * rel[2])) + r0[k]);
        }
      }
      if (fl & VISIBLE) m.set(acc.subarray(12 * i, 12 * i + 12), 12 * i);
      shaded[i] = anyShade ? +((fl & SHADED) !== 0) : 1;
      vis[i] = !(fl & VISIBLE) ? 0 : (fl & CACHED) ? 3 : 1;
    }
    return { m, vis, shaded };
  }

  /* A pack point (file axes, the model's own frame) of piece i through its matrix, onto the engine's 16.16 grid as
     the posed program's vertex stage puts it (tagpu_posedraw.c VS, tagpu_vk_unit.c `wire_records`). */
  function point(c, table, i, x, y, z, o, at) {
    const off = table[i].off, m = c.m, b = 12 * i;
    const lx = F(-F(x - off[0])), ly = F(y - off[1]), lz = F(-F(z - off[2]));
    for (let k = 0; k < 3; k++) {
      const v = F(F(F(F(m[b + k * 4] * lx) + F(m[b + k * 4 + 1] * ly)) + F(m[b + k * 4 + 2] * lz)) + m[b + k * 4 + 3]);
      o[at + k] = Math.floor(F(F(v * 65536) + 0.5)) / 65536;
    }
  }
  // a pack normal (file axes) through piece i's rotation (tagpu_posedraw.c shRot)
  function turn(c, i, x, y, z, o, at) {
    const m = c.m, b = 12 * i, lx = -x, lz = -z;
    for (let k = 0; k < 3; k++) o[at + k] = F(F(F(m[b + k * 4] * lx) + F(m[b + k * 4 + 1] * y)) + F(m[b + k * 4 + 2] * lz));
  }

  /* A dont-shade piece of a lit unit takes the engine's default row 15 (tagpu_posedraw.c VS, THE SHADE). emitUnits takes
     a lit vertex's row from its normal, trunc(dot(N, L) * 5) & 31 with the engine's light L = (-0.8, 1.0, 0.25)
     (0x5065F8): straight up at length 3.1 the dot is 3.1 and the row 15, whatever the facing. */
  const ROW15 = [0, 3.1, 0];

  /* One record's model posed: `full` the type's arrays as the pack holds them, every piece's (mesh: 7 floats a vertex,
     quad: 8, normals: 15, slant: 3, wire: 3, and the pieces of the body's, the slant's and the wire's vertices), `table`
     its pieces, `p` the parsed posed model, (cs, sn) the record's yaw. Returns the arrays emitUnits and gfx.js's wire
     take, with the yaw [1, 0]: a hidden piece's vertices left out of all of them, a slant caster kept only while its
     piece is visible and cached ((flags & 3) == 3, the stage's slant rule). */
  function model(full, table, p, cs, sn) {
    const c = compose(table, p, cs, sn), { bp, sp, wp } = full;
    const nb = bp.reduce((s, i) => s + (c.vis[i] > 0), 0);
    const mesh = new Float32Array(nb * 7), quad = new Float32Array(nb * 8), normals = new Float32Array(nb * 15);
    let o = 0;
    for (let v = 0; v < bp.length; v++) {
      const i = bp[v];
      if (!c.vis[i]) continue;
      point(c, table, i, full.mesh[7 * v], full.mesh[7 * v + 1], full.mesh[7 * v + 2], mesh, 7 * o);
      for (let k = 3; k < 7; k++) mesh[7 * o + k] = full.mesh[7 * v + k];
      quad.set(full.quad.subarray(8 * v, 8 * v + 8), 8 * o);
      for (let q = 0; q < 5; q++) {
        if (c.shaded[i]) turn(c, i, full.normals[15 * v + 3 * q], full.normals[15 * v + 3 * q + 1], full.normals[15 * v + 3 * q + 2], normals, 15 * o + 3 * q);
        else normals.set(ROW15, 15 * o + 3 * q);
      }
      o++;
    }
    const ns = sp.reduce((s, i) => s + (c.vis[i] === 3), 0), slant = new Float32Array(ns * 3);
    o = 0;
    for (let v = 0; v < sp.length; v++)
      if (c.vis[sp[v]] === 3) point(c, table, sp[v], full.slant[3 * v], full.slant[3 * v + 1], full.slant[3 * v + 2], slant, 3 * o++);
    const nw = wp.reduce((s, i) => s + (c.vis[i] > 0), 0), wire = new Float32Array(nw * 3);
    o = 0;
    for (let v = 0; v < wp.length; v++)
      if (c.vis[wp[v]]) point(c, table, wp[v], full.wire[3 * v], full.wire[3 * v + 1], full.wire[3 * v + 2], wire, 3 * o++);
    return { mesh, quad, normals, slant, wire };
  }

  /* The rest pose's arrays, every vertex of a piece Create() hides left out, and a slant caster of a dont-cache one:
     what the pack held before it carried every piece, so a unit no script poses draws exactly as it did. */
  function rest(full, table) {
    const keep = (a, st, pids, ok) => {
      let n = 0;
      for (const i of pids) n += ok(table[i]);
      const out = new Float32Array(n * st);
      let o = 0;
      pids.forEach((i, v) => { if (ok(table[i])) out.set(a.subarray(st * v, st * v + st), st * o++); });
      return out;
    };
    const shown = r => !r.hidden, cast = r => !r.hidden && !r.uncached;
    return { mesh: keep(full.mesh, 7, full.bp, shown), quad: keep(full.quad, 8, full.bp, shown),
             normals: keep(full.normals, 15, full.bp, shown), slant: keep(full.slant, 3, full.sp, cast),
             wire: keep(full.wire, 3, full.wp, shown) };
  }

  return { parse, compose, model, rest, point };
})();
if (typeof module !== 'undefined') module.exports = WikiPose;
