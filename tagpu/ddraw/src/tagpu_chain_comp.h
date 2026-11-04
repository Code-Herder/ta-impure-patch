#ifndef TAGPU_CHAIN_COMP_H
#define TAGPU_CHAIN_COMP_H
/* The mip chains' reducer -- Vulkan 1.0 compute, the format in inc/tagpu_chain.h. This header is
   the ONE copy of the shader text and no C file includes it: tools/spirv-gen.py reads the macros
   below and compiles them into inc/spirv/tagpu_chain_comp.spv.h, which tagpu_vk_chain.c runs.
   A shader is assembled as `#version 450`, TAGPU_CHAIN_COMMON_CS, then its own macro.

   THE PUSH CONSTANTS are 128 bytes, Vulkan's guaranteed minimum. One workgroup reduces one cell:
     * range mode (`list` 0): cells first .. first+count-1, row-major over `cols`;
     * list mode (`list` 1): `count` <= 24 entries of `cell[]`, each x | y << 16.
   A cell's source tile starts at cell * pitch + border of the atlas (34 and 1 for both terrain
   atlases) and is 32 x 32 -- the GRID form. With `table` 1 (range mode only; the feature pass's
   frames) cell i's source is Src.r[2i] = sx | sy << 16 and its valid size Src.r[2i+1] =
   tw | th << 16, each <= 32: a tile of a frame clipped to the frame (tagpu_vk_feat.c, which bounds
   both to the atlas). `check` 0 writes the chain; 1 and 2 compare it instead and count into the
   counter pair Chk.n[2 * (check - 1)] (mismatched texels) and the one after it (cells seen), so
   the base chain's check and the restored one's share one buffer and never one counter. */
#define TAGPU_CHAIN_COMMON_CS \
    "layout(push_constant) uniform PC {\n" \
    "  int first, count, list, cols, pitch, border, check, table;\n" \
    "  uint cell[24];\n" \
    "} pc;\n"

/* REDUCE: one cell's whole chain, tile levels 1..5 into image levels 0..4 (uL1..uL5), in the
   integer arithmetic of inc/tagpu_chain.h -- THE C MIRROR IS tagpu_chain_ref
   (src/tagpu_chain_ref.c): EDIT BOTH OR NEITHER.

   Invocation (x, y) of the 8 x 8 group reads its 4 x 4 block of level 0, writes its four level-1
   texels and its level-2 texel, and leaves the level-2 sums (sum of RGB over O, |O|, |B|) in
   shared memory; then x, y < 4 write level 3 from 2 x 2 of those, x, y < 2 level 4, and (0, 0)
   level 5, a barrier between. A sum of sums is the sum over the union, so every level is the
   level-0 bytes reduced once. The sums are exact uints (at most 1024 * 255) and the divisions
   integer, never float: a driver may divide floats through the reciprocal, and the restorer
   measured a float floor-divide off by one on an AMD driver (tagpu_restore_comp.h FILL).

   A texel reads back exactly as round(v * 255) (an RGBA8 texel is k / 255) and is written as
   (k + 0.25) / 255, which stores k under a truncating and a rounding conversion alike.

   THE BOUND. A workgroup reads only its own cell's 32 x 32 interior -- in the table form only the
   tw x th of it the cell's tile covers, so no texel of another frame or of a frame's border --
   so no tile is mixed with another, and writes only its own cell's block at each level, so two
   workgroups never write one texel. A texel outside the valid size counts in neither B nor O, and
   a level texel whose block holds none is written 0, which nothing samples (tagpu_feat.c
   taFChain clamps into the valid blocks). Src is read at 2i and 2i + 1 for i < first + count,
   which the caller keeps inside the buffer it binds. A workgroup past `count` returns whole: `wg`
   is uniform across it, so every barrier() below stays in uniform control flow. Binding 7 is a
   valid buffer in the grid form too (the terrain binds its counters there) and is not read. */
#define TAGPU_CHAIN_REDUCE_CS \
    "layout(local_size_x = 8, local_size_y = 8) in;\n" \
    "layout(binding = 0) uniform sampler2D uSrc;\n" \
    "layout(binding = 1, rgba8) uniform image2D uL1;\n" \
    "layout(binding = 2, rgba8) uniform image2D uL2;\n" \
    "layout(binding = 3, rgba8) uniform image2D uL3;\n" \
    "layout(binding = 4, rgba8) uniform image2D uL4;\n" \
    "layout(binding = 5, rgba8) uniform image2D uL5;\n" \
    "layout(std430, binding = 6) buffer Chk { uint n[4]; } chk;\n" \
    "layout(std430, binding = 7) readonly buffer Src { uint r[]; } src;\n" \
    "shared uvec4 s2[64]; shared uint b2[64];\n" \
    "shared uvec4 s3[16]; shared uint b3[16];\n" \
    "shared uvec4 s4[4];  shared uint b4[4];\n" \
    "uvec4 taMean(uvec4 s, uint nb) {\n" \
    "  uint o = s.w;\n" \
    "  uvec3 c = o > 0u ? (s.rgb + o / 2u) / o : uvec3(0u);\n" \
    "  return uvec4(c, nb > 0u ? (o * 255u + nb / 2u) / nb : 0u);\n" \
    "}\n" \
    "uvec4 taGot(int lvl, ivec2 at) {\n" \
    "  vec4 v = lvl == 1 ? imageLoad(uL1, at) : lvl == 2 ? imageLoad(uL2, at) :\n" \
    "           lvl == 3 ? imageLoad(uL3, at) : lvl == 4 ? imageLoad(uL4, at) : imageLoad(uL5, at);\n" \
    "  return uvec4(round(v * 255.0));\n" \
    "}\n" \
    "void taPut(int lvl, ivec2 at, uvec4 k) {\n" \
    "  if (pc.check != 0) {\n" \
    "    if (taGot(lvl, at) != k) atomicAdd(chk.n[2 * (pc.check - 1)], 1u);\n" \
    "    return;\n" \
    "  }\n" \
    "  vec4 v = (vec4(k) + 0.25) / 255.0;\n" \
    "  if (lvl == 1) imageStore(uL1, at, v);\n" \
    "  else if (lvl == 2) imageStore(uL2, at, v);\n" \
    "  else if (lvl == 3) imageStore(uL3, at, v);\n" \
    "  else if (lvl == 4) imageStore(uL4, at, v);\n" \
    "  else imageStore(uL5, at, v);\n" \
    "}\n" \
    "void main() {\n" \
    "  int wg = int(gl_WorkGroupID.y) * 1024 + int(gl_WorkGroupID.x);\n" \
    "  if (wg >= pc.count) return;\n" \
    "  ivec2 c;\n" \
    "  int i = pc.first + wg;\n" \
    "  if (pc.list != 0) { uint e = pc.cell[wg]; c = ivec2(int(e & 0xFFFFu), int(e >> 16)); }\n" \
    "  else c = ivec2(i % pc.cols, i / pc.cols);\n" \
    "  ivec2 so = c * pc.pitch + pc.border;\n" \
    "  ivec2 vs = ivec2(32);\n" \
    "  if (pc.table != 0) {\n" \
    "    uint a = src.r[2 * i], b = src.r[2 * i + 1];\n" \
    "    so = ivec2(int(a & 0xFFFFu), int(a >> 16));\n" \
    "    vs = ivec2(int(b & 0xFFFFu), int(b >> 16));\n" \
    "  }\n" \
    "  ivec2 l = ivec2(gl_LocalInvocationID.xy);\n" \
    "  uvec4 q2 = uvec4(0u); uint n2 = 0u;\n" \
    "  for (int j = 0; j < 2; j++)\n" \
    "    for (int i = 0; i < 2; i++) {\n" \
    "      uvec4 q1 = uvec4(0u); uint n1 = 0u;\n" \
    "      for (int y = 0; y < 2; y++)\n" \
    "        for (int x = 0; x < 2; x++) {\n" \
    "          ivec2 t = l * 4 + ivec2(i, j) * 2 + ivec2(x, y);\n" \
    "          if (t.x >= vs.x || t.y >= vs.y) continue;\n" \
    "          uvec4 k = uvec4(round(texelFetch(uSrc, so + t, 0) * 255.0));\n" \
    "          n1++;\n" \
    "          if (k.a >= 128u) q1 += uvec4(k.rgb, 1u);\n" \
    "        }\n" \
    "      taPut(1, c * 16 + l * 2 + ivec2(i, j), taMean(q1, n1));\n" \
    "      q2 += q1; n2 += n1;\n" \
    "    }\n" \
    "  taPut(2, c * 8 + l, taMean(q2, n2));\n" \
    "  s2[l.y * 8 + l.x] = q2; b2[l.y * 8 + l.x] = n2;\n" \
    "  memoryBarrierShared(); barrier();\n" \
    "  if (l.x < 4 && l.y < 4) {\n" \
    "    int a = l.y * 16 + l.x * 2;\n" \
    "    uvec4 q = s2[a] + s2[a + 1] + s2[a + 8] + s2[a + 9];\n" \
    "    uint n = b2[a] + b2[a + 1] + b2[a + 8] + b2[a + 9];\n" \
    "    taPut(3, c * 4 + l, taMean(q, n));\n" \
    "    s3[l.y * 4 + l.x] = q; b3[l.y * 4 + l.x] = n;\n" \
    "  }\n" \
    "  memoryBarrierShared(); barrier();\n" \
    "  if (l.x < 2 && l.y < 2) {\n" \
    "    int a = l.y * 8 + l.x * 2;\n" \
    "    uvec4 q = s3[a] + s3[a + 1] + s3[a + 4] + s3[a + 5];\n" \
    "    uint n = b3[a] + b3[a + 1] + b3[a + 4] + b3[a + 5];\n" \
    "    taPut(4, c * 2 + l, taMean(q, n));\n" \
    "    s4[l.y * 2 + l.x] = q; b4[l.y * 2 + l.x] = n;\n" \
    "  }\n" \
    "  memoryBarrierShared(); barrier();\n" \
    "  if (l.x == 0 && l.y == 0) {\n" \
    "    taPut(5, c, taMean(s4[0] + s4[1] + s4[2] + s4[3], b4[0] + b4[1] + b4[2] + b4[3]));\n" \
    "    if (pc.check != 0) atomicAdd(chk.n[2 * (pc.check - 1) + 1], 1u);\n" \
    "  }\n" \
    "}\n"

#endif
