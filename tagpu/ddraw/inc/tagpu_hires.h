#ifndef TAGPU_HIRES_H
#define TAGPU_HIRES_H
/* ALL THAT IS LEFT OF THE REPLACEMENT-MESH HEADER, and why it is left.

   `tagpu_hires.c` (the glTF loader) and `tagpu_hires_draw.c` (the GL draw) were
   deleted by landing 11 D3, on the owner's ruling that glTF replacement models
   are disabled and their implementation is TODO and out of scope. Every
   accessor this header used to declare went with them.

   `TAGPU_HMAXPIECE` stays because it is not about the loader: it is the size of
   the per-piece uniform array in the shader the VULKAN lane still draws. Two
   live files need it -- `src/tagpu_hires_glsl.h`, where the vertex shader
   stringifies it into `uniform vec4 uPiece[48*3]`, and `tagpu_vk_hires.c`,
   which BOUNDS a memcpy with it. Changing it changes the GLSL, so it fails
   `tools/spirv-check.sh` unless the generated header is regenerated with it.

   BUT REGENERATING THE SPIR-V IS NOT ENOUGH, AND THIS COMMENT USED TO SAY IT
   WAS. `tagpu_vk_hires.c` does NOT size its uniform block from this macro: the
   block is the literals `VS_SZ 2480` / `VS_PIECE 0` / `VS_GAME 2304`, written
   out by hand from what the generated header prints. At 48 the bound and the
   literals agree exactly -- 48 * 12 * 4 = 2304 = VS_GAME - VS_PIECE. Raise this
   macro to 64, regenerate as the paragraph above instructs, and the bound then
   admits np = 64 while the destination is still 2 480 bytes: the memcpy writes
   3 072 and walks off the end of the block. So that file now carries a
   COMPILE-TIME assertion tying the two together, and a bump fails the build
   rather than the frame. [Landing 11 D3's review, finding M1.]

   The hand-over types this file used to sit beside are now in
   `src/tagpu_vk_hires.h`, with the measurement that explains why nothing
   publishes one. [The vulkan-only plan, landing 11 D3; gpu-status 2.80.] */

#define TAGPU_HMAXPIECE 48

#endif
