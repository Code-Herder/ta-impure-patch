/* tagpu_range_glsl.inc — the range rings' draw shaders, included by tagpu_range.c at file scope.
   tools/spirv-gen.py reads them out of tagpu_range.c's preprocessed translation unit, so they stay
   `static const char* NAME =`.

   THE RING'S SHAPE IS NOT COMPUTED HERE. The reach compute pass (tagpu_range_comp.h) has written,
   per ring and direction k, the radius and the surface altitude there into `rch.p[base + k]`;
   `base` is the instance's aW.w, which tagpu_vk_range.c sets to the block it holds for that ring.
   These stages only place the points and project them.

   THE PROJECTION is the marker pass's: x - eyeX + vpL, z - alt/2 - eyeY + vpT, then the zoom
   about its centre.

   A RING'S KIND is aB.z: 0 a flat ring, 1 a ballistic one (the solver bounds its reach), 2 and up
   a sensor disc -- a jammer's flat coverage circle -- plus 1 when it covers water only (sonar
   jamming: the radar pass contacts a target at or below sea level, 0x4678E6). A sensor disc is
   cut to the scroll extent, as the map's own edge cuts the coverage the player can use; a water
   disc's fill shows over water only.

   THE SENSOR PATHS (SVS/SFS) are not rings: tagpu_range.c has laid each outline on the ground as
   points (`Path`, two vec4 a point: x, z, altitude, along; direction x, y, shown, new), and these
   stages draw a dashed band along one path an instance (tagpu_range.h, TAGPU_RGINST). */

#define TAGPU_RANGE_GLSL_COMMON \
    "layout(location=0) in vec4 aC;\n"   /* x, z, shooter altitude, range  */ \
    "layout(location=1) in vec4 aB;\n"   /* v, minbarrel, ballistic, scale */ \
    "layout(location=2) in vec4 aCol;\n" \
    "layout(location=3) in vec4 aW;\n"   /* half+rim, segments, half, base */ \
    "uniform vec2 uGame;\n" \
    "uniform float uZoom;\n" \
    "uniform vec2 uZoomC;\n" \
    "uniform vec2 uEye;\n" \
    "uniform vec2 uMapExt;\n" \
    "uniform float uPxGame;\n" \
    "uniform float uSea;\n" \
    "uniform vec4 uLodOp[3];\n"         /* the dash levels' opacity, finest first */ \
    "uniform vec2 uLodLv;\n"            /* the finest level drawn, the zoom's     */ \
    "layout(std430) readonly buffer Reach {\n" \
    "  vec2 p[];\n" \
    "} rch;\n" \
    "int taKind() { return int(aB.z + 0.5); }\n" \
    "bool taSensor() { return taKind() >= 2; }\n" \
    "bool taWaterOnly() { return taSensor() && ((taKind() - 2) & 1) != 0; }\n" \
    "vec3 taRingPoint(int k, int n) {\n" \
    "  float a = float(k) / float(n) * 6.2831853;\n" \
    "  vec2 e = rch.p[int(aW.w) + k];\n" \
    "  vec2 p = aC.xy + vec2(sin(a), cos(a)) * e.x;\n" \
    "  return vec3(p.x, e.y, p.y);\n" \
    "}\n" \
    "vec2 taProject(vec3 w) {\n" \
    "  vec2 s = vec2(w.x - uEye.x, w.z - w.y * 0.5 - uEye.y);\n" \
    "  return (s - uZoomC) * uZoom + uZoomC;\n" \
    "}\n" \
    /* the world x, z a screen point shows when the ground there is at sea level: */ \
    /* affine in the screen point, so a varying of it is exact at every fragment  */ \
    "vec2 taAtSea(vec2 p) {\n" \
    "  vec2 s = (p - uZoomC) / uZoom + uZoomC;\n" \
    "  return vec2(s.x + uEye.x, s.y + uEye.y + uSea * 0.5);\n" \
    "}\n" \
    "float taOob(vec3 w) {\n" \
    "  float inb = min(min(w.x, w.z), min(uMapExt.x - w.x, uMapExt.y - w.z));\n" \
    "  return 1.0 - clamp(inb * -0.02, 0.0, 1.0);\n" \
    "}\n" \
    "vec4 taClip(vec2 p) {\n" \
    "  return vec4(p.x / uGame.x * 2.0 - 1.0, p.y / uGame.y * 2.0 - 1.0, 0.0, 1.0);\n" \
    "}\n"

/* what the rings' two vertex stages hand the one fragment stage */
#define TAGPU_RANGE_GLSL_RINGOUT \
    "out float vEdge; out vec4 vCol; out float vHalf;\n" \
    "out vec2 vSea; out float vWater; out float vSeaLvl;\n"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
/* THE DISCS: a fan per ring as a triangle list, three vertices a segment,
   the centre at the shooter. They write the class's stencil bit and the fill
   colour the instance carries (alpha 0 for the defense rings, which BAR
   draws no fill for). A sensor disc's rim is held to the scroll extent; the
   fan stays whole because its centre, a unit or a site, is inside it. */
static const char* DVS =
    "#version 330 core\n"
    TAGPU_RANGE_GLSL_COMMON
    TAGPU_RANGE_GLSL_RINGOUT
    "void main(){\n"
    "  int n = int(aW.y);\n"
    "  int seg = gl_VertexID / 3, c = gl_VertexID - seg * 3;\n"
    "  vec3 w = vec3(aC.x, aC.z, aC.y);\n"
    "  if (c != 0) {\n"
    "    w = taRingPoint(c == 1 ? seg : (seg + 1 == n ? 0 : seg + 1), n);\n"
    "    if (taSensor()) w.xz = clamp(w.xz, vec2(0.0), uMapExt);\n"
    "  }\n"
    "  vec2 p = taProject(w);\n"
    "  gl_Position = taClip(p);\n"
    "  vEdge = 0.0; vHalf = 1e6; vCol = aCol;\n"
    "  vSea = taAtSea(p); vWater = taWaterOnly() ? 1.0 : 0.0; vSeaLvl = uSea;\n"
    "}\n";

/* THE LINES: a band of two triangles a segment, six vertices, each segment
   run on by half a width at both ends so the joints close (the reference's
   `polyline`). `vEdge` is -1..1 across the band for the one-pixel rim. */
static const char* LVS =
    "#version 330 core\n"
    TAGPU_RANGE_GLSL_COMMON
    TAGPU_RANGE_GLSL_RINGOUT
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
    "  vec3 wE = atB ? w1 : w0;\n"
    "  vCol = vec4(aCol.rgb, aCol.a * taOob(wE));\n"
    "  vSea = vec2(0.0); vWater = 0.0; vSeaLvl = 0.0;\n"
    "}\n";

/* One fragment stage for both: premultiplied, a one-target-pixel rim.
   A WATER DISC'S LAND IS TRANSPARENT, NOT DISCARDED: the fragment still
   writes the stencil, so the union the outline is cut by is the whole disc
   while the fill shows over water only. The water test is the reach pass's
   ground, bilinear over the 16-px grid (tagpu_range_comp.h), at the point the
   pixel shows if that point is water -- exact there, and a land pixel that
   maps onto water lies within (altitude - sea) / 2 px south of it. */
static const char* FS =
    "#version 330 core\n"
    "in float vEdge; in vec4 vCol; in float vHalf;\n"
    "in vec2 vSea; in float vWater; in float vSeaLvl;\n"
    "uniform sampler2D uHeight;\n"
    "out vec4 frag;\n"
    "float taGround(vec2 p) {\n"
    "  ivec2 dim = textureSize(uHeight, 0);\n"
    "  vec2 f = p / 16.0;\n"
    "  vec2 c = floor(f);\n"
    "  ivec2 ci = ivec2(c);\n"
    "  if (ci.x < 0 || ci.y < 0 || ci.x + 1 >= dim.x || ci.y + 1 >= dim.y) {\n"
    "    ivec2 cc = clamp(ci, ivec2(0), dim - ivec2(1));\n"
    "    return texelFetch(uHeight, cc, 0).r * 255.0;\n"
    "  }\n"
    "  vec2 t = f - c;\n"
    "  float a = texelFetch(uHeight, ci, 0).r;\n"
    "  float b = texelFetch(uHeight, ci + ivec2(1, 0), 0).r;\n"
    "  float d = texelFetch(uHeight, ci + ivec2(0, 1), 0).r;\n"
    "  float e = texelFetch(uHeight, ci + ivec2(1, 1), 0).r;\n"
    "  return mix(mix(a, b, t.x), mix(d, e, t.x), t.y) * 255.0;\n"
    "}\n"
    "void main(){\n"
    "  float px = abs(vEdge) * vHalf;\n"
    "  float a = clamp(vHalf - px, 0.0, 1.0);\n"
    "  float al = clamp(vCol.a, 0.0, 1.0) * a;\n"
    "  vec3 rgb = min(vCol.rgb, vec3(1.0));\n"
    "  if (vWater > 0.5 && taGround(vSea) > vSeaLvl) al = 0.0;\n"
    "  frag = vec4(rgb * al, al);\n"
    "}\n";

/* THE SENSOR PATHS: two triangles a segment, six vertices, between path
   points `first + seg` and the next. A corner stands at its point, moved
   along the point's direction (`side` 1 out of the circles, -1 into them) by
   -0.5 or the band's width + 0.5 target px: neighbouring segments share
   their edge, so no pixel is drawn twice and `along` runs on unbroken. A
   segment with neither end shown has no area; one a building being placed
   adds (its start point's `new`) takes the instance's aW.y as its alpha. */
static const char* SVS =
    "#version 330 core\n"
    TAGPU_RANGE_GLSL_COMMON
    "layout(std430) readonly buffer Path {\n"
    "  vec4 q[];\n"
    "} pth;\n"
    "out float vAcross; out float vArc; out float vVis;\n"
    "flat out vec4 vCol; flat out vec4 vLine; flat out vec4 vLv; flat out float vZoomT;\n"
    "flat out vec4 vOpA; flat out vec4 vOpB; flat out float vOpC;\n"
    "void main(){\n"
    "  int seg = gl_VertexID / 6, c = gl_VertexID - seg * 6;\n"
    "  bool atB = c == 2 || c == 3 || c == 5;\n"
    "  bool hi = c == 1 || c == 2 || c == 3;\n"
    "  int i = int(aC.x + 0.5) + seg;\n"
    "  int at = atB ? i + 1 : i;\n"
    "  vec4 p = pth.q[2 * at], d = pth.q[2 * at + 1];\n"
    "  float wT = aC.w / uPxGame;\n"
    "  float acr = hi ? wT + 0.5 : -0.5;\n"
    "  vec2 s = taProject(vec3(p.x, p.z, p.y)) + aC.z * d.xy * (acr * uPxGame);\n"
    "  gl_Position = taClip(s);\n"
    "  if (pth.q[2 * i + 1].z < 0.5 && pth.q[2 * i + 3].z < 0.5) gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);\n"
    "  vAcross = acr; vArc = p.w; vVis = d.z;\n"
    "  vCol = pth.q[2 * i + 1].w > 0.5 ? vec4(aCol.rgb, aW.y) : aCol;\n"
    "  vLine = vec4(wT, aB.x, aB.y, aB.z);\n"
    "  vLv = vec4(uLodLv.x, uLodLv.y, aW.x / uPxGame, aB.w);\n"
    "  vZoomT = uZoom / uPxGame;\n"
    "  vOpA = uLodOp[0]; vOpB = uLodOp[1]; vOpC = uLodOp[2].x;\n"
    "}\n";

/* The band: a one-target-pixel rim on both edges, and the dash this
   fragment falls in. The lattice (tagpu_range.c `put_spath`): level
   j a dash every s0 x 2^j along the path; the zoom's level and coarser are
   drawn, a finer one at its fading opacity; a dash belongs to the coarsest
   level its index is a multiple of. A path whose coarsest level is finer
   than the zoom's draws that level whole. Lengths are target px: `along` and
   s0 are zoom-1 game px, so x zoom / (game px a target px). */
static const char* SFS =
    "#version 330 core\n"
    "in float vAcross; in float vArc; in float vVis;\n"
    "flat in vec4 vCol; flat in vec4 vLine; flat in vec4 vLv; flat in float vZoomT;\n"
    "flat in vec4 vOpA; flat in vec4 vOpB; flat in float vOpC;\n"
    "out vec4 frag;\n"
    "float taOp(int i) { return i < 4 ? vOpA[i] : i < 8 ? vOpB[i - 4] : vOpC; }\n"
    "float taDashes() {\n"
    "  int top = int(floor(vLine.z + 0.5)), fine = int(floor(vLv.x + 0.5));\n"
    "  int level = int(floor(vLv.y + 0.5));\n"
    "  int f = top < level ? top : fine;\n"
    "  float s = vLine.y * exp2(float(f));\n"
    "  float u = (vArc + vLv.w) / s, n = floor(u + 0.5);\n"
    "  float d = abs(u - n) * s * vZoomT;\n"
    "  float cover = clamp(vLv.z * 0.5 - d + 0.5, 0.0, 1.0);\n"
    "  if (cover <= 0.0) return 0.0;\n"
    "  if (top < level) return cover;\n"
    "  float per = exp2(float(top - f));\n"
    "  int m = int(n - per * floor(n / per) + 0.5), lv = f;\n"
    "  if (m == 0) lv = top;\n"
    "  else for (int k = 0; k < 16 && (m & 1) == 0; k++) { m >>= 1; lv++; }\n"
    "  return cover * taOp(clamp(lv - fine, 0, 8));\n"
    "}\n"
    "void main(){\n"
    "  float a = clamp(vAcross + 0.5, 0.0, 1.0) * clamp(vLine.x + 0.5 - vAcross, 0.0, 1.0);\n"
    "  a *= clamp(vCol.a, 0.0, 1.0) * vVis;\n"
    "  a *= taDashes();\n"
    "  if (a <= 0.0) discard;\n"
    "  frag = vec4(min(vCol.rgb, vec3(1.0)) * a, a);\n"
    "}\n";
#pragma GCC diagnostic pop
