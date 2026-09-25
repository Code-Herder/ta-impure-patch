/* The GLSL of the shadow depth programs: shader source only.

   WHY A HEADER AND NOT A `.c`. `tools/spirv-gen.py` reads its shader sources by
   preprocessing one translation unit per entry in `SOURCES` -- `src/<name>.c` by
   default, or whatever `HEADER_SOURCES` maps the name to. One `HEADER_SOURCES`
   entry maps `tagpu_shadow` here, and `SOURCES` and `PROGRAMS` must keep that
   name: without it `inc/spirv/tagpu_shadow.spv.h`, which
   `tagpu_vk_shadow.c` includes, is not generated.

   IT SITS IN `src/` beside the pass that draws it, and `HEADER_SOURCES`
   names that path.

   THE TEXT IS PINNED BY THE BUILD: every generated header carries a per-shader
   SHA-256 of the GLSL it was compiled from, so a single character moved here fails
   `tools/spirv-check.sh`. */
#ifndef TAGPU_SHADOW_GLSL_H
#define TAGPU_SHADOW_GLSL_H

/* the heightfield mesh: one world point per vertex. Nothing builds one
   today; tagpu_vk_shadow.h, TAGPU_SHADOWHAND. The light matrix fills [-1, 1]
   in z and Vulkan clips to [0, w], so z is remapped here; tagpu_vk_shadow.c
   item 2. */
static const char* VS_H =
    "#version 330 core\n"
    "layout(location=0) in vec3 aW;\n"
    "uniform mat4 uShadowMat;\n"
    "void main(){\n"
    "  gl_Position = uShadowMat * vec4(aW, 1.0);\n"
    "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n"
    "}\n";
static const char* FS_NONE =
    "#version 330 core\n"
    "void main(){}\n";

#endif /* TAGPU_SHADOW_GLSL_H */
