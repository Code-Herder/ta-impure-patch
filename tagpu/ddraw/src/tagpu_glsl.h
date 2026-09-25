#ifndef TAGPU_GLSL_H
#define TAGPU_GLSL_H
/* GLSL snippets shared by the native unit shader and the effects shader —
   one copy of the scene-scaffold occlusion rule: a stamped feature row
   nearer than this fragment's depth key hides it. Both fragment shaders carry
   `in float vEnc` (the vertex's depth key) and set the uniforms below. */
#define TAGPU_GLSL_SCAF_UNIFORMS \
    "uniform sampler2D uScaf;\n"      /* R8 scaffold, viewport-sized      */ \
    "uniform int uScafOn;\n" \
    "uniform vec4 uScafP;\n"          /* vpL, vpT, vw, vh (frame px)      */ \
    "uniform float uSS;\n"            /* supersample factor (1 or 2)      */ \
    "uniform float uZoomF;\n"         /* view zoom + centre, to undo      */ \
    "uniform vec2 uZoomCF;\n"
/* VS maps game py 0 -> NDC -1 -> world-target row 0, so gl_FragCoord.xy/uSS IS
   the game-frame pixel; scaffold texture row 0 = viewport top (top-down). */
#define TAGPU_GLSL_SCAF_TEST \
    "  if (uScafOn == 1) {\n" \
    "    vec2 sfc = gl_FragCoord.xy / uSS;\n" \
    "    sfc = (sfc - uZoomCF) / uZoomF + uZoomCF;\n" \
    "    vec2 suv = vec2((sfc.x - uScafP.x) / uScafP.z,\n" \
    "                    (sfc.y - uScafP.y) / uScafP.w);\n" \
    "    if (suv.x >= 0.0 && suv.x < 1.0 && suv.y >= 0.0 && suv.y < 1.0) {\n" \
    "      float s = texture(uScaf, suv).r * 255.0;\n" \
    "      if (s > vEnc + 0.5) discard;\n" \
    "    }\n" \
    "  }\n"

/* ---- fog of war: the engine's own screen fog grid ----------------------
   One copy of the rule for all four native passes (terrain-depth.md 5).
   The engine does NOT test the LOS/MAPPED source maps per pixel; it builds a
   view-anchored grid of 32-px cells (0x4843C0, behind *(main+0x1421F)) whose
   two bytes are 4-bit CORNER masks — b0 = corners that are unexplored, b1 =
   corners that are explored but out of LOS — and the overlay (0x4848E0)
   paints b0==0xF solid black, b1==0xF a full shade remap, and every other
   value one of 14 GAF edge sprites. The grid lattice is offset half a cell
   from the map cells: a corner sits at a map cell's centre, which is why
   sampling the source maps per fragment lands ~16 px off and misses the
   feathered edge entirely.

   Bilinear coverage over the four corner bits, thresholded at 0.5, is those
   14 shapes: 0xF -> everywhere, 0x3 (both top corners) -> exactly the top
   half, a lone corner -> its quadrant. We get a clean edge where the engine
   dithers one, which is the same class of approximation as the LHT flash and
   the feature shadows.

   uFog bit0 = fog on, bit1 = this draw HIDES in grey rather than darkening:
   the engine never draws units or effects outside LOS, while terrain,
   features and wreckage stay visible, in grey (TAGPU_GLSL_FOG_GREY_RGB).

   THE CLAMP STOPS ONE CELL SHORT OF THE GRID, and that is a bound, not a
   margin. A builder fills entry gx from map cells col0+gx and col0+gx+1, so
   the LAST column of any grid — the engine's and ours alike — never has its
   RIGHT corners written, and the last row never has its bottom ones (it is
   why fogw_window asks for two spare columns and the engine's own border
   completion works on cols-2). Clamping to `uFogDim - 0.001` would put every
   sample past the grid at f.x = 1 in that column, i.e. on the corners nobody
   wrote: coverage 0, which reads as NO FOG. That is the worst way to fail — a
   zoomed-out frame the grid does not span comes out with the outer ring in
   full daylight rather than merely smeared. `uFogDim - 1.0` lands such a
   sample at f = 0 in the last entry instead, on the corners the builder did
   write, so the region past the grid REPLICATES its edge — the same thing
   fogw_edge_fill does off the map, and what the engine's own single off-map
   row already amounts to. It decides a pixel only on a BARE frame (no wide
   grid in the packet, counted by the native pass): on every other frame the
   fog bound (tagpu_zoom.c) keeps every drawn fragment inside `[0, dim - 1]`
   of the grid it samples — the engine's is taken only when the 1x rect about
   the drawn eye lies in its fully written cells, and the wide one spans the
   gathers' whole slab, a TAGPU_GATHER_MARGIN past the view on each side, for
   any eye the frame is drawn from. */
#define TAGPU_GLSL_FOG_UNIFORMS \
    "uniform sampler2D uFogGrid;\n"   /* RG8 corner masks, r = b0, g = b1  */ \
    "uniform vec2 uFogOrg;\n"         /* world x,z of grid cell (0,0)      */ \
    "uniform vec2 uFogDim;\n"         /* cols, rows                        */ \
    "uniform int uFog;\n"
#define TAGPU_GLSL_FOG_FN \
    "float taFogCov(int m, vec2 f){\n" \
    "  float tl = float( m       & 1), tr = float((m >> 1) & 1);\n" \
    "  float bl = float((m >> 2) & 1), br = float((m >> 3) & 1);\n" \
    "  return mix(mix(tl, tr, f.x), mix(bl, br, f.x), f.y);\n" \
    "}\n" \
    "vec2 taFog(vec2 w){\n" \
    "  vec2 g = clamp((w - uFogOrg) * (1.0/32.0), vec2(0.0),\n" \
    "                 max(uFogDim - 1.0, vec2(0.0)));\n" \
    "  vec2 c = floor(g);\n" \
    "  vec2 e = texelFetch(uFogGrid, ivec2(c), 0).rg * 255.0 + 0.5;\n" \
    "  return vec2(taFogCov(int(e.x) & 15, g - c), taFogCov(int(e.y) & 15, g - c));\n" \
    "}\n"
/* early, before any colour work: what the engine would not have drawn at all */
#define TAGPU_GLSL_FOG_DISCARD \
    "  vec2 taFogC = vec2(0.0);\n" \
    "  if ((uFog & 1) == 1) {\n" \
    "    taFogC = taFog(vWorld);\n" \
    "    if (taFogC.x >= 0.5) discard;\n" \
    "    if (taFogC.y >= 0.5 && (uFog & 2) == 2) discard;\n" \
    "  }\n"
/* terrain edition. Terrain is the frame's bottom layer, so where the
   engine's overlay paints an unexplored cell SOLID BLACK it must paint black
   too — discarding would punch a hole through to the engine's frame, which no
   longer holds terrain (it holds tagpu_terrown.c's key fill). The engine's
   black is DrawBar with GUI colour 0, i.e. palette index 0, so take it from
   the live palette rather than assuming vec3(0). */
#define TAGPU_GLSL_FOG_TERRAIN \
    "  vec2 taFogC = vec2(0.0);\n" \
    "  if ((uFog & 1) == 1) {\n" \
    "    taFogC = taFog(vWorld);\n" \
    "    if (taFogC.x >= 0.5) {\n" \
    "      frag = vec4(texelFetch(uPal, ivec2(0, 0), 0).rgb, 1.0); return;\n" \
    "    }\n" \
    "  }\n"
/* THE GREY BAND, over what stays visible in it (renderers.md 2.6): the
   colour is replaced by its own R+G+B mean, which is what the engine's grey
   table computes before it quantises to the palette (0x4BAD30: avg RGB/3, then
   nearest) -- in full colour, so the mean is kept and never snapped back to a
   palette entry. The engine remaps each pixel's INDEX through that table
   (*(TAProgram+0xCC); 0x4BFE10 for a full cell, 0x4B86E0 through an edge
   sprite); every world pass here takes the RGB rule instead, in both presets.
   Takes the name of a vec3 variable. Applied after lighting and the face
   shade, so shadows and relief survive as darker grey. */
#define TAGPU_GLSL_FOG_GREY_RGB(V) \
    "  if (taFogC.y >= 0.5) " V " = vec3(dot(" V ", vec3(1.0/3.0)));\n"

/* ---- past the map's edge: the mirror's tone ----------------------------
   The lab's EDGE_TONE (tascene-view.html, `edge=mirror`) at the knobs the
   owner looked at: `edgedim` 0.5, `edgegrey` 0.75, `edgefade` 1536,
   `edgesteps` 0 and `edgedither` 0, so the fade is smooth and nothing
   dithers. One copy for the two passes that draw the mirror -- terrain and
   features -- so the tile under a mirrored tree and the tree itself cannot
   drift apart.

   `w` is the point the fragment DRAWS, on the tile grid (world px, the space
   the terrain's cells are laid out in), and uMapPx is the map's extent on that
   grid: its 32-px cell count times 32. `o` is how far past the map the point
   is on each axis, so the fade runs from the nearest map edge and rounds the
   corners, exactly as the lab measures it. The colour is mixed toward its RGB
   mean -- the fog of war's grey rule (TAGPU_GLSL_FOG_GREY_RGB), in full colour
   -- then dimmed, then faded linearly to black. Unlit: the art is painted lit
   from the north-west, and its reflection is lit from wherever the mirror
   sent that light. */
#define TAGPU_GLSL_EDGE_UNIFORMS \
    "uniform vec2 uMapPx;\n"          /* the map on the tile grid, px      */
#define TAGPU_GLSL_EDGE_FN \
    "vec3 taEdge(vec3 c, vec2 w){\n" \
    "  vec2 o = max(max(-w, w - uMapPx), vec2(0.0));\n" \
    "  float t = clamp(length(o) / 1536.0, 0.0, 1.0);\n" \
    "  c = mix(c, vec3(dot(c, vec3(1.0/3.0))), 0.75);\n" \
    "  return c * 0.5 * (1.0 - t);\n" \
    "}\n"

/* ---- Classic++ lighting: the lab's one rule ---------------------------
   tascene-view.html LAB_LIGHT, renderers.md 1, tascene-design.md "Level
   ground takes exactly 1.0": an ambient-floored lambert divided by what LEVEL
   ground receives, so level is exactly 1.0 and the sun only modulates by the
   tilt from level -- the art is already lit (artlight) and must not be lit
   twice. Evaluated per fragment, because that is where a local light will
   join it. The shadow half of the lab's rule (shadowAt) is here, text for
   text but for one thing: the receiver-plane derivatives arrive as
   arguments, taken by the caller at the top of its main() before any
   discard -- the derivative of a varying is only defined while every fragment
   of the quad is still running. A
   face's normal is flat, so dFdx(p) is exactly 0.5 * mat3(M) * dFdx(W), and
   the arithmetic below is the lab's with the derivative supplied.
   One tap, in both copies: the receiver's
   own texel opens the blocker search -- the eight ring taps sit 12-28
   texels out and missed the commander's head and gun (30 texels across)
   on every frame, so they cast nothing; a caster on the receiver's own ray
   is the one blocker that must not go unfound.
   The map is drawn by tagpu_vk_shadow.c; its two samplers (uShadowCmp,
   uShadowRaw) and the uniforms' values reach each consumer through its
   hand-over (TAGPU_TERRHAND in tagpu_terr.h). Nothing produces them today:
   `shadowOn` is published as 0, so taShadowAt returns 1.0 at its first line
   (tagpu_vk_shadow.h, TAGPU_SHADOWHAND).

   uSun is the unit vector TOWARD the light in map space (x east, y up,
   z south); uAmb 1.0 is "no sun" -- the rule is then exactly 1.0 with no
   branch; uNorm = 1/level, level = uAmb + (1-uAmb)*max(uSun.y, 0), both
   from tagpu_classicpp.c. */
/* uLambert is the `light=` half of the preset (tagpu_classicpp_lit): Classic
   is `light=0`, Classic++ `light=1`, and every world pass runs this rule in
   both. uLambert 0 does not skip taLambert -- it hands it the LEVEL normal
   instead, so the slope shading goes while the shadow term, which lives
   inside taLambert, stays. Level ground is exactly 1.0 there by construction:
   uNorm is 1/level and level is that same lambert of the up normal, so the
   quotient is x/x. */
#define TAGPU_GLSL_LIGHT_UNIFORMS \
    "uniform int uLambert;\n" \
    "uniform vec3 uSun;\n" \
    "uniform float uAmb;\n" \
    "uniform float uNorm;\n"
#define TAGPU_GLSL_SHADOW_UNIFORMS \
    "uniform int uShadowOn;\n" \
    "uniform vec3 uShadowSun;\n"    /* toward the light the SHADOWS fall from */ \
    "uniform mat4 uShadowMat;\n"    /* world -> light clip, orthographic       */ \
    "uniform sampler2DShadow uShadowCmp;\n"  /* the map, compare + bilinear    */ \
    "uniform sampler2D uShadowRaw;\n"        /* the same map, raw depths       */ \
    "uniform vec3 uShScale;\n"      /* world per texel, world per depth unit, 1/res */ \
    "uniform float uPenumbra;\n"    /* penumbra width per world unit of blocker distance */ \
    "uniform float uShade;\n"       /* fraction of the direct light a shadow removes */
#define TAGPU_GLSL_LIGHT_FN \
    "const vec2 taPoisson[16] = vec2[16](\n" \
    "  vec2(-0.94201624,-0.39906216), vec2( 0.94558609,-0.76890725),\n" \
    "  vec2(-0.09418410,-0.92938870), vec2( 0.34495938, 0.29387760),\n" \
    "  vec2(-0.91588581, 0.45771432), vec2(-0.81544232,-0.87912464),\n" \
    "  vec2(-0.38277543, 0.27676845), vec2( 0.97484398, 0.75648379),\n" \
    "  vec2( 0.44323325,-0.97511554), vec2( 0.53742981,-0.47373420),\n" \
    "  vec2(-0.26496911,-0.41893023), vec2( 0.79197514, 0.19090188),\n" \
    "  vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),\n" \
    "  vec2( 0.19984126, 0.78641367), vec2( 0.14383161,-0.14100790));\n" \
    "float taShadowAt(vec3 W, vec3 n, vec3 dWdx, vec3 dWdy){\n" \
    "  if (uShadowOn == 0) return 1.0;\n" \
    "  float nl = dot(n, uShadowSun);\n" \
    "  vec3 p = (uShadowMat * vec4(W + n * uShScale.x * 1.5, 1.0)).xyz * 0.5 + 0.5;\n" \
    "  vec3 dpdx = mat3(uShadowMat) * dWdx * 0.5, dpdy = mat3(uShadowMat) * dWdy * 0.5;\n" \
    "  float det = dpdx.x * dpdy.y - dpdx.y * dpdy.x;\n" \
    "  vec2 dzduv = abs(det) > 1e-14\n" \
    "    ? vec2(dpdy.y * dpdx.z - dpdx.y * dpdy.z, dpdx.x * dpdy.z - dpdy.x * dpdx.z) / det\n" \
    "    : vec2(0.0);\n" \
    "  dzduv = clamp(dzduv, vec2(-4.0), vec2(4.0));\n" \
    "  if (nl <= 0.0) return 1.0;\n" \
    "  if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 || p.z > 1.0) return 1.0;\n" \
    "  float z = p.z - (1.0 + 2.0 * (1.0 - nl)) * uShScale.x / uShScale.y;\n" \
    "  float search = 24.0 / uShScale.x * uShScale.z;\n" \
    "  float sum = 0.0; int nb = 0;\n" \
    "  { float d = texture(uShadowRaw, p.xy).r; if (d < z) { sum += d; nb++; } }\n" \
    "  for (int i = 0; i < 16; i++) {\n" \
    "    vec2 o = taPoisson[i] * search;\n" \
    "    float d = texture(uShadowRaw, p.xy + o).r;\n" \
    "    if (d < z + dot(o, dzduv)) { sum += d; nb++; }\n" \
    "  }\n" \
    "  if (nb == 0) return 1.0;\n" \
    "  float dist = (z - sum / float(nb)) * uShScale.y;\n" \
    "  float r = max(uPenumbra * dist, 0.5 * uShScale.x) / uShScale.x * uShScale.z;\n" \
    "  float lit = 0.0;\n" \
    "  for (int i = 0; i < 16; i++) {\n" \
    "    vec2 o = taPoisson[i] * r;\n" \
    "    lit += texture(uShadowCmp, vec3(p.xy + o, z + dot(o, dzduv)));\n" \
    "  }\n" \
    "  return 1.0 - uShade * (1.0 - lit / 16.0);\n" \
    "}\n" \
    "float taLambert(vec3 n, vec3 W, vec3 dWdx, vec3 dWdy){\n" \
    "  n = normalize(n);\n" \
    "  return (uAmb + (1.0 - uAmb) * max(dot(n, uSun), 0.0) * taShadowAt(W, n, dWdx, dWdy)) * uNorm;\n" \
    "}\n"

/* ---- the sub-pixel edge nudge -----------------------------------------
   Quads are emitted on exact integer game-pixel boundaries, so at some zooms
   a quad's far edge lands EXACTLY on a fragment centre. The rasteriser gives
   that fragment to one of the two quads sharing the edge -- measured on this
   stack, to the upper/left one -- and its interpolated u (or v) is then
   exactly u1, which VK_FILTER_NEAREST resolves to the first texel of the NEXT
   atlas cell. On terrain that is the unrelated tile 64 cells later (the blue
   hairlines along tile edges at zoom 0.25); on a GAF sprite it is the packer's
   gutter (the black hairline down the right of every tree).

   Clamping the texel back into the cell is NOT the fix: the fragment then
   repeats the cell's last row, which is out of phase with the row cadence the
   rest of the minified tile is sampled on, and TA's tile art is dithered, so
   an out-of-phase row still reads as a coloured line. Measured -- it moved the
   seam metric from 1.92x to 1.86x, i.e. not at all.

   So move the geometry instead, by a fraction of a pixel, in the direction
   that puts a coincident fragment centre INSIDE the following quad. That
   restores the ordinary case exactly (the shared fragment samples the next
   cell's first texel, which is what it is standing on) and leaves the
   sampling cadence uniform. The value is in game-screen pixels, applied
   AFTER the zoom scale so it is the same sub-pixel distance at every zoom:
   1/32 px is ~300x the float noise in a coordinate that size and 1/8 of a
   texel at the 0.25 zoom floor, so it cannot be lost.
   IT SHIFTS THE PICTURE 1/32 PX AT EVERY ZOOM: a fragment reads the texel
   under its point plus NUDGE/zoom world px. On the dyadic zooms from 0.5x to
   8x no fragment centre lies that near before a texel boundary (with an even
   viewport width, whose zoom centre is a whole px), so none reads another
   texel; at the floor every tie takes the texel after it; and at the wheel's
   other resting levels every fragment within NUDGE/zoom world px before a
   boundary reads the next texel. The feature pass takes the same nudge
   (tagpu_feat.c), so a sprite and the ground under it shift together. */
#define TAGPU_EDGE_NUDGE "0.03125"
/* the same distance for a C caller that moves its quads itself (tagpu_feat.c) */
#define TAGPU_EDGE_NUDGE_PX 0.03125f

/* ---- lines: DrawLine's walk on the line grid, decided per pixel ----------
   Every line the lane draws -- order lines, build sites, selection rects,
   lasers and lightning, the nanoframe wire -- is two triangles over a BAND
   around the segment, and the fragment stage keeps a fragment only when its
   LINE-GRID pixel is one the rule of tagpu_line.h lights: `0x4CC7AB`'s walk
   between the line's two ends, thickened to w pixels across its minor axis.
   Everything that decides a pixel is integer arithmetic on that grid: the
   ends arrive as whole line-grid pixels (tagpu_line.h says how they are
   chosen), the fragment's own pixel comes from its integer target pixel, and
   the walk is closed-form. No floating-point decision near a pixel edge is
   left for two GPUs to take differently.

   `grid` is (line-grid w | w << 16, line-grid h, target w, target h), set per
   draw by the pass from the extent it records into. The line grid is the
   world target at ss x the game frame, so on the offscreen target the map is
   the identity; on a frame the offscreen target refused, the world is drawn
   into the swapchain image at whatever scale that is, and `taLinePx` takes
   the line-grid pixel whose span holds the target pixel's centre --
   floor((t + 0.5) * lw / tw), written ((2t + 1) * lw) / (2 * tw) so no float
   is involved. `taLineEnd` takes an end's pixel back from the centre a record
   carries (tagpu_line.h, THE RECORD CARRIES CENTRES).

   `taOnLine`: `0x4CC7AB`'s walk, as exe-reverse-engineering.md records it,
   and the thickening. It walks from the smaller-x end; x-major when
   |dy| <= dx (so 45 degrees is x-major); the pixel `i` major steps along
   sits at minor offset (2 * minor * i + major) / (2 * major), which is its
   error term `2 * minor - major` stepping on `>= 0` unrolled; both ends
   inclusive. A fragment is kept when it lies in the walk's major range and
   within [r - w/2, r - w/2 + w - 1] across the minor axis of the walk's pixel
   r there. At w = 1 that is the walk itself. The products are UNSIGNED so
   they cannot overflow for ends inside tagpu_line.h's TAGPU_LINE_MAXC box.
   DrawLine's clip (`0x4BEA20` to the viewport, then `0x4CC650` to the
   surface) is not part of it: it moves the ends before the walk, on the CPU
   where a record is built (tagpu_line.h `tagpu_line_clip`), for the callers
   whose engine lines are DrawLine's -- the markers (tagpu_mark.c `put_line`)
   and the effects (tagpu_fx.c `emit_line`). The nanoframe wire
   (tagpu_native.c FS) reaches this walk unclipped: the engine draws it with
   the polygon edge walk `0x4C0820`, which moves no end, and its ends are held
   to the MAXC box instead. */
#define TAGPU_GLSL_LINE_FN \
    "ivec2 taLinePx(vec2 fc, ivec4 grid) {\n" \
    "  ivec2 t = ivec2(fc);\n" \
    "  ivec2 lg = ivec2(grid.x & 65535, grid.y);\n" \
    "  return ((2 * t + 1) * lg) / (2 * grid.zw);\n" \
    "}\n" \
    "ivec2 taLineEnd(vec2 c, ivec4 grid) {\n" \
    "  return ivec2(floor(c * float(grid.x >> 16)));\n" \
    "}\n" \
    "bool taOnLine(ivec2 g, ivec2 a, ivec2 b, int w) {\n" \
    "  if (a.x > b.x) { ivec2 s0 = a; a = b; b = s0; }\n" \
    "  int dx = b.x - a.x, dy = b.y - a.y;\n" \
    "  int ady = abs(dy), sg = dy < 0 ? -1 : 1;\n" \
    "  int lo = w / 2;\n" \
    "  if (ady <= dx) {\n" \
    "    int i = g.x - a.x;\n" \
    "    if (i < 0 || i > dx) return false;\n" \
    "    int r = dx == 0 ? a.y\n" \
    "          : a.y + sg * int((2u * uint(ady) * uint(i) + uint(dx)) / (2u * uint(dx)));\n" \
    "    int k = g.y - r + lo;\n" \
    "    return k >= 0 && k < w;\n" \
    "  }\n" \
    "  int j = (g.y - a.y) * sg;\n" \
    "  if (j < 0 || j > ady) return false;\n" \
    "  int c = a.x + int((2u * uint(dx) * uint(j) + uint(ady)) / (2u * uint(ady)));\n" \
    "  int k = g.x - c + lo;\n" \
    "  return k >= 0 && k < w;\n" \
    "}\n" \
    "bool taLineKeeps(vec2 fc, vec4 ends, ivec4 grid) {\n" \
    "  return taOnLine(taLinePx(fc, grid), taLineEnd(ends.xy, grid),\n" \
    "                  taLineEnd(ends.zw, grid), grid.x >> 16);\n" \
    "}\n"

/* THE BAND a line is drawn over, in game units after the zoom: the segment
   between the two ends' line-grid pixel CENTRES (the values a record
   carries), widened by taBandR game pixels on each side and carried taBandR
   past each end. `c` is the corner, 0..5 over two triangles (A-, B-, A+) and
   (A+, B-, B+), where A and B are the ends and the sign the side; `t` comes
   back as the corner's position ALONG the segment, 0 at A's centre and 1 at
   B's, so a vertex stage can extend a per-end attribute across the band
   linearly (the wire's depth does).

   WHY 2 IS ENOUGH, which tools/line-band-check.py checks by brute force. In
   line-grid pixels: every pixel the walk lights has its centre within half a
   pixel of the segment between the two end centres, measured along the minor
   axis, and the thickening moves a copy at most floor(w/2) further along it;
   a square pixel reaches 0.71 beyond its centre. So every lit pixel lies
   within 1.21 + floor(w/2) line-grid pixels of the segment across it and
   within 0.71 * (1 + floor(w/2)) past either end -- at most 1.21 GAME pixels
   across and 0.71 past an end for every w = ss, since a line-grid pixel is
   1/ss of one. A band of 2 game pixels holds them with 0.79 to spare, and
   float error in placing the corners cannot reach that. Being wide costs fill
   only: the fragment test decides the pixels. */
#define TAGPU_GLSL_BAND_FN \
    "const float taBandR = 2.0;\n" \
    "vec2 taBand(vec2 ca, vec2 cb, int c, out float t) {\n" \
    "  vec2 d = cb - ca;\n" \
    "  float len = length(d);\n" \
    "  vec2 u = len > 0.0 ? d / len : vec2(1.0, 0.0);\n" \
    "  vec2 n = vec2(-u.y, u.x);\n" \
    "  bool atB = c == 1 || c == 4 || c == 5;\n" \
    "  float side = (c == 2 || c == 3 || c == 5) ? 1.0 : -1.0;\n" \
    "  if (len > 0.0) t = atB ? 1.0 + taBandR / len : -taBandR / len;\n" \
    "  else t = atB ? 1.0 : 0.0;\n" \
    "  return (atB ? cb + taBandR * u : ca - taBandR * u) + (taBandR * side) * n;\n" \
    "}\n"
#endif
