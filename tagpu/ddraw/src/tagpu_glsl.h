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
