/* tagpu_range_glsl.inc — the range rings' draw shaders, included by tagpu_range.c at file scope.
   tools/spirv-gen.py reads them out of tagpu_range.c's preprocessed translation unit, so they stay
   `static const char* NAME =`.

   THE RING'S SHAPE IS NOT COMPUTED HERE. The reach compute pass (tagpu_range_comp.h) has written,
   per ring and direction k, the radius and the surface altitude there into `rch.p[base + k]`;
   `base` is the instance's aW.w, which tagpu_vk_range.c sets to the block it holds for that ring.
   These stages only place the points and project them.

   THE PROJECTION is the marker pass's: x - eyeX + vpL, z - alt/2 - eyeY + vpT, then the zoom
   about its centre. */

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
    "layout(std430) readonly buffer Reach {\n" \
    "  vec2 p[];\n" \
    "} rch;\n" \
    "out float vEdge; out vec4 vCol; out float vHalf;\n" \
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
