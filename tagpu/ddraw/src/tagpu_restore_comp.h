#ifndef TAGPU_RESTORE_COMP_H
#define TAGPU_RESTORE_COMP_H
/* The restorer's compute shaders -- the unditherer's residual CNN
   (unditherer/model.py) as Vulkan 1.0 compute, research/notes/compute-restorer.md.
   This header is the ONE copy of the shader text and no C file includes it:
   tools/spirv-gen.py reads the macros below and compiles them into
   inc/spirv/tagpu_restore_comp.spv.h, which tagpu_vk_restore.c runs.

   HOW THE GENERATOR ASSEMBLES A SHADER: `#version 450`, then the variant's
   `#define` lines, then TAGPU_RESTORE_COMMON_CS, then the shader's own macro.
   The text is Vulkan GLSL already, so nothing is translated -- glslang
   preprocesses it. CONV_CS is compiled once per LAYER SHAPE (CIN, COUT, LAST),
   and the shapes are read out of the shipped weight files, so a model with a
   new shape fails the build until its variant is generated.

   THE GRID. A batch is a square of `cols` x `cols` slots, one frame per slot,
   `pitch` apart, where pitch = the core's slot size S + 1. Slot (c, r) is entry
   r * 8 + c of the table (TAGPU_RSLOT, 64 bytes each). The activations are one
   storage buffer per ping-pong side, [gh][gw][channels] fp32, gw a multiple of
   16 and gh of 32 so that every conv variant's tiles cover the grid exactly.

   THE PADDING RULE, which defines the pixels: a tap outside its slot's valid
   rect reads 0 AT EVERY LAYER, the zero padding the model was trained with.
   It holds by construction here. FILL and every conv layer write EVERY texel
   of the grid, and 0 at every texel outside a valid rect. The rect is at most
   S wide and the pitch is S + 1, so a slot always ends in a zero column and
   a zero row; a 3x3 tap reaches one texel, and so it can never land in a
   neighbouring slot's rect. The grid's own edge reads 0 through the staging's
   bounds test.

   THE COLOUR KEY (GAF frames). The reference inpaints keyed texels with
   OpenCV's TELEA before the network and restores the key's alpha after
   (unditherer/restore.py). A shader cannot reproduce TELEA, so FILL stands
   in with the mean palette colour of the opaque texels in the nearest ring
   (Chebyshev) within keyR of the keyed texel. keyR is the model's depth, its
   receptive radius: a keyed texel farther than that from every opaque one
   influences no opaque output. OUT writes (0, 0, 0, 0) at a keyed texel,
   which is what the reference's save writes.

   THE SOURCE IS ONE OF TWO KINDS, and pc.base says which (FILL and OUT alike).
   0: an R8 atlas of palette indices, looked up through uPal, a texel keyed
   where its index is the slot's key -- the UI's. 1: a world pass's BASE atlas,
   RGBA8 already expanded through the palette, a texel keyed where its alpha is
   0. A slot's key of -1 keys nothing on either kind.

   THE ROUNDING. OUT and MIP write (k + 0.25) / 255 into an RGBA8 image, so a
   driver whose float-to-unorm conversion truncates and one that rounds both
   store exactly k (the specification allows either). */

/* Every shader: the push constants and the slot record. One push-constant
   block for all of them, so one pipeline layout range fits every pipeline. */
#define TAGPU_RESTORE_COMMON_CS \
    "layout(push_constant) uniform PC {\n" \
    "  int gw, gh, pitch, cols;\n" \
    "  int y0;\n" \
    "  int wbase, bbase;\n" \
    "  int base;\n" \
    "  int keyR;\n" \
    "  int dstW, dstH;\n" \
    "  int spare;\n" \
    "} pc;\n" \
    "struct Slot { int rw, rh, ax, ay, sw, sh, key, dx, dy, border, padR, padB, r0, r1, r2, r3; };\n"

/* FILL: the model's input, one vec4 per texel (RGB, 0). Inside a slot's rect,
   pad = (rect - size) / 2 is the wrap radius (0 for a frame that does not
   tile), and the source texel wraps by floor division, so a pad wider than
   the frame is still right. A keyed texel takes the stand-in above, searched
   inside the frame (wrapped when the frame wraps, clipped when it does not).
   Outside every rect: 0, which is half of the padding rule. */
#define TAGPU_RESTORE_FILL_CS \
    "layout(local_size_x = 8, local_size_y = 8) in;\n" \
    "layout(std430, binding = 1) writeonly buffer ActOut { vec4 aout[]; };\n" \
    "layout(std430, binding = 3) readonly buffer Tab { Slot slot[]; };\n" \
    "layout(binding = 4) uniform sampler2D uAtlas;\n" \
    "layout(binding = 5) uniform sampler2D uPal;\n" \
    "bool keyOf(vec4 v, int key) {\n" \
    "  if (key < 0) return false;\n" \
    "  return pc.base == 1 ? v.a < 0.5 : int(v.r * 255.0 + 0.5) == key;\n" \
    "}\n" \
    "vec3 colOf(vec4 v) {\n" \
    "  return pc.base == 1 ? v.rgb : texelFetch(uPal, ivec2(int(v.r * 255.0 + 0.5), 0), 0).rgb;\n" \
    "}\n" \
    "void tap(ivec2 t, ivec2 o, ivec2 sz, bool wrap, int key, inout vec3 acc, inout int n) {\n" \
    "  if (wrap) t -= sz * ivec2(floor(vec2(t) / vec2(sz)));\n" \
    "  else if (t.x < 0 || t.y < 0 || t.x >= sz.x || t.y >= sz.y) return;\n" \
    "  vec4 q = texelFetch(uAtlas, o + t, 0);\n" \
    "  if (keyOf(q, key)) return;\n" \
    "  acc += colOf(q); n++;\n" \
    "}\n" \
    "void main() {\n" \
    "  ivec2 f = ivec2(gl_GlobalInvocationID.xy);\n" \
    "  if (f.x >= pc.gw || f.y >= pc.gh) return;\n" \
    "  ivec2 cr = f / pc.pitch;\n" \
    "  ivec2 sl = f - cr * pc.pitch;\n" \
    "  vec4 v = vec4(0.0);\n" \
    "  if (cr.x < pc.cols && cr.y < pc.cols) {\n" \
    "    Slot s = slot[cr.y * 8 + cr.x];\n" \
    "    ivec2 sz = ivec2(s.sw, s.sh);\n" \
    "    if (sl.x < s.rw && sl.y < s.rh && sz.x > 0 && sz.y > 0) {\n" \
    "      ivec2 pad = (ivec2(s.rw, s.rh) - sz) / 2;\n" \
    "      ivec2 t = sl - pad;\n" \
    "      t -= sz * ivec2(floor(vec2(t) / vec2(sz)));\n" \
    "      ivec2 o = ivec2(s.ax, s.ay);\n" \
    "      vec4 pv = texelFetch(uAtlas, o + t, 0);\n" \
    "      if (keyOf(pv, s.key)) {\n" \
    "        bool wrap = pad.x > 0 || pad.y > 0;\n" \
    "        vec3 acc = vec3(0.0); int n = 0;\n" \
    "        for (int r = 1; r <= pc.keyR; r++) {\n" \
    "          for (int i = -r; i < r; i++) {\n" \
    "            tap(t + ivec2(i, -r), o, sz, wrap, s.key, acc, n);\n" \
    "            tap(t + ivec2(r, i), o, sz, wrap, s.key, acc, n);\n" \
    "            tap(t + ivec2(-i, r), o, sz, wrap, s.key, acc, n);\n" \
    "            tap(t + ivec2(-r, -i), o, sz, wrap, s.key, acc, n);\n" \
    "          }\n" \
    "          if (n > 0) break;\n" \
    "        }\n" \
    "        v = vec4(n > 0 ? acc / float(n) : vec3(0.0), 0.0);\n" \
    "      } else {\n" \
    "        v = vec4(colOf(pv), 0.0);\n" \
    "      }\n" \
    "    }\n" \
    "  }\n" \
    "  aout[f.y * pc.gw + f.x] = v;\n" \
    "}\n"

/* CONV: one 3x3 layer, direct. Defines CIN COUT LAST TH (the generator's).
   A workgroup is 16 x TH texels and all COUT channels; each invocation owns 8
   consecutive texels x 8 output channels, so it is 2 x TH x COUT/8
   invocations. The input is staged 4 channels at a time with its one-texel
   halo, and the weights of those 4 channels beside it: at most 12 KB of
   shared memory and 128 invocations, inside Vulkan 1.0's guaranteed minimums
   (16 KB, 128), so this runs on every device that has compute at all.
   The weights are w[wbase + ((tap * CIN + ci) * COUT) / 4 + co4], tap = ky*3+kx,
   and the biases w[bbase + co4] (tagpu_vk_restore.c repacks them out of the
   weight file). LAST writes the network's output, conv + bias with no ReLU,
   as one vec4 per texel; its COUT is 8, the last layer's 4 channels padded
   with zero weights to one invocation's share.
   The dispatch covers rows y0 .. y0 + groups.y * TH, a BAND of the grid, so
   one layer can be split across several dispatches and the core's budget
   stays fine-grained on a slow device. */
#define TAGPU_RESTORE_CONV_CS \
    "layout(local_size_x = 2 * TH * (COUT / 8)) in;\n" \
    "layout(std430, binding = 0) readonly buffer ActIn { float ain[]; };\n" \
    "layout(std430, binding = 1) writeonly buffer ActOut { vec4 aout[]; };\n" \
    "layout(std430, binding = 2) readonly buffer Wt { vec4 w[]; };\n" \
    "layout(std430, binding = 3) readonly buffer Tab { Slot slot[]; };\n" \
    "const uint TW = 16u;\n" \
    "const uint CC = 4u;\n" \
    "const uint HWID = TW + 2u;\n" \
    "const uint HROWS = uint(TH) + 2u;\n" \
    "const uint CG = uint(COUT) / 8u;\n" \
    "const uint NTHR = 2u * uint(TH) * CG;\n" \
    "const uint CO4 = uint(COUT) / 4u;\n" \
    "shared float hs[CC * HROWS * HWID];\n" \
    "shared vec4 ws[9u * CC * CO4];\n" \
    "bool inRect(int x, int y) {\n" \
    "  ivec2 cr = ivec2(x, y) / pc.pitch;\n" \
    "  if (cr.x >= pc.cols || cr.y >= pc.cols) return false;\n" \
    "  ivec2 sl = ivec2(x, y) - cr * pc.pitch;\n" \
    "  Slot s = slot[cr.y * 8 + cr.x];\n" \
    "  return sl.x < s.rw && sl.y < s.rh;\n" \
    "}\n" \
    "void main() {\n" \
    "  uint lid = gl_LocalInvocationID.x;\n" \
    "  uint cg = lid % CG, pg = lid / CG;\n" \
    "  uint row = pg >> 1, x0 = (pg & 1u) * 8u;\n" \
    "  int tx0 = int(gl_WorkGroupID.x * TW), ty0 = pc.y0 + int(gl_WorkGroupID.y) * TH;\n" \
    "  vec4 acc[8][2];\n" \
    "  for (int i = 0; i < 8; i++) { acc[i][0] = vec4(0.0); acc[i][1] = vec4(0.0); }\n" \
    "  for (uint c0 = 0u; c0 < uint(CIN); c0 += CC) {\n" \
    "    barrier();\n" \
    "    for (uint i = lid; i < CC * HROWS * HWID; i += NTHR) {\n" \
    "      uint ci = i % CC, p = i / CC, col = p % HWID, r = p / HWID;\n" \
    "      int gy = ty0 + int(r) - 1, gx = tx0 + int(col) - 1;\n" \
    "      float v = 0.0;\n" \
    "      if (gx >= 0 && gy >= 0 && gx < pc.gw && gy < pc.gh)\n" \
    "        v = ain[(gy * pc.gw + gx) * CIN + int(c0 + ci)];\n" \
    "      hs[(ci * HROWS + r) * HWID + col] = v;\n" \
    "    }\n" \
    "    for (uint i = lid; i < 9u * CC * CO4; i += NTHR) {\n" \
    "      uint co4 = i % CO4, rest = i / CO4;\n" \
    "      uint ci = rest % CC, tp = rest / CC;\n" \
    "      ws[i] = w[pc.wbase + int(((tp * uint(CIN) + c0 + ci) * uint(COUT)) / 4u + co4)];\n" \
    "    }\n" \
    "    barrier();\n" \
    "    for (uint ky = 0u; ky < 3u; ky++) {\n" \
    "      for (uint ci = 0u; ci < CC; ci++) {\n" \
    "        float a[10];\n" \
    "        uint hb = (ci * HROWS + row + ky) * HWID + x0;\n" \
    "        for (int j = 0; j < 10; j++) a[j] = hs[hb + uint(j)];\n" \
    "        for (uint kx = 0u; kx < 3u; kx++) {\n" \
    "          uint wb = ((ky * 3u + kx) * CC + ci) * CO4 + cg * 2u;\n" \
    "          vec4 b0 = ws[wb], b1 = ws[wb + 1u];\n" \
    "          for (int i = 0; i < 8; i++) {\n" \
    "            float av = a[i + int(kx)];\n" \
    "            acc[i][0] = fma(vec4(av), b0, acc[i][0]);\n" \
    "            acc[i][1] = fma(vec4(av), b1, acc[i][1]);\n" \
    "          }\n" \
    "        }\n" \
    "      }\n" \
    "    }\n" \
    "  }\n" \
    "  int y = ty0 + int(row);\n" \
    "  if (y >= pc.gh) return;\n" \
    "  vec4 bb0 = w[pc.bbase + int(cg * 2u)], bb1 = w[pc.bbase + int(cg * 2u) + 1];\n" \
    "  for (int i = 0; i < 8; i++) {\n" \
    "    int x = tx0 + int(x0) + i;\n" \
    "    bool ok = inRect(x, y);\n" \
    "#if LAST\n" \
    "    aout[y * pc.gw + x] = ok ? vec4((acc[i][0] + bb0).xyz, 0.0) : vec4(0.0);\n" \
    "#else\n" \
    "    int o = (y * pc.gw + x) * int(CO4) + int(cg * 2u);\n" \
    "    aout[o] = ok ? max(acc[i][0] + bb0, vec4(0.0)) : vec4(0.0);\n" \
    "    aout[o + 1] = ok ? max(acc[i][1] + bb1, vec4(0.0)) : vec4(0.0);\n" \
    "#endif\n" \
    "  }\n" \
    "}\n"

/* OUT: every frame of the batch into the destination atlas, in the
   consumer's cell layout, one workgroup layer per frame (gl_WorkGroupID.z).
   A frame covers its cell INCLUDING the replicated border and the cell's
   slack past the right and bottom border: the texel clamps into the frame,
   which is what makes the border a copy of the edge. It reads the network's
   output at the slot's matching texel (pad undoes the wrap padding: a centre
   crop) and the input colour from the SOURCE atlas at the destination
   coordinate itself -- the source atlas and the RGBA destination share one
   layout. A keyed texel is written (0, 0, 0, 0): alpha 0 is the hole, and the
   atlas's alpha is also how a consumer tells a painted cell from one the job
   has not reached. A texel past the destination is not written: the bound is
   the destination's own size, in pc.dstW / pc.dstH. */
#define TAGPU_RESTORE_OUT_CS \
    "layout(local_size_x = 8, local_size_y = 8) in;\n" \
    "layout(std430, binding = 0) readonly buffer ActIn { vec4 net[]; };\n" \
    "layout(std430, binding = 3) readonly buffer Tab { Slot slot[]; };\n" \
    "layout(binding = 4) uniform sampler2D uAtlas;\n" \
    "layout(binding = 5) uniform sampler2D uPal;\n" \
    "layout(binding = 6, rgba8) uniform writeonly image2D uDst;\n" \
    "bool keyOf(vec4 v, int key) {\n" \
    "  if (key < 0) return false;\n" \
    "  return pc.base == 1 ? v.a < 0.5 : int(v.r * 255.0 + 0.5) == key;\n" \
    "}\n" \
    "vec3 colOf(vec4 v) {\n" \
    "  return pc.base == 1 ? v.rgb : texelFetch(uPal, ivec2(int(v.r * 255.0 + 0.5), 0), 0).rgb;\n" \
    "}\n" \
    "void main() {\n" \
    "  int t = int(gl_WorkGroupID.z);\n" \
    "  ivec2 cr = ivec2(t % pc.cols, t / pc.cols);\n" \
    "  Slot s = slot[cr.y * 8 + cr.x];\n" \
    "  ivec2 size = ivec2(s.sw, s.sh);\n" \
    "  ivec2 q = ivec2(gl_GlobalInvocationID.xy);\n" \
    "  ivec2 quad = size + 2 * s.border + ivec2(s.padR, s.padB);\n" \
    "  if (q.x >= quad.x || q.y >= quad.y || size.x <= 0 || size.y <= 0) return;\n" \
    "  ivec2 cell = ivec2(s.dx, s.dy);\n" \
    "  ivec2 f = cell - s.border + q;\n" \
    "  if (f.x < 0 || f.y < 0 || f.x >= pc.dstW || f.y >= pc.dstH) return;\n" \
    "  ivec2 d = clamp(f - cell, ivec2(0), size - 1);\n" \
    "  vec4 pv = texelFetch(uAtlas, cell + d, 0);\n" \
    "  if (keyOf(pv, s.key)) { imageStore(uDst, f, vec4(0.0)); return; }\n" \
    "  ivec2 pad = (ivec2(s.rw, s.rh) - size) / 2;\n" \
    "  ivec2 a = cr * pc.pitch + pad + d;\n" \
    "  vec3 n = net[a.y * pc.gw + a.x].rgb;\n" \
    "  vec3 k = clamp(floor((colOf(pv) - n) * 255.0 + 0.5), 0.0, 255.0);\n" \
    "  imageStore(uDst, f, vec4((k + 0.25) / 255.0, 1.0));\n" \
    "}\n"

/* MIP: one level of a restored twin from the level above it, as an EXACT
   INTEGER 2x2 BOX AVERAGE. The twin is sampled trilinear, so levels 1.. are
   part of the picture, and a driver's own reduction rounds its own way
   (gpu-status 2.45): the reduction is ours, so the levels are the same bytes
   on every driver. A texel of an RGBA8 image reads as exactly k/255, so
   round(v * 255) recovers k with no tolerance, the average is (sum + 1) / 4 in
   integers, and it is written back through the (k + 0.25) / 255 rule.
   uSrc is a view of the source level alone and uLvl a view of the level
   written, so the source can never reach the level being written. pc.dstW /
   pc.dstH is the level written; the caller refuses odd levels, which would
   need a weighted three-tap rather than this. */
#define TAGPU_RESTORE_MIP_CS \
    "layout(local_size_x = 8, local_size_y = 8) in;\n" \
    "layout(binding = 7) uniform sampler2D uSrc;\n" \
    "layout(binding = 8, rgba8) uniform writeonly image2D uLvl;\n" \
    "void main() {\n" \
    "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n" \
    "  if (p.x >= pc.dstW || p.y >= pc.dstH) return;\n" \
    "  ivec2 q = p * 2;\n" \
    "  ivec4 s = ivec4(round(texelFetch(uSrc, q, 0) * 255.0));\n" \
    "  s += ivec4(round(texelFetch(uSrc, q + ivec2(1, 0), 0) * 255.0));\n" \
    "  s += ivec4(round(texelFetch(uSrc, q + ivec2(0, 1), 0) * 255.0));\n" \
    "  s += ivec4(round(texelFetch(uSrc, q + ivec2(1, 1), 0) * 255.0));\n" \
    "  imageStore(uLvl, p, (vec4((s + 1) / 4) + 0.25) / 255.0);\n" \
    "}\n"

#endif
