#ifndef TAGPU_RANGE_COMP_H
#define TAGPU_RANGE_COMP_H
/* The range rings' reach -- Vulkan 1.0 compute. This header is the ONE copy of the shader text and
   no C file includes it: tools/spirv-gen.py reads the macros below and compiles them into
   inc/spirv/tagpu_range_comp.spv.h, which tagpu_vk_range.c runs. A shader is assembled as
   `#version 450`, TAGPU_RANGE_COMMON_CS, then its own macro.

   ONE WORKGROUP A RING, one invocation a direction (striding by 64 up to the ring's segment
   count). It writes, per direction k, the ring's radius there and the surface altitude at that
   point, `p[base + k] = (r, y)`, which every draw of the ring -- its disc and its two bands --
   then reads instead of walking the terrain again. tagpu_vk_range.c keeps the result for as long
   as the ring's inputs stay the same, so a ring that does not move is walked once.

   THE REACH is the engine's, and tools/tascene-ranges.js is the reference these functions are
   ported from (`ground`, `surface`, `solverAccepts`, `reachable`, `reachAlong`):
   - the ground is GetPosHeight 0x485070's: bilinear over the 16-px height grid, the nearest
     cell at the edge (the image holds the engine's height byte per cell, tagpu_terr.c
     build_height);
   - a target on water stands on the surface;
   - range is horizontal (can-aim 0x49AA80, can-engage 0x49ABB0), and a ballistic weapon must
     also pass the solver 0x49A890: with D' = v^4 + 2 g dy v^2 - g^2 d^2 (dy = shooter minus
     target altitude) it refuses when D' < 0; the two roots s = v^2 cos^2(theta) give the angles
     acos(sqrt(s)/v) (0x4E67F0 is the CRT acos), a root <= 0 is pi/2, and an angle is taken when
     minbarrel < angle <= pi/4 (DISASSEMBLED, research/notes/weapon-range-rings.md);
   - the ring sits at the FIRST refused point along each direction: an 8-px march, then eight
     halvings between the last kept and the first refused.

   A JOB is three vec4: (x, z, shooter altitude, range), (velocity, minbarrel, 1 = ballistic,
   radius scale), (base, segments, 0, 0). THE BOUND: a write lands at base + k with k < segments,
   and tagpu_vk_range.c hands out base and segments from its own block allocator inside the
   buffer it binds; `cap` is that buffer's length in entries and the shader refuses any write at
   or past it, so a job the allocator got wrong cannot write past the buffer. The march is
   bounded by the range (at most 4096 steps of 8 px, the 32767 cap of TAGPU_PK_RING_RMAX). */
#define TAGPU_RANGE_COMMON_CS \
    "layout(push_constant) uniform PC {\n" \
    "  vec2 hdim; float sea; float grav; int jobs; int cap;\n" \
    "} pc;\n" \
    "layout(binding = 0) uniform sampler2D uHeight;\n" \
    "layout(std430, binding = 1) readonly buffer Jobs { vec4 j[]; } jobs;\n" \
    "layout(std430, binding = 2) writeonly buffer Reach { vec2 p[]; } rch;\n" \
    "float taGround(vec2 p) {\n" \
    "  vec2 f = p / 16.0;\n" \
    "  vec2 c = floor(f);\n" \
    "  ivec2 ci = ivec2(c);\n" \
    "  ivec2 dim = ivec2(pc.hdim);\n" \
    "  if (ci.x < 0 || ci.y < 0 || ci.x + 1 >= dim.x || ci.y + 1 >= dim.y) {\n" \
    "    ivec2 cc = clamp(ci, ivec2(0), dim - ivec2(1));\n" \
    "    return texelFetch(uHeight, cc, 0).r * 255.0;\n" \
    "  }\n" \
    "  vec2 t = f - c;\n" \
    "  float a = texelFetch(uHeight, ci, 0).r;\n" \
    "  float b = texelFetch(uHeight, ci + ivec2(1, 0), 0).r;\n" \
    "  float d = texelFetch(uHeight, ci + ivec2(0, 1), 0).r;\n" \
    "  float e = texelFetch(uHeight, ci + ivec2(1, 1), 0).r;\n" \
    "  return mix(mix(a, b, t.x), mix(d, e, t.x), t.y) * 255.0;\n" \
    "}\n" \
    "float taSurface(vec2 p) { return max(taGround(p), pc.sea); }\n" \
    "bool taSolver(float d, float dy, float v, float mb) {\n" \
    "  float g = pc.grav, v2 = v * v;\n" \
    "  float dp = v2 * v2 + 2.0 * g * dy * v2 - g * g * d * d;\n" \
    "  if (dp < 0.0) return false;\n" \
    "  float d2 = d * d, den = 2.0 * (d2 + dy * dy);\n" \
    "  if (den <= 0.0) return false;\n" \
    "  float sq = d2 * sqrt(dp), base = d2 * (v2 + g * dy);\n" \
    "  for (int k = 0; k < 2; k++) {\n" \
    "    float s = (k == 0 ? base + sq : base - sq) / den;\n" \
    "    float a = s <= 0.0 ? 1.5707963 : acos(min(1.0, sqrt(s) / v));\n" \
    "    if (a > mb && a <= 0.7853982 + 1e-6) return true;\n" \
    "  }\n" \
    "  return false;\n" \
    "}\n" \
    "bool taReachable(vec4 c, vec4 b, float d, vec2 dir) {\n" \
    "  if (d > c.w) return false;\n" \
    "  return taSolver(d, c.z - taSurface(c.xy + dir * d), b.x, b.y);\n" \
    "}\n" \
    "float taReach(vec4 c, vec4 b, vec2 dir) {\n" \
    "  float R = c.w, lo = 0.0, hi = -1.0;\n" \
    "  for (int i = 1; i <= 4096; i++) {\n" \
    "    float d = 8.0 * float(i);\n" \
    "    if (d > R) break;\n" \
    "    if (!taReachable(c, b, d, dir)) { hi = d; break; }\n" \
    "    lo = d;\n" \
    "  }\n" \
    "  if (hi < 0.0) {\n" \
    "    if (lo < R && !taReachable(c, b, R, dir)) hi = R;\n" \
    "    else return R;\n" \
    "  }\n" \
    "  for (int k = 0; k < 8; k++) {\n" \
    "    float mid = (lo + hi) * 0.5;\n" \
    "    if (mid > 0.0 && taReachable(c, b, mid, dir)) lo = mid; else hi = mid;\n" \
    "  }\n" \
    "  return lo;\n" \
    "}\n"

/* REACH: job gl_WorkGroupID.x, directions k = invocation, invocation + 64, ... */
#define TAGPU_RANGE_REACH_CS \
    "layout(local_size_x = 64) in;\n" \
    "void main() {\n" \
    "  int job = int(gl_WorkGroupID.x);\n" \
    "  if (job >= pc.jobs) return;\n" \
    "  vec4 c = jobs.j[3 * job], b = jobs.j[3 * job + 1], o = jobs.j[3 * job + 2];\n" \
    "  int base = int(o.x), n = int(o.y);\n" \
    "  for (int k = int(gl_LocalInvocationID.x); k < n; k += 64) {\n" \
    "    if (base + k >= pc.cap) return;\n" \
    "    float a = float(k) / float(n) * 6.2831853;\n" \
    "    vec2 dir = vec2(sin(a), cos(a));\n" \
    "    float r = c.w * b.w;\n" \
    "    if (b.z > 0.5) r = taReach(c, b, dir);\n" \
    "    rch.p[base + k] = vec2(r, taSurface(c.xy + dir * r));\n" \
    "  }\n" \
    "}\n"

#endif
