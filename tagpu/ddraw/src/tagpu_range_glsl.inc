/* tagpu_range_glsl.inc — the range rings' shaders, included by tagpu_range.c
   at file scope. tools/spirv-gen.py reads them out of tagpu_range.c's
   preprocessed translation unit, so they stay `static const char* NAME =`.

   THE REACH, per direction, on the GPU. tools/tascene-ranges.js is the
   reference and these functions are its `ground`, `surface`,
   `solverAccepts`, `reachable` and `reachAlong`, line for line:

   - the ground is GetPosHeight 0x485070's: bilinear over the 16-px height
     grid, the nearest cell at the edge (the height texture holds the same
     byte per cell the engine's grid does: tagpu_terr.c build_height);
   - a ring vertex sits on the water surface, and so does a target there;
   - range is horizontal (can-aim 0x49AA80, can-engage 0x49ABB0), and a
     ballistic weapon must also pass the solver 0x49A890: with
     D' = v^4 + 2 g dy v^2 - g^2 d^2 (dy = shooter minus target altitude)
     it refuses when D' < 0; the two roots s = v^2 cos^2(theta) give the
     angles acos(sqrt(s)/v) (0x4E67F0 is the CRT acos), a root <= 0 is pi/2,
     and an angle is taken when minbarrel < angle <= pi/4 (DISASSEMBLED
     2026-11-09, research/notes/weapon-range-rings.md);
   - the ring sits at the FIRST refused point along each direction: an 8-px
     march, then eight halvings between the last kept and the first refused.

   THE PROJECTION is the marker pass's: x - eyeX + vpL, z - alt/2 - eyeY + vpT,
   then the zoom about its centre. */

#define TAGPU_RANGE_GLSL_COMMON \
    "layout(location=0) in vec4 aC;\n"   /* x, z, shooter altitude, range  */ \
    "layout(location=1) in vec4 aB;\n"   /* v, minbarrel, ballistic, scale */ \
    "layout(location=2) in vec4 aCol;\n" \
    "layout(location=3) in vec4 aW;\n"   /* half+rim, segments, half, 0    */ \
    "uniform vec2 uGame;\n" \
    "uniform float uZoom;\n" \
    "uniform vec2 uZoomC;\n" \
    "uniform vec2 uEye;\n" \
    "uniform vec2 uHDim;\n" \
    "uniform vec2 uMapExt;\n" \
    "uniform float uSea;\n" \
    "uniform float uGrav;\n" \
    "uniform float uPxGame;\n" \
    "uniform sampler2D uHeight;\n" \
    "out float vEdge; out vec4 vCol; out float vHalf;\n" \
    "float taGround(vec2 p) {\n" \
    "  vec2 f = p / 16.0;\n" \
    "  vec2 c = floor(f);\n" \
    "  ivec2 ci = ivec2(c);\n" \
    "  ivec2 dim = ivec2(uHDim);\n" \
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
    "float taSurface(vec2 p) { return max(taGround(p), uSea); }\n" \
    "bool taSolver(float d, float dy, float v, float mb) {\n" \
    "  float v2 = v * v;\n" \
    "  float dp = v2 * v2 + 2.0 * uGrav * dy * v2 - uGrav * uGrav * d * d;\n" \
    "  if (dp < 0.0) return false;\n" \
    "  float d2 = d * d, den = 2.0 * (d2 + dy * dy);\n" \
    "  if (den <= 0.0) return false;\n" \
    "  float sq = d2 * sqrt(dp), base = d2 * (v2 + uGrav * dy);\n" \
    "  for (int k = 0; k < 2; k++) {\n" \
    "    float s = (k == 0 ? base + sq : base - sq) / den;\n" \
    "    float a = s <= 0.0 ? 1.5707963 : acos(min(1.0, sqrt(s) / v));\n" \
    "    if (a > mb && a <= 0.7853982 + 1e-6) return true;\n" \
    "  }\n" \
    "  return false;\n" \
    "}\n" \
    "bool taReachable(vec2 o, float y0, float d, vec2 dir) {\n" \
    "  if (d > aC.w) return false;\n" \
    "  return taSolver(d, y0 - taSurface(o + dir * d), aB.x, aB.y);\n" \
    "}\n" \
    "float taReach(vec2 o, float y0, vec2 dir) {\n" \
    "  float R = aC.w, lo = 0.0, hi = -1.0;\n" \
    "  for (int i = 1; i <= 4096; i++) {\n" \
    "    float d = 8.0 * float(i);\n" \
    "    if (d > R) break;\n" \
    "    if (!taReachable(o, y0, d, dir)) { hi = d; break; }\n" \
    "    lo = d;\n" \
    "  }\n" \
    "  if (hi < 0.0) {\n" \
    "    if (lo < R && !taReachable(o, y0, R, dir)) hi = R;\n" \
    "    else return R;\n" \
    "  }\n" \
    "  for (int k = 0; k < 8; k++) {\n" \
    "    float mid = (lo + hi) * 0.5;\n" \
    "    if (mid > 0.0 && taReachable(o, y0, mid, dir)) lo = mid; else hi = mid;\n" \
    "  }\n" \
    "  return lo;\n" \
    "}\n" \
    "vec3 taRingPoint(int k, int n) {\n" \
    "  float a = float(k) / float(n) * 6.2831853;\n" \
    "  vec2 dir = vec2(sin(a), cos(a));\n" \
    "  vec2 o = aC.xy;\n" \
    "  float r = aC.w * aB.w;\n" \
    "  if (aB.z > 0.5) r = taReach(o, aC.z, dir);\n" \
    "  vec2 p = o + dir * r;\n" \
    "  return vec3(p.x, taSurface(p), p.y);\n" \
    "}\n" \
    "vec2 taProject(vec3 w) {\n" \
    "  vec2 s = vec2(w.x - uEye.x, w.z - w.y * 0.5 - uEye.y);\n" \
    "  return (s - uZoomC) * uZoom + uZoomC;\n" \
    "}\n" \
    "float taOob(vec3 w) {\n" \
    "  float inb = min(min(w.x, w.z), min(uMapExt.x - w.x, uMapExt.y - w.z));\n" \
    "  return 1.0 - clamp(inb * -0.02, 0.0, 1.0);\n" \
    "}\n" \
    "vec4 taClip(vec2 p) {\n" \
    "  return vec4(p.x / uGame.x * 2.0 - 1.0, p.y / uGame.y * 2.0 - 1.0, 0.0, 1.0);\n" \
    "}\n"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
/* THE DISCS: a fan per ring as a triangle list, three vertices a segment,
   the centre at the shooter. They write the class's stencil bit and the fill
   colour the instance carries (alpha 0 for the defense rings, which BAR
   draws no fill for). */
static const char* DVS =
    "#version 330 core\n"
    TAGPU_RANGE_GLSL_COMMON
    "void main(){\n"
    "  int n = int(aW.y);\n"
    "  int seg = gl_VertexID / 3, c = gl_VertexID - seg * 3;\n"
    "  vec2 p;\n"
    "  if (c == 0) p = taProject(vec3(aC.x, aC.z, aC.y));\n"
    "  else p = taProject(taRingPoint(c == 1 ? seg : (seg + 1 == n ? 0 : seg + 1), n));\n"
    "  gl_Position = taClip(p);\n"
    "  vEdge = 0.0; vHalf = 1e6; vCol = aCol;\n"
    "}\n";

/* THE LINES: a band of two triangles a segment, six vertices, each segment
   run on by half a width at both ends so the joints close (the reference's
   `polyline`). `vEdge` is -1..1 across the band for the one-pixel rim. */
static const char* LVS =
    "#version 330 core\n"
    TAGPU_RANGE_GLSL_COMMON
    "void main(){\n"
    "  int n = int(aW.y);\n"
    "  int seg = gl_VertexID / 6, c = gl_VertexID - seg * 6;\n"
    "  vec3 w0 = taRingPoint(seg, n);\n"
    "  vec3 w1 = taRingPoint(seg + 1 == n ? 0 : seg + 1, n);\n"
    "  vec2 p0 = taProject(w0), p1 = taProject(w1);\n"
    "  vec2 d = p1 - p0;\n"
    "  float len = length(d);\n"
    "  vec2 u = len > 1e-6 ? d / len : vec2(1.0, 0.0);\n"
    "  vec2 nn = vec2(-u.y, u.x) * aW.x;\n"
    "  vec2 ex = u * aW.z;\n"
    "  bool atB = c == 2 || c == 3 || c == 5;\n"
    "  float side = (c == 0 || c == 2 || c == 3) ? 1.0 : -1.0;\n"
    "  vec2 p = (atB ? p1 + ex : p0 - ex) + side * nn;\n"
    "  gl_Position = taClip(p);\n"
    "  vEdge = side;\n"
    "  vHalf = aW.x / uPxGame;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * taOob(atB ? w1 : w0));\n"
    "}\n";

/* One fragment stage for both: premultiplied, a one-target-pixel rim. */
static const char* FS =
    "#version 330 core\n"
    "in float vEdge; in vec4 vCol; in float vHalf;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float px = abs(vEdge) * vHalf;\n"
    "  float a = clamp(vHalf - px, 0.0, 1.0);\n"
    "  float al = clamp(vCol.a, 0.0, 1.0) * a;\n"
    "  frag = vec4(min(vCol.rgb, vec3(1.0)) * al, al);\n"
    "}\n";
#pragma GCC diagnostic pop
