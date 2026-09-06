#ifndef TAGPU_RESTORE_GLSL_H
#define TAGPU_RESTORE_GLSL_H
/* The GLSL restorer's shaders -- the unditherer's residual CNN as fragment
   passes (research/notes/renderers.md 4c). This header is the ONE copy of the
   shader text: tagpu_restoreglsl.c compiles it under "#version 330 core", and
   tools/tascene extracts the same macros into the pack for the browser lab,
   which compiles them under "#version 300 es". Neither side may carry its own
   edition. Two prefix lines are supplied by the compiler side, not here:

     #define NK <n>       output channel-tiles per conv draw (1, 2, 4 or 8)
     #define WMAX <m>     mat4s in the bound weight range = NK * the model's
                          largest k-block (148 for 12x64, 56 for 6x24)

   LAYOUT. Activations are four channels per texel, one 2D-array layer per
   channel tile (tile j = channels 4j..4j+3): 16 layers for 64 channels, 6 for
   24, 1 for the RGB input and 1 for the RGB residual. A batch is a grid of
   SLOTS, uSlot texels apart, one frame per slot; every slot has a valid RECT
   (x0, y0, w, h, slot-local) and a tap that lands outside it reads 0 -- at
   EVERY layer, which is the zero padding the model was trained with. A frame
   that tiles (tagpu_rglsl_tileable) is wrap-padded by the model's
   depth inside its rect by the fill pass and centre-cropped by the out pass,
   the unditherer's own rule (classical.is_tileable's 12-level test, infer.py's
   wrap-pad by the depth). Frames of different sizes share a batch: the slot
   pitch is the batch's largest padded edge and each slot's rect is its own.
   The weights are unditherer/weights.py's
   layout: per output tile k one std140 block of mat4 -- bias in column 0 of
   mat4 0, then mat4 1 + t*Jin + j for tap t (offset (t%3-1, t/3-1)) and input
   tile j -- and a conv draw binds NK consecutive k-blocks as one uniform range.

   THE COLOUR KEY (GAF frames). The reference inpaints keyed texels with
   OpenCV's TELEA before the network and restores the key's alpha after
   (unditherer/restore.py load_image / save_image); a shader cannot reproduce
   TELEA, so the FILL pass stands in with the mean palette colour of the
   opaque texels in the nearest ring (Chebyshev) within uKeyR of the keyed
   texel -- uKeyR is the model's depth, its receptive radius, because a keyed
   texel farther than that from every opaque one influences no opaque output.
   The OUT pass writes (0, 0, 0, 0) at a keyed texel, which is what save_image
   writes (alpha 0, RGB zeroed). uKey per slot = the key index, or -1 for a
   frame with no key (terrain). The correctness bar for keyed frames is the
   opaque texels farther than uKeyR from any keyed texel (renderers.md 4c);
   the band nearer than that differs from the reference by construction and
   is reported separately.

   PASSES per batch: FILL (palette lookup into layer 0 of the ping-pong array),
   depth x CONV (each in ceil(tiles/NK) draws, MRT over the destination's
   layers), then OUT (residual, 8-bit rounding, straight into the destination
   atlas in its bordered cell layout). The rounding writes (k + 0.25)/255 so
   that a driver which truncates the float-to-unorm conversion and one which
   rounds both store exactly k. */

/* a full-viewport triangle, no vertex buffer: FILL and CONV */
#define TAGPU_RESTORE_FS_VS \
    "void main(){\n" \
    "  vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n" \
    "  gl_Position = vec4(p, 0.0, 1.0);\n" \
    "}\n"

/* FILL: the model's input. uSrc per slot = (ax, ay, sw, sh): the frame's
   first texel in the R8 atlas and its size; uRect per slot = the valid rect;
   uKey per slot = (key index or -1, 0, 0, 0). pad = (rect - size)/2 is the
   wrap radius (0 for a zero-padded frame), and the source texel wraps by
   floor division so a pad wider than the frame is still right. A keyed texel
   takes the stand-in described above, searched inside the frame (wrapped
   when the frame wraps, clipped when it does not). Alpha 0: the 4th input
   channel has zero weights anyway. */
#define TAGPU_RESTORE_FILL_FS \
    "uniform sampler2D uAtlas;\n" \
    "uniform sampler2D uPal;\n" \
    "uniform sampler2D uRect;\n" \
    "uniform sampler2D uSrc;\n" \
    "uniform sampler2D uKey;\n" \
    "uniform int uSlot;\n" \
    "uniform int uKeyR;\n" \
    "out vec4 frag;\n" \
    "int idxAt(ivec2 o, ivec2 t) { return int(texelFetch(uAtlas, o + t, 0).r * 255.0 + 0.5); }\n" \
    "void tap(ivec2 t, ivec2 o, ivec2 sz, bool wrap, int key, inout vec3 acc, inout int n) {\n" \
    "  if (wrap) t -= sz * ivec2(floor(vec2(t) / vec2(sz)));\n" \
    "  else if (t.x < 0 || t.y < 0 || t.x >= sz.x || t.y >= sz.y) return;\n" \
    "  int q = idxAt(o, t);\n" \
    "  if (q == key) return;\n" \
    "  acc += texelFetch(uPal, ivec2(q, 0), 0).rgb; n++;\n" \
    "}\n" \
    "void main(){\n" \
    "  ivec2 f = ivec2(gl_FragCoord.xy);\n" \
    "  ivec2 slot = f / uSlot;\n" \
    "  ivec2 sl = f - slot * uSlot;\n" \
    "  vec4 rect = texelFetch(uRect, slot, 0);\n" \
    "  vec4 src = texelFetch(uSrc, slot, 0);\n" \
    "  int key = int(texelFetch(uKey, slot, 0).x);\n" \
    "  ivec2 sz = ivec2(src.zw);\n" \
    "  if (sz.x <= 0 || sz.y <= 0) { frag = vec4(0.0); return; }\n" \
    "  ivec2 pad = (ivec2(rect.zw) - sz) / 2;\n" \
    "  ivec2 s = sl - ivec2(rect.xy) - pad;\n" \
    "  s -= sz * ivec2(floor(vec2(s) / vec2(sz)));\n" \
    "  ivec2 o = ivec2(src.xy);\n" \
    "  int pi = idxAt(o, s);\n" \
    "  if (key >= 0 && pi == key) {\n" \
    "    bool wrap = pad.x > 0 || pad.y > 0;\n" \
    "    vec3 acc = vec3(0.0); int n = 0;\n" \
    "    for (int r = 1; r <= uKeyR; r++) {\n" \
    "      for (int i = -r; i < r; i++) {\n" \
    "        tap(s + ivec2(i, -r), o, sz, wrap, key, acc, n);\n" \
    "        tap(s + ivec2(r, i), o, sz, wrap, key, acc, n);\n" \
    "        tap(s + ivec2(-i, r), o, sz, wrap, key, acc, n);\n" \
    "        tap(s + ivec2(-r, -i), o, sz, wrap, key, acc, n);\n" \
    "      }\n" \
    "      if (n > 0) break;\n" \
    "    }\n" \
    "    frag = vec4(n > 0 ? acc / float(n) : vec3(0.0), 0.0);\n" \
    "    return;\n" \
    "  }\n" \
    "  frag = vec4(texelFetch(uPal, ivec2(pi, 0), 0).rgb, 0.0);\n" \
    "}\n"

/* CONV: one 3x3 layer, NK output tiles per fragment (MRT). uJin input tiles
   are read from uAct's layers 0..uJin-1; the weight range holds NK k-blocks
   of uKStride mat4s each. The rect test is per tap, so a frame never reads
   its neighbour's slot and never reads its own gutter. */
#define TAGPU_RESTORE_CONV_FS \
    "uniform highp sampler2DArray uAct;\n" \
    "uniform sampler2D uRect;\n" \
    "uniform int uJin;\n" \
    "uniform int uKStride;\n" \
    "uniform int uSlot;\n" \
    "uniform int uRelu;\n" \
    "layout(std140) uniform WBlock { mat4 w[WMAX]; };\n" \
    "layout(location = 0) out vec4 o0;\n" \
    "#if NK > 1\n" \
    "layout(location = 1) out vec4 o1;\n" \
    "#endif\n" \
    "#if NK > 2\n" \
    "layout(location = 2) out vec4 o2;\n" \
    "layout(location = 3) out vec4 o3;\n" \
    "#endif\n" \
    "#if NK > 4\n" \
    "layout(location = 4) out vec4 o4;\n" \
    "layout(location = 5) out vec4 o5;\n" \
    "layout(location = 6) out vec4 o6;\n" \
    "layout(location = 7) out vec4 o7;\n" \
    "#endif\n" \
    "void main(){\n" \
    "  ivec2 f = ivec2(gl_FragCoord.xy);\n" \
    "  ivec2 slot = f / uSlot;\n" \
    "  ivec2 sl = f - slot * uSlot;\n" \
    "  vec4 rect = texelFetch(uRect, slot, 0);\n" \
    "  ivec2 r0 = ivec2(rect.xy), r1 = r0 + ivec2(rect.zw);\n" \
    "  vec4 acc[NK];\n" \
    "  for (int n = 0; n < NK; n++) acc[n] = w[n * uKStride][0];\n" \
    "  int m = 1;\n" \
    "  for (int t = 0; t < 9; t++) {\n" \
    "    ivec2 p = sl + ivec2(t % 3 - 1, t / 3 - 1);\n" \
    "    if (p.x >= r0.x && p.y >= r0.y && p.x < r1.x && p.y < r1.y) {\n" \
    "      ivec2 q = slot * uSlot + p;\n" \
    "      for (int j = 0; j < uJin; j++) {\n" \
    "        vec4 a = texelFetch(uAct, ivec3(q, j), 0);\n" \
    "        for (int n = 0; n < NK; n++) acc[n] += w[n * uKStride + m] * a;\n" \
    "        m++;\n" \
    "      }\n" \
    "    } else m += uJin;\n" \
    "  }\n" \
    "  if (uRelu == 1) for (int n = 0; n < NK; n++) acc[n] = max(acc[n], vec4(0.0));\n" \
    "  o0 = acc[0];\n" \
    "#if NK > 1\n" \
    "  o1 = acc[1];\n" \
    "#endif\n" \
    "#if NK > 2\n" \
    "  o2 = acc[2]; o3 = acc[3];\n" \
    "#endif\n" \
    "#if NK > 4\n" \
    "  o4 = acc[4]; o5 = acc[5]; o6 = acc[6]; o7 = acc[7];\n" \
    "#endif\n" \
    "}\n"

/* OUT: one quad per frame over its destination cell INCLUDING the replicated
   border (the quad is the driver's: dest origin - border .. + size + border).
   aCell = dest origin x, y of the frame's first texel and its slot column,
   row; aSize = the frame's w, h. The fragment clamps into the frame, which is
   what makes the border a copy of the edge, reads the residual at the slot's
   matching texel (pad undoes the wrap padding: a centre crop) and the input
   colour from the atlas at the destination coordinate itself -- the source R8
   atlas and the RGBA destination share one layout. A keyed texel is written
   (0, 0, 0, 0): alpha 0 is the hole, and the atlas's alpha is also how a
   consumer tells a painted cell from one the job has not reached. */
#define TAGPU_RESTORE_OUT_VS \
    "layout(location = 0) in vec2 aPos;\n" \
    "layout(location = 1) in vec4 aCell;\n" \
    "layout(location = 2) in vec2 aSize;\n" \
    "uniform vec2 uDst;\n" \
    "flat out vec4 vCell;\n" \
    "flat out vec2 vSize;\n" \
    "void main(){\n" \
    "  gl_Position = vec4(aPos / uDst * 2.0 - 1.0, 0.0, 1.0);\n" \
    "  vCell = aCell; vSize = aSize;\n" \
    "}\n"
#define TAGPU_RESTORE_OUT_FS \
    "uniform highp sampler2DArray uAct;\n" \
    "uniform sampler2D uAtlas;\n" \
    "uniform sampler2D uPal;\n" \
    "uniform sampler2D uRect;\n" \
    "uniform sampler2D uKey;\n" \
    "uniform int uSlot;\n" \
    "flat in vec4 vCell;\n" \
    "flat in vec2 vSize;\n" \
    "out vec4 frag;\n" \
    "void main(){\n" \
    "  ivec2 f = ivec2(gl_FragCoord.xy);\n" \
    "  ivec2 cell = ivec2(vCell.xy), slot = ivec2(vCell.zw), size = ivec2(vSize);\n" \
    "  ivec2 d = clamp(f - cell, ivec2(0), size - 1);\n" \
    "  vec4 rect = texelFetch(uRect, slot, 0);\n" \
    "  int key = int(texelFetch(uKey, slot, 0).x);\n" \
    "  int pi = int(texelFetch(uAtlas, cell + d, 0).r * 255.0 + 0.5);\n" \
    "  if (key >= 0 && pi == key) { frag = vec4(0.0); return; }\n" \
    "  ivec2 pad = (ivec2(rect.zw) - size) / 2;\n" \
    "  ivec2 q = slot * uSlot + ivec2(rect.xy) + pad + d;\n" \
    "  vec3 net = texelFetch(uAct, ivec3(q, 0), 0).rgb;\n" \
    "  vec3 c = texelFetch(uPal, ivec2(pi, 0), 0).rgb;\n" \
    "  vec3 k = clamp(floor((c - net) * 255.0 + 0.5), 0.0, 255.0);\n" \
    "  frag = vec4((k + 0.25) / 255.0, 1.0);\n" \
    "}\n"
#endif
