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
   which sizes the matching block. Changing it changes the GLSL, so it fails
   `tools/spirv-check.sh` unless the generated header is regenerated with it.

   The hand-over types this file used to sit beside are now in
   `src/tagpu_vk_hires.h`, with the measurement that explains why nothing
   publishes one. [The vulkan-only plan, landing 11 D3; gpu-status 2.80.] */

#define TAGPU_HMAXPIECE 48

#endif
