#ifndef TAGPU_GLSL_H
#define TAGPU_GLSL_H
/* GLSL snippets shared by the native unit shader and the effects shader —
   one copy of the scene-scaffold occlusion rule (G12a): a stamped feature row
   nearer than this fragment's depth key hides it. Both fragment shaders carry
   `in float vEnc` (the vertex's depth key) and set the uniforms below. */
#define TAGPU_GLSL_SCAF_UNIFORMS \
    "uniform sampler2D uScaf;\n"      /* R8 scaffold, viewport-sized      */ \
    "uniform int uScafOn;\n" \
    "uniform vec4 uScafP;\n"          /* vpL, vpT, vw, vh (frame px)      */ \
    "uniform float uSS;\n"            /* supersample factor (1 or 2)      */ \
    "uniform float uZoomF;\n"         /* view zoom + centre, to undo      */ \
    "uniform vec2 uZoomCF;\n"
/* VS maps game py 0 -> NDC -1 -> FBO window y 0, so gl_FragCoord.xy/uSS IS
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
   features and wreckage stay visible and are merely shade-remapped. */
#define TAGPU_GLSL_FOG_UNIFORMS \
    "uniform sampler2D uFogGrid;\n"   /* RG8 corner masks, r = b0, g = b1  */ \
    "uniform sampler2D uFogLUT;\n"    /* 256x1 palette remap for the grey  */ \
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
    "  vec2 g = clamp((w - uFogOrg) * (1.0/32.0), vec2(0.0), uFogDim - 0.001);\n" \
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
/* terrain edition (G13b). Terrain is the frame's bottom layer, so where the
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
/* the overlay's darken over what stays visible in grey: the engine remaps
   every pixel's palette INDEX through *(TAProgram+0xCC) (0x4BFE10 for a full
   cell, 0x4B86E0 through an edge sprite), so we do the same on the index
   before the palette fetch rather than scaling the resolved colour. Takes the
   name of the int index variable. */
#define TAGPU_GLSL_FOG_SHADE(I) \
    "  if (taFogC.y >= 0.5) " I " = int(texelFetch(uFogLUT, ivec2(" I ", 0), 0).r * 255.0 + 0.5);\n"
/* The Classic++ edition of the same band (renderers.md 2.6): a restored texel
   has no palette index to remap, so the LIT colour is replaced by its own
   R+G+B mean -- which is exactly what the engine's grey table computes
   before it quantises to the palette (0x4BAD30: avg RGB/3, then nearest).
   Takes the name of a vec3 variable. Applied after lighting, so shadows
   and relief survive as darker grey. */
#define TAGPU_GLSL_FOG_GREY_RGB(V) \
    "  if (taFogC.y >= 0.5) " V " = vec3(dot(" V ", vec3(1.0/3.0)));\n"

/* ---- Classic++ lighting: the lab's one rule ---------------------------
   tascene-view.html LAB_LIGHT, renderers.md 1, tascene-design.md "Level
   ground takes exactly 1.0": an ambient-floored lambert divided by what LEVEL
   ground receives, so level is exactly 1.0 and the sun only modulates by the
   tilt from level -- the art is already lit (artlight) and must not be lit
   twice. Evaluated per fragment, because that is where a local light will
   join it. The shadow half of the lab's rule (shadowAt) is here since G14h,
   text for text but for one thing: the receiver-plane derivatives arrive as
   arguments, taken by the caller at the top of its main() before any
   discard -- the derivative of a varying is only defined while every fragment
   of the quad is still running (G14g's lesson on the mipped sample). A
   face's normal is flat, so dFdx(p) is exactly 0.5 * mat3(M) * dFdx(W), and
   the arithmetic below is the lab's with the derivative supplied.
   One tap the lab did not have until G14h, in both copies: the receiver's
   own texel opens the blocker search -- the eight ring taps sit 12-28
   texels out and missed the commander's head and gun (30 texels across)
   on every frame, so they cast nothing; a caster on the receiver's own ray
   is the one blocker that must not go unfound.
   The map, its samplers on units 12 and 13, and the uniforms' values are
   tagpu_shadow.c's (tagpu_shadow_locate / _apply).

   uSun is the unit vector TOWARD the light in map space (x east, y up,
   z south); uAmb 1.0 is "no sun" -- the rule is then exactly 1.0 with no
   branch; uNorm = 1/level, level = uAmb + (1-uAmb)*max(uSun.y, 0), both
   from tagpu_classicpp.c. uLit = 1 selects the Classic++ colour path in the
   shader that carries it (light, then the RGB grey band); 0 is Classic,
   byte for byte. */
#define TAGPU_GLSL_LIGHT_UNIFORMS \
    "uniform int uLit;\n" \
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
   exactly u1, which GL_NEAREST resolves to the first texel of the NEXT atlas
   cell. On terrain that is the unrelated tile 64 cells later (the blue
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
   texel at the 0.25 zoom floor, so it can neither be lost nor change which
   texel any other fragment reads. */
#define TAGPU_EDGE_NUDGE "0.03125"
#endif
