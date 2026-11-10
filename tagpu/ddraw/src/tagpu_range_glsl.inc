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
   a sensor ring -- a flat coverage circle -- plus 1 when it covers water only (sonar: the radar
   pass contacts a target at or below sea level, 0x4678E6) and 2 when its outline is dashed (the
   jammers). A sensor disc is cut to the scroll extent, as the map's own edge cuts the coverage
   the player can use; a water ring's fill and outline show over water only. */

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
    "layout(std430) readonly buffer Reach {\n" \
    "  vec2 p[];\n" \
    "} rch;\n" \
    "out float vEdge; out vec4 vCol; out float vHalf;\n" \
    "out vec2 vSea; out float vWater; out float vSeaLvl;\n" \
    "int taKind() { return int(aB.z + 0.5); }\n" \
    "bool taSensor() { return taKind() >= 2; }\n" \
    "bool taWaterOnly() { return taSensor() && ((taKind() - 2) & 1) != 0; }\n" \
    "bool taDashed() { return taSensor() && ((taKind() - 2) & 2) != 0; }\n" \
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
   `polyline`). `vEdge` is -1..1 across the band for the one-pixel rim. A
   dashed ring skips every other run of 12 segments (the reference's 6 of
   512); a water ring's point on land has no alpha, as its fill has none
   there. */
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
    "  vec3 wE = atB ? w1 : w0;\n"
    "  float a = aCol.a * taOob(wE);\n"
    "  if (taWaterOnly() && wE.y > uSea + 0.01) a = 0.0;\n"
    "  if (taDashed() && ((seg / 12) & 1) == 1) a = 0.0;\n"
    "  vCol = vec4(aCol.rgb, a);\n"
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
    "  if (vWater > 0.5 && taGround(vSea) > vSeaLvl) al = 0.0;\n"
    "  frag = vec4(min(vCol.rgb, vec3(1.0)) * al, al);\n"
    "}\n";
#pragma GCC diagnostic pop
