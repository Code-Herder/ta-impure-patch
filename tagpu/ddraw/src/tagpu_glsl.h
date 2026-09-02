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
#endif
