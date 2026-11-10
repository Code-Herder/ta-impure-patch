// tascene-ranges.js — the RANGE RINGS lab: BAR's weapon range preview on TA's
// own reach rule, drawn over the tascene composite (tascene-view.html, `rings=1`).
//
// The design is the 2026-11-09 grill (research/notes/weapon-range-rings.md):
//   * WHEN, WHAT and the LOOK are BAR's two widgets, gui_attackrange_gl4.lua and
//     gui_defenserange_gl4.lua: selected units, the unit under the cursor, the
//     builders' build range while a build command is active, a defense being
//     placed (which also lights every allied ring of its class), and the enemy's
//     and ally's defenses by class. Per class a merged union outline, a faint
//     fill (attack only) and faint per-unit inner rings, BAR's colours, widths,
//     camera fades and mouse highlight.
//   * The REACH is TA's: a target is in range when its horizontal distance is
//     at most `range` (can-aim 0x49AA80, can-engage 0x49ABB0), and a ballistic
//     weapon must also pass the engine's solver 0x49A890. The ring stops, in
//     each direction, at the first point the engine would refuse.
//   * Rings sit on the water surface, as BAR's do.
//
// Everything here is the CPU reference of what the DLL will do on the GPU; the
// lab caches ring shapes because the scene is static.
(function () {
"use strict";

// ---------------------------------------------------------------- BAR's tables
// gui_attackrange_gl4.lua colorConfig (l.84-182): colour, fade (start, end,
// startAlpha, endAlpha) by camera distance in elmos, outer/inner widths in px.
const ATTACK = {
  ground: { c: [1.0, 0.22, 0.05, 0.60], fade: [4000, 6000, 1, 0], w: [3.0, 1.8], dim: 0.75 },
  nano:   { c: [0.24, 1.0, 0.2, 0.60],  fade: [2000, 4000, 1, 0], w: [3.0, 2.0], dim: 0.05 },
  AA:     { c: [0.8, 0.44, 2.0, 0.40],  fade: [2000, 5000, 1, 0], w: [3.0, 1.8], dim: 0.75 },
  cannon: { c: [1.0, 0.22, 0.05, 0.60], fade: [5000, 8500, 1, 0], w: [3.0, 1.8], dim: 0.75 },
  lrpc:   { c: [1.0, 0.22, 0.05, 0.60], fade: [5000, 1000, 1, 0.5], w: [2.0, 0], dim: 0.75 },
};
const ATTACK_SUB = {           // sub-styles replace the colour (l.312-326)
  water: [0.32, 0.48, 1.0, 0.30],
  emp:   [0.72, 0.72, 1.0, 0.60],
};
const ATTACK_ALPHA = { outer: 0.75, inner: 0.12, fill: 0.12, outerFadeOffset: 2500 };
// mask channels: ground and cannon share one union, lrpc is never merged
const ATTACK_BIT = { ground: 0, cannon: 0, nano: 1, AA: 2 };

// gui_defenserange_gl4.lua (l.82-146): no fill, mouse highlight x5
const DEFENSE = {
  ground: { c: [1.3, 0.18, 0.04, 0.70], fade: [2200, 5500, 1.0, 0], w: [5.0, 3.0], a: [0.75, 0.10] },
  air:    { c: [0.8, 0.44, 1.6, 0.70],  fade: [3200, 8000, 0.4, 0], w: [5.0, 3.0], a: [0.75, 0.10] },
  nuke:   { c: [1.05, 1.0, 0.2, 0.72],  fade: [6000, 3000, 0.6, 0], w: [5.0, 2.0], a: [0.80, 0.15] },
  cannon: { c: [1.3, 0.18, 0.04, 0.70], fade: [2000, 8000, 0.8, 0], w: [5.0, 2.5], a: [0.75, 0.10] },
  lrpc:   { c: [1.3, 0.18, 0.04, 0.68], fade: [9000, 6000, 0.8, 0], w: [3.0, 1.5], a: [0.25, 0.0] },
};
const DEFENSE_WATER = [0.48, 0.67, 1.0, 0.30];
const DEFENSE_BIT = { ground: 0, air: 1, nuke: 2, cannon: 3 };
const DEFENSE_CLASSES = ["ground", "air", "nuke", "cannon", "lrpc"];
// buttonConfig (l.77-80)
const DEFENSE_DEFAULT = {
  ally:  { ground: false, air: false, nuke: true, cannon: false, lrpc: false },
  enemy: { ground: true,  air: true,  nuke: true, cannon: true,  lrpc: false },
};

const SEL_CAP = Math.floor(512 * 0.7);     // selectionDisableThreshold x mult = 358
const LRPC_RANGE = 2600, GROUND_MAX = 700;  // BAR's classification thresholds
const FOV_SPAN = 2 * Math.tan(Math.PI / 8); // BAR's 45 deg camera: visible height / distance

// ---------------------------------------------------------------- the reach
// The engine's ballistic solver 0x49A890 (DISASSEMBLED 2026-11-09): dy is
// shooter minus target altitude, d the horizontal distance, v the weapon's
// velocity in world units per tick, g gravity in world units per tick^2.
// D' = v^4 + 2 g dy v^2 - g^2 d^2; unreachable when negative. The two roots
// s = v^2 cos^2(theta) give the angles acos(sqrt(s)/v) -- 0x4E67F0 is acos,
// DISASSEMBLED: atan2(sqrt(1-x^2), x) -- so a downhill shot's negative pitch
// counts by its size. A root <= 0 is pi/2. An angle is taken when
// minbarrel < angle <= pi/4.
function solverAccepts(d, dy, v, g, minbarrel) {
  const v2 = v * v;
  const Dp = v2 * v2 + 2 * g * dy * v2 - g * g * d * d;
  if (Dp < 0) return false;
  const d2 = d * d, den = 2 * (d2 + dy * dy);
  if (den <= 0) return false;
  const sq = d2 * Math.sqrt(Dp), base = d2 * (v2 + g * dy);
  for (const s of [(base + sq) / den, (base - sq) / den]) {
    const a = s <= 0 ? Math.PI / 2 : Math.acos(Math.min(1, Math.sqrt(s) / v));
    if (a > minbarrel && a <= Math.PI / 4 + 1e-12) return true;
  }
  return false;
}

// ---------------------------------------------------------------- classes
// gui_attackrange_gl4.lua l.762-795, on TA's weapon flags (weapons.tdf): AA is
// toairweapon (TA's "only targets air"); a ballistic weapon is a cannon, lrpc
// past 2600; anything else is ground below 700, lrpc past 2600. Dropped bombs
// get no ring (BAR skips AircraftBomb). Sub-styles: paralyzer -> emp, a
// command-fire weapon that is not stockpiled (the commander's D-gun) -> dgun,
// waterweapon -> water.
function attackClass(w) {
  if (w.toairweapon) return "AA";
  if (w.ballistic) return w.range > LRPC_RANGE ? "lrpc" : "cannon";
  if (w.range < GROUND_MAX) return "ground";
  return w.range > LRPC_RANGE ? "lrpc" : "ground";
}
function subStyle(w) {
  if (w.paralyzer) return "emp";
  if (w.commandfire && !w.stockpile) return "dgun";
  if (w.waterweapon) return "water";
  return null;
}
// The defense widget's classes, derived from data instead of BAR's list: an
// interceptor is `nuke` at its coverage, toair is `air`, otherwise the attack
// rule's ground / cannon / lrpc.
function defenseClass(w) {
  if (w.interceptor) return "nuke";
  if (w.toairweapon) return "air";
  return attackClass(w);
}

// A unit type's rings: one per weapon, identical ones collapsed (same class,
// style, range and ballistics), and the build range for a builder. An
// interceptor has no attack ring (its coverage is the defense widget's nuke
// ring; its `range` is the 32000 it can be told to fire at).
function attackRings(T) {
  const out = [], seen = new Set();
  for (const w of T.weapons) {
    if (w.dropped || w.interceptor || w.range <= 0) continue;
    const cls = attackClass(w), sub = subStyle(w);
    const key = [cls, sub, w.range, w.ballistic, w.v, w.minbarrel].join("|");
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ cls, sub, range: w.range, ballistic: w.ballistic, v: w.v,
               minbarrel: w.minbarrel, label: w.name });
  }
  if (T.builder && T.builddist > 0)
    out.push({ cls: "nano", sub: null, range: T.builddist, ballistic: false, label: "build" });
  return out;
}
function defenseRings(T) {
  if (T.mobile) return [];
  const out = [], seen = new Set();
  for (const w of T.weapons) {
    if (w.dropped) continue;
    const cls = defenseClass(w);
    const range = cls === "nuke" ? w.coverage : w.range;
    if (range <= 0) continue;
    const key = [cls, w.waterweapon, range, w.ballistic && cls !== "nuke", w.v, w.minbarrel].join("|");
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({ cls, water: w.waterweapon, range, ballistic: w.ballistic && cls !== "nuke",
               v: w.v, minbarrel: w.minbarrel, label: w.name });
  }
  return out;
}

// ---------------------------------------------------------------- sensors
// The placement preview of a sensor building, on the engine's coverage rule
// (the radar pass 0x467440): a flat circle on x/z, no terrain. BAR's look,
// gui_sensor_ranges_radar_preview.lua (l.42-82) for radar and
// gui_sensor_ranges_jammer_preview.lua (l.98-135) for the jammers; sonar
// takes the radar preview's structure in gui_sensor_ranges_sonar.lua's colour.
//   outline: the union outline of the new ring and the existing coverage
//   sheet:   the fill under the new ring
//   muted:   the fill of the existing coverage alone, at mutedRel of the sheet
const SENSOR = {
  radar:    { outline: [0.45, 1.0, 0.57, 0.40], sheet: [0.22, 0.85, 0.50, 0.20],
              muted: [0.35, 0.62, 0.50], mutedRel: 0.55, w: 2.0, dash: false, water: false },
  sonar:    { outline: [0.50, 0.70, 0.90, 0.45], sheet: [0.40, 0.60, 0.85, 0.20],
              muted: [0.35, 0.48, 0.62], mutedRel: 0.55, w: 2.0, dash: false, water: true },
  radarjam: { outline: [1.0, 0.32, 0.22, 0.80], sheet: [0.90, 0.22, 0.20, 0.17],
              muted: [0.55, 0.25, 0.45], mutedRel: 0.55, w: 2.0, dash: true, water: false },
  sonarjam: { outline: [1.0, 0.32, 0.22, 0.80], sheet: [0.90, 0.22, 0.20, 0.17],
              muted: [0.55, 0.25, 0.45], mutedRel: 0.55, w: 2.0, dash: true, water: true },
};
const SENSOR_KINDS = ["radar", "sonar", "radarjam", "sonarjam"];
// a type's sensors that will emit: a value above 0 on a def that switches on
// when built or that the player can switch on (Q1 of the 2026-11-09 grill)
function sensorKinds(T) {
  if (!T || !(T.awb || T.onoff)) return [];
  return SENSOR_KINDS.filter((k) => (T[k] || 0) > 0);
}
// the radius the engine tests: radar is (RadarDistance + 2 * altitude)^2, but
// the bucket walk only visits units within max(radar, sonar), so the bonus
// survives only past a larger sonar (0x46755E..0x4675A3)
function sensorRadius(T, kind, alt) {
  if (kind !== "radar") return T[kind];
  return Math.min(T.radar + 2 * Math.trunc(alt), Math.max(T.radar, T.sonar || 0));
}

// ---------------------------------------------------------------- GL
const SFILL_FS = `#version 300 es
precision highp float;
uniform sampler2D uMask;
uniform vec4 uSheet;         // premultiplied: under the new ring
uniform vec4 uMuted;         // premultiplied: the existing coverage alone
out vec4 o;
void main() {
  vec4 m = texelFetch(uMask, ivec2(gl_FragCoord.xy), 0);
  o = m.r > 0.5 ? uSheet : (m.g > 0.5 ? uMuted : vec4(0.0));
}`;
const LINE_VS = `#version 300 es
precision highp float;
layout(location=0) in vec2 aPos;    // canvas px
layout(location=1) in float aEdge;  // -1..1 across the line
layout(location=2) in vec4 aCol;    // straight alpha
layout(location=3) in float aHalf;  // half width in px (including the AA rim)
uniform vec2 uCanvas;
out float vEdge; out vec4 vCol; out float vHalf;
void main() {
  vEdge = aEdge; vCol = aCol; vHalf = aHalf;
  gl_Position = vec4(aPos.x / uCanvas.x * 2.0 - 1.0, 1.0 - aPos.y / uCanvas.y * 2.0, 0.0, 1.0);
}`;
const LINE_FS = `#version 300 es
precision highp float;
precision highp int;
in float vEdge; in vec4 vCol; in float vHalf;
uniform sampler2D uMask;
uniform int uMaskBit;        // -1: no clip
uniform float uCanvasH;
out vec4 o;
void main() {
  if (uMaskBit >= 0) {
    vec4 m = texelFetch(uMask, ivec2(gl_FragCoord.x, gl_FragCoord.y), 0);
    if (m[uMaskBit] > 0.5) discard;   // inside the merged area of this class
  }
  float px = abs(vEdge) * vHalf;
  float a = clamp(vHalf - px, 0.0, 1.0);   // one px of AA at the rim
  vec3 c = min(vCol.rgb, vec3(1.0));
  float al = clamp(vCol.a, 0.0, 1.0) * a;
  o = vec4(c * al, al);
}`;
const MASK_VS = `#version 300 es
precision highp float;
layout(location=0) in vec2 aPos;
uniform vec2 uCanvas;
void main() {
  gl_Position = vec4(aPos.x / uCanvas.x * 2.0 - 1.0, 1.0 - aPos.y / uCanvas.y * 2.0, 0.0, 1.0);
}`;
const MASK_FS = `#version 300 es
precision highp float;
out vec4 o;
void main() { o = vec4(1.0); }`;
const FILL_VS = `#version 300 es
precision highp float;
const vec2 P[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
void main() { gl_Position = vec4(P[gl_VertexID], 0.0, 1.0); }`;
const FILL_FS = `#version 300 es
precision highp float;
uniform sampler2D uMask;
uniform vec4 uCol[4];        // premultiplied per channel
out vec4 o;
void main() {
  vec4 m = texelFetch(uMask, ivec2(gl_FragCoord.xy), 0);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 4; i++) if (m[i] > 0.5) acc = uCol[i] + acc * (1.0 - uCol[i].a);
  o = acc;
}`;
const PT_VS = `#version 300 es
precision highp float;
layout(location=0) in vec2 aPos;
layout(location=1) in vec4 aCol;
uniform vec2 uCanvas; uniform float uSize;
out vec4 vCol;
void main() {
  vCol = aCol; gl_PointSize = uSize;
  gl_Position = vec4(aPos.x / uCanvas.x * 2.0 - 1.0, 1.0 - aPos.y / uCanvas.y * 2.0, 0.0, 1.0);
}`;
const PT_FS = `#version 300 es
precision highp float;
in vec4 vCol; out vec4 o;
void main() { o = vec4(vCol.rgb * vCol.a, vCol.a); }`;

function compile(gl, vs, fs, name) {
  const sh = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
      throw new Error(`${name}: ${gl.getShaderInfoLog(s)}`);
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, sh(gl.VERTEX_SHADER, vs));
  gl.attachShader(p, sh(gl.FRAGMENT_SHADER, fs));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`${name}: ${gl.getProgramInfoLog(p)}`);
  return p;
}

// ---------------------------------------------------------------- the lab
function create(ctx) {
  const { gl, GW, GH, scene, heightBuf } = ctx;
  const R = scene.ranges;
  if (!R) throw new Error("rings=1, but this pack has no range facts.\n" +
                          "Rebuild it with:  tools/tascene build <scenario> -o <pack>");
  const map = scene.map, W16 = map.w16, H16 = map.h16, SEA = map.sealevel || 0;
  const G = R.g16 / 65536;                     // world units per tick^2
  // where the map ends on screen: the engine's scroll extent (tagpu_fx.h
  // tagpu_edge_area), the tile map less 32 wide and 128 tall
  const MAPW = W16 * 16 - 32, MAPH = H16 * 16 - 128;

  // GetPosHeight 0x485070: bilinear over the 16-wu height grid, -1 off it
  function ground(x, z) {
    const fx = x / 16, fz = z / 16;
    const c = Math.floor(fx), r = Math.floor(fz);
    if (c < 0 || r < 0 || c + 1 >= W16 || r + 1 >= H16) {
      const cc = Math.min(W16 - 1, Math.max(0, c)), rr = Math.min(H16 - 1, Math.max(0, r));
      return heightBuf[rr * W16 + cc];
    }
    const tx = fx - c, tz = fz - r, i = r * W16 + c;
    const a = heightBuf[i], b = heightBuf[i + 1], d = heightBuf[i + W16], e = heightBuf[i + W16 + 1];
    return (a * (1 - tx) + b * tx) * (1 - tz) + (d * (1 - tx) + e * tx) * tz;
  }
  // where a ring vertex sits and what a target there stands on: the water surface
  const surface = (x, z) => Math.max(ground(x, z), SEA);

  // ---- units: the pack's, plus the defenses placed in the lab
  const types = R.types;
  const units = [];
  (scene.units ? scene.units.instances : []).forEach((inst, i) => {
    if (inst.feature || !types[inst.type]) return;
    units.push({ id: inst.id || `#${i}`, type: inst.type, owner: inst.owner || 0,
                 x: inst.pos[0], z: inst.pos[1], agl: inst.agl || 0, placed: false });
  });
  // the shooter's altitude, the unit's own +0x6A y: the ground, the water
  // surface for anything not flying, cruise height for aircraft
  const unitAlt = (u) => {
    const T = types[u.type];
    return T && T.canfly ? ground(u.x, u.z) + u.agl : surface(u.x, u.z);
  };
  const ME = 0;
  const isAlly = (u) => u.owner === ME;

  // ---- ring shapes in world space, cached (the scene is static)
  const shapeCache = new Map();
  function ringShape(u, ring, nseg) {
    const key = `${u.x},${u.z}|${ring.range}|${ring.sub || ""}|${ring.ballistic ? ring.v + "," + ring.minbarrel : "f"}|${nseg}`;
    let s = shapeCache.get(key);
    if (s) return s;
    const y0 = unitAlt(u);
    const pts = new Float32Array(nseg * 3), rad = new Float32Array(nseg);
    for (let i = 0; i < nseg; i++) {
      const a = (i / nseg) * Math.PI * 2;
      const dx = Math.sin(a), dz = Math.cos(a);
      let r = ring.range * (ring.sub === "dgun" ? 1.05 : 1);   // BAR draws the D-gun 5 % out
      if (ring.ballistic) r = reachAlong(u.x, u.z, y0, dx, dz, ring);
      const x = u.x + dx * r, z = u.z + dz * r;
      pts[i * 3] = x; pts[i * 3 + 1] = surface(x, z); pts[i * 3 + 2] = z;
      rad[i] = r;
    }
    s = { pts, rad, nseg };
    shapeCache.set(key, s);
    if (shapeCache.size > 4000) shapeCache.delete(shapeCache.keys().next().value);
    return s;
  }
  const STEP = 8;      // half a height cell
  function reachable(x0, z0, y0, d, dx, dz, ring) {
    if (d > ring.range) return false;
    const x = x0 + dx * d, z = z0 + dz * d;
    return solverAccepts(d, y0 - surface(x, z), ring.v, G, ring.minbarrel);
  }
  // the first point along (dx, dz) the engine refuses, refined by 8 halvings
  function reachAlong(x0, z0, y0, dx, dz, ring) {
    let lo = 0, hi = -1;
    for (let d = STEP; d <= ring.range; d += STEP) {
      if (!reachable(x0, z0, y0, d, dx, dz, ring)) { hi = d; break; }
      lo = d;
    }
    if (hi < 0) {
      if (lo < ring.range && !reachable(x0, z0, y0, ring.range, dx, dz, ring)) hi = ring.range;
      else return ring.range;
    }
    for (let k = 0; k < 8; k++) {
      const mid = (lo + hi) / 2;
      if (mid > 0 && reachable(x0, z0, y0, mid, dx, dz, ring)) lo = mid; else hi = mid;
    }
    return lo;
  }

  // ---- settings: the Visuals block, persisted (the DLL keeps them in impure.cfg)
  const KEY = "tascene-rangerings";
  const settings = {
    on: true, shiftOnly: false, cursorRange: true,
    ally: { ...DEFENSE_DEFAULT.ally }, enemy: { ...DEFENSE_DEFAULT.enemy },
    toggles: {},          // type -> { ally: [bool…], enemy: [bool…] } (alt+. / alt+,)
    reach: false,         // lab only: paint the engine's reachable cells
    sensor: true,         // the sensor rings while a sensor building is placed
  };
  try {
    const s = JSON.parse(localStorage.getItem(KEY) || "null");
    if (s) Object.assign(settings, s, { ally: { ...settings.ally, ...s.ally },
                                        enemy: { ...settings.enemy, ...s.enemy } });
  } catch (_) {}
  const save = () => { try { localStorage.setItem(KEY, JSON.stringify(settings)); } catch (_) {} };

  // ---- state
  const selected = new Set();       // unit ids
  let hover = null;                 // unit id
  let mouse = null;                 // world [x, z] under the pointer
  let shift = false;
  let placing = null;               // unit type being placed
  let message = "";
  const byId = () => new Map(units.map((u) => [u.id, u]));
  // a state as a link (and what a headless shot draws): rrsel=<id>,<id>…
  // selects, rrplace=<type>@<x>,<z> holds a placement there, rrreach=1 paints
  // the reachable cells, rrshift=1 holds Shift
  const Q = ctx.query;
  if (Q) {
    for (const id of (Q.get("rrsel") || "").split(",").filter(Boolean)) selected.add(id);
    const pl = /^([a-z0-9_]+)@(-?\d+),(-?\d+)$/i.exec(Q.get("rrplace") || "");
    if (pl && types[pl[1].toLowerCase()]) { placing = pl[1].toLowerCase(); mouse = [+pl[2], +pl[3]]; }
    if (Q.get("rrreach") === "1") settings.reach = true;
    if (Q.get("rrshift") === "1") shift = true;
    if (Q.get("rrsensor") === "0") settings.sensor = false;
    if (Q.get("rrdef") === "0")
      for (const side of ["ally", "enemy"]) for (const c of DEFENSE_CLASSES) settings[side][c] = false;
  }

  const ringEnabled = (u, idx, n) => {
    const t = settings.toggles[u.type];
    const arr = t && t[isAlly(u) ? "ally" : "enemy"];
    return !arr || arr.length !== n || arr[idx];
  };

  // ---- GL objects
  const lineProg = compile(gl, LINE_VS, LINE_FS, "range lines");
  const maskProg = compile(gl, MASK_VS, MASK_FS, "range mask");
  const fillProg = compile(gl, FILL_VS, FILL_FS, "range fill");
  const ptProg = compile(gl, PT_VS, PT_FS, "range reach");
  const sfillProg = compile(gl, FILL_VS, SFILL_FS, "sensor fill");
  const U = (p, names) => Object.fromEntries(names.map((n) => [n, gl.getUniformLocation(p, n)]));
  const LU = U(lineProg, ["uCanvas", "uMask", "uMaskBit"]);
  const MU = U(maskProg, ["uCanvas"]);
  const FU = U(fillProg, ["uMask", "uCol"]);
  const PU = U(ptProg, ["uCanvas", "uSize"]);
  const SU = U(sfillProg, ["uMask", "uSheet", "uMuted"]);
  const TEX_UNIT = 14;
  const maskTex = gl.createTexture();
  gl.activeTexture(gl.TEXTURE0 + TEX_UNIT);
  gl.bindTexture(gl.TEXTURE_2D, maskTex);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, GW, GH, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.activeTexture(gl.TEXTURE0);
  const maskFBO = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, maskFBO);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, maskTex, 0);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  const vao = gl.createVertexArray(), vbo = gl.createBuffer();
  gl.bindVertexArray(vao);
  gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
  // line layout: x, y, edge, r, g, b, a, half = 8 floats
  for (const [loc, n, off] of [[0, 2, 0], [1, 1, 8], [2, 4, 12], [3, 1, 28]]) {
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, n, gl.FLOAT, false, 32, off);
  }
  gl.bindVertexArray(null);
  const mvao = gl.createVertexArray(), mvbo = gl.createBuffer();
  gl.bindVertexArray(mvao);
  gl.bindBuffer(gl.ARRAY_BUFFER, mvbo);
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 8, 0);
  gl.bindVertexArray(null);
  const pvao = gl.createVertexArray(), pvbo = gl.createBuffer();
  gl.bindVertexArray(pvao);
  gl.bindBuffer(gl.ARRAY_BUFFER, pvbo);
  gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 24, 0);
  gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 24, 8);
  gl.bindVertexArray(null);

  let lineBuf = new Float32Array(1 << 16), lineN = 0;
  const pushV = (x, y, e, c, half) => {
    if (lineN + 8 > lineBuf.length) { const g = new Float32Array(lineBuf.length * 2); g.set(lineBuf); lineBuf = g; }
    lineBuf.set([x, y, e, c[0], c[1], c[2], c[3], half], lineN); lineN += 8;
  };
  let maskBuf = new Float32Array(1 << 16), maskN = 0;
  const pushM = (x, y) => {
    if (maskN + 2 > maskBuf.length) { const g = new Float32Array(maskBuf.length * 2); g.set(maskBuf); maskBuf = g; }
    maskBuf[maskN++] = x; maskBuf[maskN++] = y;
  };

  // ---- one frame
  function draw() {
    const { proj, zoom, vp } = ctx.view();
    const [vpL, vpT, vw, vh] = vp;
    // the camera distance BAR's fades read: the 45-degree camera that shows
    // as much ground as this view does
    const camDist = (vh / zoom) / FOV_SPAN;
    const ids = byId();

    // what each layer draws: [{u, ring, mode: "sel"|"hover"|"ghost", style}]
    const attack = { ally: [], enemy: [] }, defense = { ally: [], enemy: [] };
    const showAttack = settings.on && (!settings.shiftOnly || shift);
    let selCount = 0, builderCount = 0;
    if (showAttack) {
      const sel = [...selected].map((id) => ids.get(id)).filter(Boolean).slice(0, SEL_CAP);
      selCount = sel.length;
      const add = (u, mode) => {
        const rings = attackRings(types[u.type]);
        rings.forEach((ring, k) => {
          if (!ringEnabled(u, k, rings.length)) return;
          (isAlly(u) ? attack.ally : attack.enemy).push({ u, ring, mode });
        });
      };
      for (const u of sel) add(u, "sel");
      if (settings.cursorRange && hover && !selected.has(hover)) {
        const u = ids.get(hover);
        if (u) add(u, "hover");
      }
      // a build command is active: every own builder's build range
      if (placing) {
        for (const u of units) {
          const T = types[u.type];
          if (!isAlly(u) || !T.builder || T.builddist <= 0 || selected.has(u.id)) continue;
          attack.ally.push({ u, ring: { cls: "nano", range: T.builddist, ballistic: false }, mode: "sel" });
        }
      }
      builderCount = attack.ally.filter((e) => e.ring.cls === "nano").length;
    }
    // defenses: enemies and allies by class, plus the placement preview, which
    // also turns on every allied ring of its class
    let ghost = null;
    const override = new Set();
    if (settings.on && placing && mouse) {
      const T = types[placing];
      ghost = { id: "ghost", type: placing, owner: ME, x: snap(mouse[0], T.foot[0]),
                z: snap(mouse[1], T.foot[1]), agl: 0 };
      for (const ring of defenseRings(T)) { override.add(ring.cls); defense.ally.push({ u: ghost, ring, mode: "ghost" }); }
    }
    if (settings.on) {
      for (const u of units) {
        const T = types[u.type];
        if (!T || T.mobile) continue;
        const side = isAlly(u) ? "ally" : "enemy", want = settings[side];
        for (const ring of defenseRings(T))
          if (want[ring.cls] || (side === "ally" && override.has(ring.cls)))
            defense[side].push({ u, ring, mode: "static" });
      }
    }

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, GW, GH);
    gl.disable(gl.DEPTH_TEST);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(vpL, GH - vpT - vh, vw, vh);

    if (settings.reach) drawReach(proj, ids);

    // the sensor layer is under the weapon rings: larger and fainter
    if (settings.on) drawSensors(ghost, proj);

    // the defense widget's layer is under the attack widget's (layer -100 vs -99)
    for (const side of ["ally", "enemy"])
      drawLayer(defense[side], "defense", proj, camDist, selCount, builderCount, vp);
    for (const side of ["ally", "enemy"])
      drawLayer(attack[side], "attack", proj, camDist, selCount, builderCount, vp);
    drawMarks(proj, ids, ghost);
    gl.disable(gl.SCISSOR_TEST);
    gl.disable(gl.BLEND);
  }

  // Pos2BuildPos: a footprint's centre on the 16-wu grid (odd footprints sit
  // on a cell centre, even ones on a cell corner)
  const snap = (v, foot) => (foot % 2 ? Math.floor(v / 16) * 16 + 8 : Math.round(v / 16) * 16);

  function styleOf(kind, ring) {
    if (kind === "attack") {
      const s = ATTACK[ring.cls];
      let c = s.c;
      if (ring.sub && ATTACK_SUB[ring.sub]) c = ATTACK_SUB[ring.sub];
      return { c, fade: s.fade, w: s.w, dim: s.dim, outerA: ATTACK_ALPHA.outer,
               innerA: ATTACK_ALPHA.inner, fadeOff: ATTACK_ALPHA.outerFadeOffset,
               bit: ring.cls in ATTACK_BIT ? ATTACK_BIT[ring.cls] : -1,
               dgun: ring.sub === "dgun", mouseMul: 1.0,
               segs: ring.cls === "AA" || ring.cls === "lrpc" ? 1024 : 128 };
    }
    const s = DEFENSE[ring.cls];
    return { c: ring.water ? DEFENSE_WATER : s.c, fade: s.fade, w: s.w, dim: 0,
             outerA: s.a[0], innerA: s.a[1], fadeOff: 0,
             bit: ring.cls in DEFENSE_BIT ? DEFENSE_BIT[ring.cls] : -1, dgun: false,
             mouseMul: 5.0, segs: ring.cls === "nuke" || ring.cls === "lrpc" ? 1024 : 128 };
  }

  // the BAR vertex shader's alpha (weapon_range_rings_unified_gl4.vert.glsl
  // l.312-437): fade by camera distance, mouse proximity, inner dimming
  function ringAlpha(st, e, camDist, outer, selCount, builderCount) {
    const [fs, fe, sa, ea] = st.fade;
    const off = outer ? st.fadeOff : 0;
    const lo = Math.min(ea, sa), hi = Math.max(ea, sa);
    let fade = (fe + off + (st.dgun ? 1000 : 0) - camDist) / (fe - fs);
    fade = Math.min(hi, Math.max(lo, fade));
    let hl = 0;
    if (mouse) {
      const d = Math.hypot(e.u.x - mouse[0], e.u.z - mouse[1]);
      const t = Math.min(1, Math.max(0, (d - 48) / 16));
      hl = (1 - t * t * (3 - 2 * t)) * st.mouseMul;
    }
    const mouseA = 0.6 * hl;
    let a = st.c[3] * (mouseA + fade * (outer ? st.outerA : st.innerA));
    if (st.dgun) a = Math.min(1, Math.max(0.1, a * 3));
    if (!outer && st.dim > 0) {
      const n = Math.min(25, Math.max(1, e.ring.cls === "nano" ? builderCount : selCount));
      const dim = st.dim * 0.1 * n;
      a *= Math.min(1, 1 / (dim * dim));
    }
    return a;
  }

  // one widget's group: mask, fill, outer outline, inner rings
  function drawLayer(list, kind, proj, camDist, selCount, builderCount, vp) {
    if (!list.length) return;
    const items = list.map((e) => {
      const st = styleOf(kind, e.ring);
      const shape = ringShape(e.u, e.ring, st.segs);
      const scr = new Float32Array(shape.nseg * 2), oob = new Float32Array(shape.nseg);
      for (let i = 0; i < shape.nseg; i++) {
        const x = shape.pts[i * 3], y = shape.pts[i * 3 + 1], z = shape.pts[i * 3 + 2];
        const [sx, sy] = proj([x, y, z]);
        scr[i * 2] = sx; scr[i * 2 + 1] = sy;
        // out-of-map vertices fade over 50 wu
        const inb = Math.min(x, z, MAPW - x, MAPH - z);
        oob[i] = 1 - Math.min(1, Math.max(0, inb * -0.02));
      }
      const [cx, cy] = proj([e.u.x, unitAlt(e.u), e.u.z]);
      return { e, st, scr, oob, c: [cx, cy] };
    });
    const merged = items.filter((it) => it.st.bit >= 0);

    // 1. the coverage mask: every disc of a class into its channel
    if (merged.length) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, maskFBO);
      gl.disable(gl.SCISSOR_TEST);
      gl.colorMask(true, true, true, true);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.disable(gl.BLEND);
      gl.useProgram(maskProg);
      gl.uniform2f(MU.uCanvas, GW, GH);
      gl.bindVertexArray(mvao);
      gl.bindBuffer(gl.ARRAY_BUFFER, mvbo);
      for (let bit = 0; bit < 4; bit++) {
        maskN = 0;
        for (const it of merged) {
          if (it.st.bit !== bit) continue;
          const n = it.scr.length / 2;
          for (let i = 0; i < n; i++) {
            const j = (i + 1) % n;
            pushM(it.c[0], it.c[1]);
            pushM(it.scr[i * 2], it.scr[i * 2 + 1]);
            pushM(it.scr[j * 2], it.scr[j * 2 + 1]);
          }
        }
        if (!maskN) continue;
        gl.colorMask(bit === 0, bit === 1, bit === 2, bit === 3);
        gl.bufferData(gl.ARRAY_BUFFER, maskBuf.subarray(0, maskN), gl.DYNAMIC_DRAW);
        gl.drawArrays(gl.TRIANGLES, 0, maskN / 2);
      }
      gl.colorMask(true, true, true, true);
      gl.bindVertexArray(null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.enable(gl.SCISSOR_TEST);
    }
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.activeTexture(gl.TEXTURE0 + TEX_UNIT);
    gl.bindTexture(gl.TEXTURE_2D, maskTex);
    gl.activeTexture(gl.TEXTURE0);

    // 2. the fill (attack only): colour.a x fill x internal alpha
    if (kind === "attack" && merged.length) {
      const cols = new Float32Array(16);
      for (const it of merged) {
        const a = it.st.c[3] * ATTACK_ALPHA.fill * ATTACK_ALPHA.inner;
        const b = it.st.bit * 4;
        cols.set([Math.min(1, it.st.c[0]) * a, Math.min(1, it.st.c[1]) * a,
                  Math.min(1, it.st.c[2]) * a, a], b);
      }
      gl.useProgram(fillProg);
      gl.uniform1i(FU.uMask, TEX_UNIT);
      gl.uniform4fv(FU.uCol, cols);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    }

    // 3. outer rings through the mask, 4. inner rings, unmasked
    gl.useProgram(lineProg);
    gl.uniform2f(LU.uCanvas, GW, GH);
    gl.uniform1i(LU.uMask, TEX_UNIT);
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    for (const outer of [true, false]) {
      for (let bit = -1; bit < 4; bit++) {
        lineN = 0;
        for (const it of items) {
          if (it.st.bit !== bit && outer) continue;
          if (!outer && bit !== -1) continue;
          const w = it.st.w[outer ? 0 : 1];
          if (!w) continue;
          const a = ringAlpha(it.st, it.e, camDist, outer, selCount, builderCount);
          if (a <= 0.002) continue;
          polyline(it.scr, it.oob, w, it.st.c, a);
        }
        if (!lineN) continue;
        gl.uniform1i(LU.uMaskBit, outer ? bit : -1);
        gl.bufferData(gl.ARRAY_BUFFER, lineBuf.subarray(0, lineN), gl.DYNAMIC_DRAW);
        gl.drawArrays(gl.TRIANGLES, 0, lineN / 8);
      }
    }
    gl.bindVertexArray(null);
  }

  // a closed screen-space polyline as quads, each segment extended by half a
  // width at both ends so the joints close
  function polyline(scr, oob, width, col, alpha, dash) {
    const n = scr.length / 2, half = width / 2 + 0.5;
    for (let i = 0; i < n; i++) {
      if (dash && Math.floor(i / dash) % 2) continue;
      const j = (i + 1) % n;
      let x0 = scr[i * 2], y0 = scr[i * 2 + 1], x1 = scr[j * 2], y1 = scr[j * 2 + 1];
      const dx = x1 - x0, dy = y1 - y0, len = Math.hypot(dx, dy);
      if (len < 1e-4) continue;
      const ux = dx / len, uy = dy / len, nx = -uy * half, ny = ux * half;
      const ex = ux * width / 2, ey = uy * width / 2;
      x0 -= ex; y0 -= ey; x1 += ex; y1 += ey;
      const c0 = [col[0], col[1], col[2], alpha * oob[i]], c1 = [col[0], col[1], col[2], alpha * oob[j]];
      pushV(x0 + nx, y0 + ny, 1, c0, half); pushV(x0 - nx, y0 - ny, -1, c0, half); pushV(x1 + nx, y1 + ny, 1, c1, half);
      pushV(x1 + nx, y1 + ny, 1, c1, half); pushV(x0 - nx, y0 - ny, -1, c0, half); pushV(x1 - nx, y1 - ny, -1, c1, half);
    }
  }

  // ---- the sensor rings of a building being placed (one pass a sensor kind):
  // the new ring and every own unit that emits that sensor, merged into one
  // outline; the sheet under the new ring, the muted fill under the rest.
  // Sonar finds only targets at or below sea level (0x4678E6), so its fill and
  // outline are cut to water, a 16-wu height cell at a time.
  const waterCells = new Map();
  function waterCellsOf(x0, z0, r) {
    const key = `${x0},${z0},${r}`;
    let c = waterCells.get(key);
    if (c) return c;
    c = [];
    const r2 = r * r;
    for (let z = Math.floor((z0 - r) / 16) * 16; z < z0 + r; z += 16)
      for (let x = Math.floor((x0 - r) / 16) * 16; x < x0 + r; x += 16) {
        const cx = x + 8, cz = z + 8;
        if ((cx - x0) ** 2 + (cz - z0) ** 2 > r2) continue;
        if (x < 0 || z < 0 || x + 16 > MAPW || z + 16 > MAPH) continue;
        if (ground(cx, cz) > SEA) continue;
        c.push(x, z);
      }
    waterCells.set(key, c);
    if (waterCells.size > 64) waterCells.delete(waterCells.keys().next().value);
    return c;
  }
  function drawSensors(ghost, proj) {
    if (!settings.sensor || !ghost) return;
    for (const kind of sensorKinds(types[ghost.type])) {
      const st = SENSOR[kind];
      const list = [{ u: ghost, isNew: true }];
      for (const u of units)
        if (isAlly(u) && sensorKinds(types[u.type]).includes(kind)) list.push({ u, isNew: false });
      const items = list.map((e) => {
        const T = types[e.u.type], alt = unitAlt(e.u);
        const r = sensorRadius(T, kind, alt), nseg = r > 1500 ? 1024 : 512;
        const scr = new Float32Array(nseg * 2), vis = new Float32Array(nseg);
        const disc = new Float32Array(nseg * 2);   // the disc cut to the scroll extent
        for (let i = 0; i < nseg; i++) {
          const a = (i / nseg) * Math.PI * 2;
          const x = e.u.x + Math.sin(a) * r, z = e.u.z + Math.cos(a) * r;
          const [sx, sy] = proj([x, surface(x, z), z]);
          scr[i * 2] = sx; scr[i * 2 + 1] = sy;
          const cx = Math.min(MAPW, Math.max(0, x)), cz = Math.min(MAPH, Math.max(0, z));
          const [dx, dy] = proj([cx, surface(cx, cz), cz]);
          disc[i * 2] = dx; disc[i * 2 + 1] = dy;
          const inb = Math.min(x, z, MAPW - x, MAPH - z);
          vis[i] = (1 - Math.min(1, Math.max(0, inb * -0.02))) * (st.water && ground(x, z) > SEA ? 0 : 1);
        }
        const [cx, cy] = proj([e.u.x, alt, e.u.z]);
        return { ...e, r, scr, vis, disc, c: [cx, cy] };
      });

      // the mask: R the new ring's fill, G the existing coverage's fill (both
      // cut to water for sonar), A the union of the whole discs, which the
      // outline is clipped by
      gl.bindFramebuffer(gl.FRAMEBUFFER, maskFBO);
      gl.disable(gl.SCISSOR_TEST);
      gl.colorMask(true, true, true, true);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.disable(gl.BLEND);
      gl.useProgram(maskProg);
      gl.uniform2f(MU.uCanvas, GW, GH);
      gl.bindVertexArray(mvao);
      gl.bindBuffer(gl.ARRAY_BUFFER, mvbo);
      const flush = (r, g, a) => {
        if (!maskN) return;
        gl.colorMask(r, g, false, a);
        gl.bufferData(gl.ARRAY_BUFFER, maskBuf.subarray(0, maskN), gl.DYNAMIC_DRAW);
        gl.drawArrays(gl.TRIANGLES, 0, maskN / 2);
      };
      for (const isNew of [true, false]) {
        maskN = 0;
        for (const it of items) {
          if (it.isNew !== isNew) continue;
          const n = it.disc.length / 2;
          for (let i = 0; i < n; i++) {
            const j = (i + 1) % n;
            pushM(it.c[0], it.c[1]);
            pushM(it.disc[i * 2], it.disc[i * 2 + 1]);
            pushM(it.disc[j * 2], it.disc[j * 2 + 1]);
          }
        }
        flush(!st.water && isNew, !st.water && !isNew, true);
        if (!st.water) continue;
        maskN = 0;
        for (const it of items) {
          if (it.isNew !== isNew) continue;
          const cells = waterCellsOf(it.u.x, it.u.z, it.r);
          for (let k = 0; k < cells.length; k += 2) {
            const x = cells[k], z = cells[k + 1];
            const p00 = proj([x, SEA, z]), p10 = proj([x + 16, SEA, z]);
            const p01 = proj([x, SEA, z + 16]), p11 = proj([x + 16, SEA, z + 16]);
            pushM(p00[0], p00[1]); pushM(p10[0], p10[1]); pushM(p11[0], p11[1]);
            pushM(p00[0], p00[1]); pushM(p11[0], p11[1]); pushM(p01[0], p01[1]);
          }
        }
        flush(isNew, !isNew, false);
      }
      gl.colorMask(true, true, true, true);
      gl.bindVertexArray(null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.enable(gl.SCISSOR_TEST);

      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.activeTexture(gl.TEXTURE0 + TEX_UNIT);
      gl.bindTexture(gl.TEXTURE_2D, maskTex);
      gl.activeTexture(gl.TEXTURE0);

      // the fill
      const sa = st.sheet[3], ma = st.sheet[3] * st.mutedRel;
      gl.useProgram(sfillProg);
      gl.uniform1i(SU.uMask, TEX_UNIT);
      gl.uniform4f(SU.uSheet, st.sheet[0] * sa, st.sheet[1] * sa, st.sheet[2] * sa, sa);
      gl.uniform4f(SU.uMuted, st.muted[0] * ma, st.muted[1] * ma, st.muted[2] * ma, ma);
      gl.drawArrays(gl.TRIANGLES, 0, 3);

      // the union outline: each ring's line survives only outside the union
      lineN = 0;
      for (const it of items)
        polyline(it.scr, it.vis, st.w, st.outline, st.outline[3], st.dash ? 6 : 0);
      if (lineN) {
        gl.useProgram(lineProg);
        gl.uniform2f(LU.uCanvas, GW, GH);
        gl.uniform1i(LU.uMask, TEX_UNIT);
        gl.uniform1i(LU.uMaskBit, 3);
        gl.bindVertexArray(vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
        gl.bufferData(gl.ARRAY_BUFFER, lineBuf.subarray(0, lineN), gl.DYNAMIC_DRAW);
        gl.drawArrays(gl.TRIANGLES, 0, lineN / 8);
        gl.bindVertexArray(null);
      }
    }
  }

  // lab marks: the selection, the hovered unit, placed defenses and the ghost
  // as footprint outlines (the DLL draws the engine's own markers instead)
  function drawMarks(proj, ids, ghost) {
    lineN = 0;
    const box = (u, col) => {
      const T = types[u.type];
      const hx = T.foot[0] * 8, hz = T.foot[1] * 8, y = unitAlt(u);
      const corners = [[-hx, -hz], [hx, -hz], [hx, hz], [-hx, hz]];
      const scr = new Float32Array(8), oob = new Float32Array([1, 1, 1, 1]);
      corners.forEach(([ox, oz], k) => {
        const [sx, sy] = proj([u.x + ox, y, u.z + oz]);
        scr[k * 2] = sx; scr[k * 2 + 1] = sy;
      });
      polyline(scr, oob, 1.5, col, col[3]);
    };
    for (const u of units) {
      if (selected.has(u.id)) box(u, [0.3, 1.0, 0.3, 0.9]);
      else if (u.id === hover) box(u, [1, 1, 1, 0.6]);
      else if (u.placed) box(u, [0.7, 0.8, 1.0, 0.5]);
    }
    if (ghost) box(ghost, [0.6, 1.0, 0.6, 0.8]);
    if (!lineN) return;
    gl.useProgram(lineProg);
    gl.uniform2f(LU.uCanvas, GW, GH);
    gl.uniform1i(LU.uMaskBit, -1);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo);
    gl.bufferData(gl.ARRAY_BUFFER, lineBuf.subarray(0, lineN), gl.DYNAMIC_DRAW);
    gl.drawArrays(gl.TRIANGLES, 0, lineN / 8);
    gl.bindVertexArray(null);
  }

  // LAB ONLY: every 16-wu cell around each selected ballistic weapon, green
  // where the engine would fire and red where it would not, so the ring's
  // first-refusal edge can be judged against the true reachable set
  const reachCache = new Map();
  function drawReach(proj, ids) {
    const pts = [];
    for (const id of selected) {
      const u = ids.get(id);
      if (!u) continue;
      for (const ring of attackRings(types[u.type])) {
        if (!ring.ballistic) continue;
        const key = `${u.x},${u.z}|${ring.range}|${ring.v}|${ring.minbarrel}`;
        let cells = reachCache.get(key);
        if (!cells) {
          cells = [];
          const y0 = unitAlt(u), R0 = ring.range;
          for (let z = u.z - R0; z <= u.z + R0; z += 16)
            for (let x = u.x - R0; x <= u.x + R0; x += 16) {
              const d = Math.hypot(x - u.x, z - u.z);
              if (d > R0 + 16 || d < 1) continue;
              const ok = d <= R0 && solverAccepts(d, y0 - surface(x, z), ring.v, G, ring.minbarrel);
              cells.push(x, surface(x, z), z, ok ? 1 : 0);
            }
          reachCache.set(key, cells);
        }
        for (let i = 0; i < cells.length; i += 4) {
          const [sx, sy] = proj([cells[i], cells[i + 1], cells[i + 2]]);
          pts.push(sx, sy, cells[i + 3] ? 0.2 : 1.0, cells[i + 3] ? 1.0 : 0.15, 0.2, 0.35);
        }
      }
    }
    if (!pts.length) return;
    gl.useProgram(ptProg);
    gl.uniform2f(PU.uCanvas, GW, GH);
    gl.uniform1f(PU.uSize, Math.max(2, 16 * ctx.view().zoom * 0.5));
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(pvao);
    gl.bindBuffer(gl.ARRAY_BUFFER, pvbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(pts), gl.DYNAMIC_DRAW);
    gl.drawArrays(gl.POINTS, 0, pts.length / 6);
    gl.bindVertexArray(null);
  }

  // ---- input (the viewer calls these)
  // the unit under a world point: within its footprint, else 24 wu
  function pick(g) {
    let best = null, bd = Infinity;
    for (const u of units) {
      const T = types[u.type];
      const r = Math.max(24, Math.max(T.foot[0], T.foot[1]) * 8);
      const d = Math.hypot(u.x - g[0], u.z - g[1]);
      if (d < r && d < bd) { bd = d; best = u.id; }
    }
    return best;
  }
  function onMove(g) {
    mouse = g;
    hover = g ? pick(g) : null;
  }
  function onClick(g, e) {
    if (placing) {
      if (!g) return;
      const T = types[placing];
      const u = { id: `placed${units.length}`, type: placing, owner: ME,
                  x: snap(g[0], T.foot[0]), z: snap(g[1], T.foot[1]), agl: 0, placed: true };
      units.push(u);
      if (!e.shiftKey) placing = null;     // shift keeps the build command, as in BAR
      return;
    }
    const id = g ? pick(g) : null;
    const u = id && units.find((x) => x.id === id);
    if (!u || !isAlly(u)) { if (!e.shiftKey) selected.clear(); return; }
    if (e.shiftKey) { if (selected.has(id)) selected.delete(id); else selected.add(id); }
    else { selected.clear(); selected.add(id); }
  }
  // a box drag: every own unit whose centre projects inside
  function onBox(x0, y0, x1, y1, add, proj) {
    if (!add) selected.clear();
    const lx = Math.min(x0, x1), hx = Math.max(x0, x1), ly = Math.min(y0, y1), hy = Math.max(y0, y1);
    for (const u of units) {
      if (!isAlly(u)) continue;
      const [sx, sy] = proj([u.x, unitAlt(u), u.z]);
      if (sx >= lx && sx <= hx && sy >= ly && sy <= hy) selected.add(u.id);
    }
  }
  function onKey(e) {
    if (e.type === "keydown" && e.key === "Shift") { shift = true; return true; }
    if (e.type === "keyup" && e.key === "Shift") { shift = false; return true; }
    if (e.type !== "keydown") return false;
    if (e.key === "Escape" && placing) { placing = null; return true; }
    if (e.ctrlKey && (e.key === "a" || e.key === "A")) {
      for (const u of units) if (isAlly(u)) selected.add(u.id);
      e.preventDefault();
      return true;
    }
    if (e.altKey && (e.key === "." || e.key === "," || e.code === "Period" || e.code === "Comma")) {
      cycle(e.code === "Period" || e.key === "." ? 1 : -1);
      e.preventDefault();
      return true;
    }
    return false;
  }
  // alt+. / alt+, (BAR cycleUnitDisplay): one selected unit; its type's
  // per-weapon on/off as a bitmap, +1 or -1 modulo 2^n, kept per ally/enemy
  function cycle(dir) {
    if (selected.size !== 1) { message = "Please select only one unit to change display setting!"; return; }
    const u = units.find((x) => x.id === [...selected][0]);
    const rings = attackRings(types[u.type]);
    if (!rings.length) { message = "Unit has no weapon range!"; return; }
    const side = isAlly(u) ? "ally" : "enemy";
    const t = settings.toggles[u.type] || (settings.toggles[u.type] = {});
    let cur = t[side] && t[side].length === rings.length ? t[side] : rings.map(() => true);
    let bits = cur.reduce((a, on, i) => a | (on ? 1 << i : 0), 0);
    const m = 1 << rings.length;
    bits = ((bits + dir) % m + m) % m;
    t[side] = rings.map((_, i) => !!(bits & (1 << i)));
    message = `Changed range display of ${types[u.type].name} to config ${bits}: ` +
              rings.map((r, i) => `${r.label}=${t[side][i] ? "on" : "off"}`).join(", ");
    save();
  }

  // ---- the panel
  const armedStructures = () => Object.entries(types)
    .filter(([, T]) => !T.mobile && T.side === "ARM" && (defenseRings(T).length || sensorKinds(T).length))
    .sort((a, b) => a[1].name.localeCompare(b[1].name));
  function rows() {
    const b = (k, on, label) => `<button data-rr="${k}" class="${on ? "on" : ""}">${label}</button>`;
    const cls = (side) => DEFENSE_CLASSES.map((c) => b(`${side}:${c}`, settings[side][c], c)).join("");
    const sel = [...selected];
    const selInfo = sel.length === 1
      ? (() => {
          const u = units.find((x) => x.id === sel[0]);
          const T = types[u.type];
          return `${T.name}: ` + attackRings(T).map((r) =>
            `${r.label} ${r.cls}${r.sub ? "/" + r.sub : ""} ${r.range}` +
            (r.ballistic ? ` (v ${(r.v * 30).toFixed(0)}, flat reach ${Math.round(r.v * r.v / G)})` : ""))
            .join(" · ");
        })()
      : `${sel.length} selected`;
    return [
      `<div class="row modes"><span>range rings</span>${b("on", settings.on, "on")}` +
      `${b("shiftOnly", settings.shiftOnly, "shift only")}${b("cursorRange", settings.cursorRange, "cursor range")}` +
      `<span>enemy defenses</span>${cls("enemy")}<span>ally defenses</span>${cls("ally")}` +
      `${b("sensor", settings.sensor, "sensor rings")}` +
      `<span>lab</span>${b("reach", settings.reach, "reach cells")}</div>`,
      `<div class="row modes"><span>place</span>` +
      armedStructures().map(([k, T]) => `<button data-rrplace="${k}" class="${placing === k ? "on" : ""}">${T.name}</button>`).join("") +
      `</div>`,
      `<div class="row"><span>selection</span><b>${selInfo}</b>` +
      (hover ? `<span>hover</span><b>${types[units.find((x) => x.id === hover).type].name}</b>` : "") +
      `<span>keys</span><b>click/drag select · shift adds · ctrl+a all · alt+. alt+, cycle · ` +
      `right-drag pans · esc cancels placing</b>` +
      (message ? `<span>note</span><b>${message}</b>` : "") + `</div>`,
    ];
  }
  function onPanel(target) {
    const t = target.closest("button[data-rr]");
    if (t) {
      const k = t.dataset.rr;
      if (k.includes(":")) { const [side, c] = k.split(":"); settings[side][c] = !settings[side][c]; }
      else settings[k] = !settings[k];
      save();
      return true;
    }
    const p = target.closest("button[data-rrplace]");
    if (p) { placing = placing === p.dataset.rrplace ? null : p.dataset.rrplace; return true; }
    return false;
  }

  return { draw, onMove, onClick, onBox, onKey, onPanel, rows,
           get placing() { return placing; }, get shiftHeld() { return shift; } };
}

window.TRanges = { create, solverAccepts, attackRings, defenseRings };
})();
