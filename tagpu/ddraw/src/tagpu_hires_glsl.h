/* The GLSL of the replacement-mesh program, lifted out of `tagpu_hires_draw.c`
   so that the file that made the GL calls can be deleted and the SHADERS can
   stay -- the Vulkan lane draws this pair as `hires`
   (`tools/spirv-gen.py:194`).

   WHY A HEADER AND NOT A `.c`. `tools/spirv-gen.py` reads its shader sources by
   preprocessing one translation unit per entry in `SOURCES` -- `src/<name>.c` by
   default, or whatever `HEADER_SOURCES` maps the name to. Lifting the strings
   here and adding one `HEADER_SOURCES` entry keeps `SOURCES` and `PROGRAMS`
   untouched, which matters: removing a name from those would stop generating
   `inc/spirv/tagpu_hires_draw.spv.h`, which the surviving Vulkan file includes.

   WHY IT SITS IN `src/` AND NOT `inc/`. The fragment shader pastes five
   `TAGPU_GLSL_*` macros out of `src/tagpu_glsl.h`, and the generator
   preprocesses with `-Iinc` alone -- a copy in `inc/` could not find them. The
   vertex shader also stringifies `TAGPU_HMAXPIECE`, which IS in `inc/`.

   `STR`/`STR2` LIVE HERE NOW rather than in the `.c`, because the stringify is
   part of the shader text: `uniform vec4 uPiece[48*3]` is what the generator
   must see, and a macro left behind in the deleted file would leave the array
   unsized.

   THE TEXT IS UNCHANGED, and the build proves it: every generated header carries
   a per-shader md5 of the GLSL it was compiled from, so a single character moved
   here fails `tools/spirv-check.sh`. [The vulkan-only plan, landing 11 D1.] */
#ifndef TAGPU_HIRES_GLSL_H
#define TAGPU_HIRES_GLSL_H

#include "tagpu_hires.h"   /* TAGPU_HMAXPIECE */
#include "tagpu_glsl.h"    /* TAGPU_GLSL_SCAF_*, TAGPU_GLSL_FOG_* */

#define STR2(x) #x
#define STR(x) STR2(x)

static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"      /* engine model space, REST     */
    "layout(location=1) in vec3 aNrm;\n"
    "layout(location=2) in vec2 aUV;\n"
    "layout(location=3) in float aPiece;\n"
    /* 3 rows of a 4x3 per piece: the COB pose, rest -> posed. Identity when
       the unit's pose could not be read; all zeros when the piece is HIDden,
       which collapses its triangles onto the model origin and rasterises
       nothing. */
    "uniform vec4 uPiece[" STR(TAGPU_HMAXPIECE) "*3];\n"
    "uniform vec2 uGame;\n"
    "uniform vec2 uOffset;\n"
    "uniform float uZoom;\n"
    "uniform vec2 uZoomC;\n"
    "uniform float uDepthScale;\n"
    "uniform vec4 uAnchor;\n"                 /* ax, ay, world x, world z     */
    "uniform vec3 uYawEnc;\n"                 /* cos yaw, sin yaw, depth key  */
    "uniform int uSlant;\n"                   /* 1: structure shadow slant    */
    "uniform int uDepthPass;\n"               /* 1: into the shadow map (G14i) */
    "uniform mat4 uShadowMat;\n"
    "uniform vec3 uCast;\n"                   /* altitude, ground + throw, sv */
    "out vec3 vPos; out vec3 vNrm; out vec2 vUV;\n"
    "out vec2 vWorld; out float vEnc; out float vVY;\n"
    "void main(){\n"
    "  int pb = int(aPiece) * 3;\n"
    "  vec4 rp = vec4(aPos, 1.0);\n"
    "  vec3 bp = vec3(dot(uPiece[pb], rp), dot(uPiece[pb+1], rp), dot(uPiece[pb+2], rp));\n"
    "  vec3 bn = vec3(dot(uPiece[pb].xyz, aNrm), dot(uPiece[pb+1].xyz, aNrm),\n"
    "                 dot(uPiece[pb+2].xyz, aNrm));\n"
    /* body yaw, the engine's own rotation (tagpu_native.c rot2), applied to
       the posed piece: x' = x cos - z sin, z' = x sin + z cos */
    "  float c = uYawEnc.x, s = uYawEnc.y;\n"
    "  vec3 m = vec3(bp.x*c - bp.z*s, bp.y, bp.x*s + bp.z*c);\n"
    "  vec3 n = vec3(bn.x*c - bn.z*s, bn.y, bn.x*s + bn.z*c);\n"
    /* the engine's projection, per vertex, exactly as the native emitters bake
       it on the CPU: sx = anchor + x, sy = anchor + (-z - y/2); a structure's
       shadow pass takes its slant projection instead (x + y/4, -z - y/4) */
    "  vec2 pm = (uSlant == 1) ? vec2(m.x + m.y*0.25, -m.z - m.y*0.25)\n"
    "                          : vec2(m.x, -m.z - m.y*0.5);\n"
    "  vec2 p0 = uAnchor.xy + pm;\n"
    "  float md = clamp((2.0*m.y - m.z) / 256.0, -1.8, 1.8);\n"
    "  float enc = uYawEnc.z + md;\n"
    "  vec2 p = (p0 + uOffset - uZoomC) * uZoom + uZoomC;\n"
    "  gl_Position = vec4(p.x/uGame.x*2.0-1.0, p.y/uGame.y*2.0-1.0,\n"
    "                     clamp(1.0 - enc/uDepthScale, 0.0, 1.0), 1.0);\n"
    "  vPos = m; vNrm = n; vUV = aUV;\n"
    "  vWorld = uAnchor.zw + pm;\n"
    "  vEnc = enc; vVY = m.y;\n"
    /* Classic++ shadows: the depth pass takes the posed point in the WORLD --
       real z = projected z + (altitude + height)/2, the height the ground
       plus the throw plus the scaled model height -- the expression
       tagpu_shadow.c's program uses for a 3DO, so a replacement mesh casts
       from where its 3DO would (renderers.md 2.4, 2.11) */
    "  if (uDepthPass == 1) {\n"
    "    vec3 W = vec3(uAnchor.z + m.x, uCast.y + uCast.z * m.y,\n"
    "                  uAnchor.w + pm.y + (uCast.x + m.y) * 0.5);\n"
    "    gl_Position = uShadowMat * vec4(W, 1.0);\n"
    "  }\n"
    "}\n";

static const char* FS =
    "#version 330 core\n"
    "in vec3 vPos; in vec3 vNrm; in vec2 vUV;\n"
    "in vec2 vWorld; in float vEnc; in float vVY;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAlbedo;\n"
    "uniform sampler2D uNormal;\n"
    "uniform sampler2D uLUT;\n"
    "uniform sampler2D uPal;\n"
    TAGPU_GLSL_SCAF_UNIFORMS
    TAGPU_GLSL_FOG_UNIFORMS
    "uniform int uHasNrm;\n"
    "uniform vec4 uBase;\n"                   /* baseColorFactor, linear      */
    "uniform vec2 uMR;\n"                     /* metallic, roughness          */
    "uniform float uCutoff;\n"                /* < 0 = opaque                 */
    "uniform int uDepthPass;\n"               /* 1: depth only, after the cutout */
    "uniform int uShadow;\n"
    "uniform float uAlpha;\n"
    "uniform float uGamma;\n"                 /* the engine's palette scale   */
    "uniform float uWaterT;\n"
    "uniform int uWaterMode;\n"
    "uniform float uDigT;\n"
    "uniform vec3 uLight;\n"
    "uniform vec3 uView;\n"
    "uniform vec2 uSunAmb;\n"                 /* sun, ambient                 */
    "uniform float uAnchorMix;\n"             /* 0 modern .. 1 engine ramp    */
    "uniform ivec3 uShade;\n"
    TAGPU_GLSL_FOG_FN
    "const float PI = 3.14159265;\n"
    "float lum(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    /* The engine's own answer for a face with this normal, as a brightness:
       its SHD row, applied to a reference palette index, over that index's own
       brightness — so a neutral row comes back as 1.0 and the curve either
       side is the engine's, not one we invented. */
    "float engineMul(vec3 N){\n"
    "  float I = dot(N, normalize(uLight));\n"
    "  int row = clamp(uShade.y + uShade.z * int(floor(I*12.0 + 0.5)), 0, 31);\n"
    "  int a = int(texelFetch(uLUT, ivec2(uShade.x, row), 0).r * 255.0 + 0.5);\n"
    "  float r = lum(texelFetch(uPal, ivec2(uShade.x, 0), 0).rgb);\n"
    "  return r > 0.004 ? lum(texelFetch(uPal, ivec2(a, 0), 0).rgb) / r : 1.0;\n"
    "}\n"
    /* the same trick for the fog overlay's grey: the engine remaps a pixel's
       palette INDEX through uFogLUT, so ask it what that does to our reference */
    "float fogMul(){\n"
    "  int a = int(texelFetch(uFogLUT, ivec2(uShade.x, 0), 0).r * 255.0 + 0.5);\n"
    "  float r = lum(texelFetch(uPal, ivec2(uShade.x, 0), 0).rgb);\n"
    "  return r > 0.004 ? lum(texelFetch(uPal, ivec2(a, 0), 0).rgb) / r : 1.0;\n"
    "}\n"
    "vec3 pbr(vec3 alb, vec3 N){\n"
    "  vec3 L = normalize(uLight), V = normalize(uView), H = normalize(L + V);\n"
    "  float NdL = max(dot(N, L), 0.0), NdV = max(dot(N, V), 1e-4);\n"
    "  float NdH = max(dot(N, H), 0.0), VdH = max(dot(V, H), 0.0);\n"
    "  float r = clamp(uMR.y, 0.045, 1.0);\n"
    "  float a = r*r, a2 = a*a;\n"
    "  float d = NdH*NdH*(a2 - 1.0) + 1.0;\n"
    "  float D = a2 / (PI * d * d);\n"
    "  float k = a * 0.5;\n"
    "  float G = (NdL / (NdL*(1.0-k) + k)) * (NdV / (NdV*(1.0-k) + k));\n"
    "  vec3 F0 = mix(vec3(0.04), alb, uMR.x);\n"
    "  vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);\n"
    "  vec3 spec = D * G * F / (4.0 * NdL * NdV + 1e-4);\n"
    "  vec3 kd = (1.0 - F) * (1.0 - uMR.x);\n"
    /* The ambient stands in for an environment map we do not have, and it has
       to cover BOTH lobes. A metal has no diffuse at all, so lighting it with
       `albedo * ambient` alone leaves it BLACK everywhere the one directional
       highlight does not land — which is most of a unit most of the time. Give
       the specular lobe its own ambient, weighted by F0 and opened by
       roughness. */
    "  vec3 amb = uSunAmb.y * (kd * alb + F0 * (2.0 - r));\n"
    "  return (kd * alb / PI + spec) * uSunAmb.x * NdL + amb;\n"
    "}\n"
    "void main(){\n"
    "  vec4 tex = texture(uAlbedo, vUV);\n"
    "  if (uCutoff >= 0.0 && tex.a * uBase.a < uCutoff) discard;\n"
    "  if (uDepthPass == 1) { frag = vec4(0.0); return; }\n"
    TAGPU_GLSL_SCAF_TEST
    TAGPU_GLSL_FOG_DISCARD
    "  if (vVY <= uDigT) discard;\n"
    "  if (uShadow == 1) { if (vVY <= uWaterT) discard;\n"
    "                      frag = vec4(0.0, 0.0, 0.0, 0.5); return; }\n"
    /* double-sided: face the viewer, the same rule the native emitters use */
    "  vec3 N = normalize(vNrm);\n"
    "  if (dot(N, normalize(uView)) < 0.0) N = -N;\n"
    "  if (uHasNrm == 1) {\n"
    /* tangent frame from screen-space derivatives (Mikkelsen): no TANGENT
       attribute needed, and correct for whatever UV layout the model has */
    "    vec3 dp1 = dFdx(vPos), dp2 = dFdy(vPos);\n"
    "    vec2 du1 = dFdx(vUV),  du2 = dFdy(vUV);\n"
    "    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);\n"
    "    vec3 T = dp2p*du1.x + dp1p*du2.x;\n"
    "    vec3 B = dp2p*du1.y + dp1p*du2.y;\n"
    "    float im = inversesqrt(max(dot(T,T), dot(B,B)));\n"
    "    if (im < 1e8) {\n"
    "      vec3 t = texture(uNormal, vUV).xyz * 2.0 - 1.0;\n"
    "      N = normalize(mat3(T*im, B*im, N) * t);\n"
    "    }\n"
    "  }\n"
    /* base colour textures are sRGB by the glTF spec, the factor is linear */
    "  vec3 albLin = pow(tex.rgb, vec3(2.2)) * uBase.rgb;\n"
    "  vec3 modern = pow(max(pbr(albLin, N), 0.0), vec3(1.0/2.2));\n"
    "  vec3 engine = pow(albLin, vec3(1.0/2.2)) * engineMul(N);\n"
    "  vec3 rgb = mix(modern, engine, uAnchorMix);\n"
    "  if (taFogC.y >= 0.5) rgb *= fogMul();\n"
    /* engine water table (prog+0xD0): r/2, g/2, b/2 + 0x32 */
    "  if (vVY <= uWaterT) {\n"
    "    if (uWaterMode == 1) discard;\n"
    "    rgb = rgb * 0.5 + vec3(0.0, 0.0, 50.0/255.0);\n"
    "  }\n"
    /* A replacement mesh is the one thing on screen whose colour never came
       from a palette, so resolving through the presented one cannot reach it:
       apply the engine's own scale here instead, the same min(255, c*gamma)
       0x4BA200 applies to every palette entry (tagpu_pal.h). 1.0 at the
       default Gamma, where this is the identity. */
    "  frag = vec4(clamp(rgb * uGamma, 0.0, 1.0) * uAlpha, uAlpha);\n"
    "}\n";

#endif /* TAGPU_HIRES_GLSL_H */
