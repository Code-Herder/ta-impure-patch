/* The GLSL of the shadow depth programs: shader source only, with no GL calls
   beside it.

   WHY A HEADER AND NOT A `.c`. `tools/spirv-gen.py` reads its shader sources by
   preprocessing one translation unit per entry in `SOURCES` -- `src/<name>.c` by
   default, or whatever `HEADER_SOURCES` maps the name to. One `HEADER_SOURCES`
   entry maps `tagpu_shadow` here, and `SOURCES` and `PROGRAMS` must keep that
   name: without it `inc/spirv/tagpu_shadow.spv.h`, which
   `tagpu_vk_shadow.c:93` includes, is not generated.

   WHY IT SITS IN `src/` RATHER THAN `inc/`. Its sibling
   `tagpu_hires_glsl.h` must, because the hires fragment shader pulls macros out
   of `src/tagpu_glsl.h` and the generator preprocesses with `-Iinc` alone; these
   two are kept in one place rather than split on a rule only one of them needs.

   THE TEXT IS PINNED BY THE BUILD: every generated header carries a per-shader
   md5 of the GLSL it was compiled from, so a single character moved here fails
   `tools/spirv-check.sh`. */
#ifndef TAGPU_SHADOW_GLSL_H
#define TAGPU_SHADOW_GLSL_H

/* ---- the depth program for the native stream: attributes 4 (world x,
   PROJECTED z) and 5 (posed height) of tagpu_native.c's VAO, and the
   caster's three numbers. The world point is the one the unit shader's
   vShW derives -- the same expression, so a unit is consistent with itself
   (renderers.md 2.11): real z = projected z + (altitude + height)/2, and the
   shadow's height is the ground plus the throw plus the scaled model height. */
static const char* VS_U =
    "#version 330 core\n"
    "layout(location=4) in vec2 aWorld;\n"
    "layout(location=5) in float aVY;\n"
    "uniform mat4 uShadowMat;\n"
    "uniform vec3 uCast;\n"                  /* altitude, ground + throw, sv */
    "void main(){\n"
    "  vec3 W = vec3(aWorld.x, uCast.y + uCast.z * aVY,\n"
    "                aWorld.y + (uCast.x + aVY) * 0.5);\n"
    "  gl_Position = uShadowMat * vec4(W, 1.0);\n"
    "}\n";
/* the heightfield mesh: one world point per vertex (tagpu_terr.c) */
static const char* VS_H =
    "#version 330 core\n"
    "layout(location=0) in vec3 aW;\n"
    "uniform mat4 uShadowMat;\n"
    "void main(){ gl_Position = uShadowMat * vec4(aW, 1.0); }\n";
static const char* FS_NONE =
    "#version 330 core\n"
    "void main(){}\n";

#endif /* TAGPU_SHADOW_GLSL_H */
