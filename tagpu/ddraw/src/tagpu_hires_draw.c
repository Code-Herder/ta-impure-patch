/* tagpu_hires_draw.c — the replacement-mesh RENDER PASS.

   A glTF unit does not belong in the native pass's shader. That one is a
   faithful reproduction of TA's 1997 rasteriser: colour is a PALETTE INDEX end
   to end, shading is one flat SHD row per face computed on the CPU, and the
   atlas is an R8 index texture. A modern asset arrives with smooth normals,
   normal maps, metallic/roughness and 1024px base colour; pushed through that
   shader it comes out flat, unlit and unmipmapped.

   So replacement meshes get their own program — and share the FRAME CONTRACT
   with the native pass, which is what makes them composite with engine units,
   terrain and effects rather than float above them:

     - the same FBO at the same supersample, premultiplied vec4(rgb*a, a);
     - the same viewport-px -> NDC mapping and the same DEPTH KEY encoding
       (the painter's row key, not a world-space Z), so a native 3DO unit in a
       nearer row still covers ours;
     - the same G12a scaffold occlusion test, the same fog-grid discard, the
       same waterline / digger rules, the same engine anchor and body yaw;
     - the same silhouette shadow: black at 50%, +5px, at ground height.

   What it does differently, being its own program:

     - the mesh is a STATIC vertex buffer uploaded once (tagpu_hires.c); a unit
       costs a handful of uniforms and one draw per material, and NOTHING is
       transformed on the CPU. Yaw and the anchor are uniforms, applied in the
       vertex shader;
     - the COB POSE is uniforms too. Every vertex carries the index of the
       piece it belongs to, and `uPiece` holds one 4x3 matrix per piece that
       carries a rest vertex to where the unit's script is holding that piece
       this frame. So a replacement unit walks, aims and recoils off the same
       static buffer, at one uniform upload per unit rather than one draw per
       piece. tagpu_native.c reads the pose out of the engine; the axis and
       angle conventions it decodes are in research/notes/model-import.md;
     - per-PIXEL lighting against the engine's own light direction, so our unit
       is lit from where the game lights everything else;
     - normal maps through a tangent frame derived from screen-space
       derivatives (glTF gives us no TANGENT, and deriving one costs less than
       storing one);
     - metallic/roughness through a small GGX lobe;
     - sRGB in, linear light, sRGB out — the frame is 8-bit sRGB because the
       engine's palette is.

   THE ANCHOR KNOB (`anchor=` in tagpu_hires.on, 0 by default). A PBR unit
   standing in a palette-shaded scene can read as pasted in. At anchor=1 the
   albedo is instead shaded by the ENGINE's own curve: the face's SHD row is
   computed exactly as tagpu_render3do does, and the brightness that row gives
   a reference palette index is used as a multiplier. Anything between mixes
   the two, so the look is a dial rather than a rewrite.

   Double-sided always: the normal is flipped toward the viewer, which is what
   the native emitters already do. Nothing here culls, so a model whose winding
   disagrees with glTF's still draws. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "opengl_utils.h"
#include "tagpu_hires.h"
#include "tagpu_hires_draw.h"
#include "tagpu_glsl.h"

/* the engine's light and view directions, in TA model space — the same two
   constants tagpu_render3do.c shades engine 3DO faces with */
static const float SH_V[3] = { 0.0f, 0.8944f, -0.4472f };
static const float SH_L[3] = { -0.35f, 0.80f, -0.49f };

/* the palette index whose SHD ramp calibrates the anchor curve: 0xB4 is the
   metal grey the replacement slot has always defaulted to, and one of the
   indices with a sane ramp (GUI colours render black through the LUT) */
#define SHADE_REF 0xB4

static void dlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

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
    "uniform int uDepthPass;\n"               /* 1: into the shadow map (G14h) */
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
    "  frag = vec4(clamp(rgb, 0.0, 1.0) * uAlpha, uAlpha);\n"
    "}\n";

typedef void (APIENTRY *PFN_DRAWARRAYS)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_ACTIVETEX)(GLenum);
typedef void (APIENTRY *PFN_STENCILFUNC)(GLenum, GLint, GLuint);
typedef void (APIENTRY *PFN_STENCILOP)(GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_COLORMASK)(GLboolean, GLboolean, GLboolean, GLboolean);
static PFN_DRAWARRAYS  x_glDrawArrays;
static PFN_ACTIVETEX   x_glActiveTexture;
static PFN_STENCILFUNC x_glStencilFunc;
static PFN_STENCILOP   x_glStencilOp;
static PFN_COLORMASK   x_glColorMask;

static int    s_state = 0;                 /* 0 unloaded, 1 ready, 2 failed */
static GLuint s_prog;
static GLint  u_game, u_offset, u_zoom, u_zoomC, u_depthScale, u_anchor, u_yawEnc;
static GLint  u_piece;
static GLint  u_hasNrm, u_base, u_mr, u_cutoff, u_shadow, u_alpha, u_slant;
static GLint  u_waterT, u_waterMode, u_digT, u_light, u_view, u_sunAmb;
static GLint  u_anchorMix, u_shade, u_fog, u_fogOrg, u_fogDim;
static GLint  u_scafOn, u_scafP, u_ss, u_zoomF, u_zoomCF;
static GLint  u_depthPass, u_shadowMat, u_cast;    /* the depth pass (G14h) */

/* tagpu_hires.on tweaks, re-read on the same 30-frame cadence as the rest */
static float s_anchorMix = 0.0f;
static float s_sun = 2.67f;      /* pi * 0.85: full light lands near albedo   */
static float s_amb = 0.18f;
static int   s_normalMaps = 1;
static int   s_log = 0;

static void* getgl(const char* n)
{
    void* p = xwglGetProcAddress ? (void*)xwglGetProcAddress(n) : NULL;
    if (!p) { HMODULE gl = GetModuleHandleA("opengl32.dll");
              if (gl) p = (void*)GetProcAddress(gl, n); }
    return p;
}

static GLuint mksh(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, (const GLchar**)&src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        char b[1100];
        _snprintf(b, sizeof b, "hires draw: shader FAILED: %s", log);
        dlog(b);
        s_state = 2;
    }
    return s;
}

static void ensure(void)
{
    if (s_state) return;
    x_glDrawArrays    = (PFN_DRAWARRAYS)getgl("glDrawArrays");
    x_glActiveTexture = (PFN_ACTIVETEX)getgl("glActiveTexture");
    x_glStencilFunc   = (PFN_STENCILFUNC)getgl("glStencilFunc");
    x_glStencilOp     = (PFN_STENCILOP)getgl("glStencilOp");
    x_glColorMask     = (PFN_COLORMASK)getgl("glColorMask");
    if (!x_glDrawArrays || !x_glActiveTexture ||
        !x_glStencilFunc || !x_glStencilOp || !x_glColorMask) {
        dlog("hires draw: missing GL proc"); s_state = 2; return;
    }
    GLuint vs = mksh(GL_VERTEX_SHADER, VS), fs = mksh(GL_FRAGMENT_SHADER, FS);
    /* a failed build still has to give its objects back: a display-mode change
       restarts the render thread with a new context and runs all of this
       again, so "one-shot" is only true of a context, not of a session */
    if (s_state == 2) { glDeleteShader(vs); glDeleteShader(fs); return; }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs); glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        dlog("hires draw: link FAILED");
        glDeleteShader(vs); glDeleteShader(fs);
        glDeleteProgram(s_prog);
        s_prog = 0;
        s_state = 2;
        return;
    }
    glDeleteShader(vs); glDeleteShader(fs);
#define U(n) glGetUniformLocation(s_prog, n)
    u_game = U("uGame");        u_offset = U("uOffset");
    u_zoom = U("uZoom");        u_zoomC = U("uZoomC");
    u_depthScale = U("uDepthScale");
    u_anchor = U("uAnchor");    u_yawEnc = U("uYawEnc");
    u_piece = U("uPiece");
    u_hasNrm = U("uHasNrm");    u_base = U("uBase");
    u_mr = U("uMR");            u_cutoff = U("uCutoff");
    u_shadow = U("uShadow");    u_alpha = U("uAlpha");
    u_slant = U("uSlant");
    u_waterT = U("uWaterT");    u_waterMode = U("uWaterMode");
    u_digT = U("uDigT");        u_light = U("uLight");
    u_view = U("uView");        u_sunAmb = U("uSunAmb");
    u_anchorMix = U("uAnchorMix");  u_shade = U("uShade");
    u_fog = U("uFog");          u_fogOrg = U("uFogOrg");
    u_fogDim = U("uFogDim");    u_scafOn = U("uScafOn");
    u_scafP = U("uScafP");      u_ss = U("uSS");
    u_zoomF = U("uZoomF");      u_zoomCF = U("uZoomCF");
    u_depthPass = U("uDepthPass"); u_shadowMat = U("uShadowMat"); u_cast = U("uCast");
    glUseProgram(s_prog);
    /* the shared textures keep the unit numbers tagpu_native.c bound them to,
       so its binds are still live when we run; ours take the two above them */
    glUniform1i(U("uLUT"), 1);
    glUniform1i(U("uPal"), 2);
    glUniform1i(U("uScaf"), 3);
    glUniform1i(U("uFogGrid"), 4);
    glUniform1i(U("uFogLUT"), 5);
    glUniform1i(U("uAlbedo"), 6);
    glUniform1i(U("uNormal"), 7);
#undef U
    glUseProgram(0);
    s_state = 1;
    dlog("hires draw: program ready");
}

static void tune(unsigned frame_counter)
{
    static int first = 1;
    if (!first && (frame_counter % 30) != 0) return;
    first = 0;
    /* Back to the defaults FIRST, every re-read. These are live knobs whose
       whole point is a one-second loop, and a flag that only ever latches on
       is not a knob: write `nonormal` to compare, delete it, and normal maps
       would have stayed off for the rest of the session — as would `log`,
       still writing to tagpu.log every 60 frames. Deleting the file has to
       mean the defaults too, so this happens before the early return. */
    s_anchorMix = 0.0f;
    s_sun = 2.67f;
    s_amb = 0.18f;
    s_normalMaps = 1;
    s_log = 0;
    HANDLE h = CreateFileA("tagpu_hires.on", GENERIC_READ, FILE_SHARE_READ,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[128];
    DWORD n = 0;
    if (ReadFile(h, buf, sizeof buf - 1, &n, 0) && n > 0) {
        buf[n] = 0;
        char* p = buf;
        while (p < buf + n) {
            while (*p && *p <= ' ') p++;
            if (!*p) break;
            char* q = p;
            while (*q && *q > ' ') q++;
            int last = (*q == 0);
            *q = 0;
            if (!_strnicmp(p, "anchor=", 7))    s_anchorMix = (float)atof(p + 7);
            else if (!_strnicmp(p, "sun=", 4))  s_sun = (float)atof(p + 4);
            else if (!_strnicmp(p, "amb=", 4))  s_amb = (float)atof(p + 4);
            else if (!lstrcmpiA(p, "nonormal")) s_normalMaps = 0;
            else if (!lstrcmpiA(p, "log"))      s_log = 1;
            if (last) break;
            p = q + 1;
        }
    }
    CloseHandle(h);
    if (s_anchorMix < 0.0f) s_anchorMix = 0.0f;
    if (s_anchorMix > 1.0f) s_anchorMix = 1.0f;
}

/* what a unit with no readable pose gets: every piece at its rest place */
static const float* ident_pose(void)
{
    static float I[TAGPU_HMAXPIECE * 12];
    static int built = 0;
    if (!built) {
        int i;
        built = 1;
        for (i = 0; i < TAGPU_HMAXPIECE; i++) {
            I[i*12 + 0] = 1.0f;
            I[i*12 + 5] = 1.0f;
            I[i*12 + 10] = 1.0f;
        }
    }
    return I;
}

/* Builds the program if it has not been built, so the answer is the truth and
   not "nobody has asked the driver yet" -- the caller routes units on this,
   and a replacement unit routed to a pass that cannot draw is invisible rather
   than stock. CALL ONLY WITH A CURRENT GL CONTEXT, like tagpu_hires_vao. */
int tagpu_hires_draw_ready(void) { ensure(); return s_state != 2; }

void tagpu_hires_draw(const TAGPU_HVIEW* v, const TAGPU_HUNIT* u, int n,
                      int shadowPass, unsigned frame_counter)
{
    int i, k;
    ensure();
    if (s_state != 1 || n <= 0) return;
    tune(frame_counter);

    glUseProgram(s_prog);
    glUniform2fv(u_game, 1, v->game);
    glUniform1f(u_zoom, v->zoom);
    glUniform2fv(u_zoomC, 1, v->zoomC);
    glUniform1f(u_zoomF, v->zoom);
    glUniform2fv(u_zoomCF, 1, v->zoomC);
    glUniform1f(u_depthScale, v->depthScale);
    glUniform1f(u_ss, v->ss);
    glUniform1i(u_scafOn, v->scafOn ? 1 : 0);
    glUniform4fv(u_scafP, 1, v->scafP);
    glUniform2fv(u_fogOrg, 1, v->fogOrg);
    glUniform2fv(u_fogDim, 1, v->fogDim);
    glUniform3fv(u_light, 1, SH_L);
    glUniform3fv(u_view, 1, SH_V);
    glUniform1i(u_shadow, shadowPass ? 1 : 0);
    {
        float sa[2]; sa[0] = s_sun; sa[1] = s_amb;
        glUniform2fv(u_sunAmb, 1, sa);
        glUniform1f(u_anchorMix, s_anchorMix);
        GLint sh[3];
        sh[0] = SHADE_REF; sh[1] = v->shNeutral; sh[2] = v->shDir;
        glUniform3iv(u_shade, 1, sh);
    }
    /* The shared textures this pass samples, bound from the view struct that
       carries them rather than inherited from wherever the native pass last
       left the units. It used to work only because these calls happen to sit
       after that binding block; anything inserted between — a new pass, a
       reordered one — would have shaded our units through the wrong LUT with
       nothing to show for it. Unit 0 is deliberately NOT touched: it holds the
       native atlas, and the texture uploads below would clobber it mid-frame.
       Units 6 and 7 are ours. */
    x_glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, v->lutTex);
    x_glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, v->palTex);
    x_glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, v->scafTex);
    x_glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, v->fogTex);
    x_glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, v->fogLutTex);
    x_glActiveTexture(GL_TEXTURE6);

    int drawn = 0, tris = 0;
    for (i = 0; i < n; i++) {
        const TAGPU_HUNIT* h = &u[i];
        if (shadowPass && !h->shadow) continue;
        GLuint vao = tagpu_hires_vao(h->mesh);
        if (!vao) continue;
        int ng = tagpu_hires_ngroup(h->mesh);
        glBindVertexArray(vao);
        {
            int np = tagpu_hires_npiece(h->mesh);
            if (np > TAGPU_HMAXPIECE) np = TAGPU_HMAXPIECE;
            if (np > 0)
                glUniform4fv(u_piece, np * 3,
                             (h->pose && h->npose >= np) ? h->pose : ident_pose());
        }
        {
            float anc[4]; anc[0] = h->ax; anc[1] = h->ay; anc[2] = h->wx0; anc[3] = h->wz0;
            float ang = (float)h->yaw * 6.2831853f / 65536.0f;
            float ye[3]; ye[0] = cosf(ang); ye[1] = sinf(ang); ye[2] = h->enc;
            float off[2]; off[0] = shadowPass ? 5.0f : 0.0f;
            off[1] = shadowPass ? h->shadowDy : 0.0f;
            glUniform4fv(u_anchor, 1, anc);
            glUniform3fv(u_yawEnc, 1, ye);
            glUniform2fv(u_offset, 1, off);
            glUniform1i(u_slant, (shadowPass && h->slant) ? 1 : 0);
            glUniform1f(u_alpha, shadowPass ? 0.5f : h->alpha);
            glUniform1i(u_fog, h->fog);
            /* a structure's slant shadow is never erased at the waterline:
               the engine blits its cached sprite as built (tagpu_native.c
               emit_slant); the erase is the silhouette's and the body's */
            glUniform1f(u_waterT, (shadowPass && h->slant) ? -1e9f : h->waterT);
            glUniform1f(u_digT,   (shadowPass && h->slant) ? -1e9f : h->digT);
            glUniform1i(u_waterMode, shadowPass ? 0 : h->waterMode);
        }
        /* ONE 50% BLEND PER SILHOUETTE PIXEL, exactly as tagpu_native.c does it
           for a 3DO: the engine blits one blackened copy of the composite, so a
           pixel the model covers twice is still darkened once. A replacement
           mesh is worse than a 3DO here -- it is denser and its groups overlap
           -- so without the mask a hires unit's shadow came out at 0.25 of the
           ground beside a stock unit's 0.50 in the same frame. Pass 0 marks the
           whole unit (every material group) into the stencil with colour writes
           off; pass 1 blends where the mark is with the op that ZEROES it, so a
           second fragment on that pixel fails EQUAL 1. Both passes issue the
           same draws with the same discards, so no mark is left behind, and
           clearing per unit keeps two units' shadows stacking as the engine's
           separate blits do. The caller leaves GL_STENCIL_TEST enabled for the
           shadow pass and turns it off after us. */
        int pass, npass = shadowPass ? 2 : 1;
        for (pass = 0; pass < npass; pass++) {
            if (shadowPass) {
                if (pass == 0) {
                    x_glStencilFunc(GL_ALWAYS, 1, 0xFF);
                    x_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    x_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                } else {
                    x_glStencilFunc(GL_EQUAL, 1, 0xFF);
                    x_glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                    x_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                }
            }
            for (k = 0; k < ng; k++) {
                TAGPU_HGROUP g;
                if (!tagpu_hires_group(h->mesh, k, &g)) continue;
                /* the shadow is a silhouette: it still needs the material's alpha
                   cutout, to punch the same holes, and nothing else it carries */
                glUniform4fv(u_base, 1, g.base);
                glUniform1f(u_cutoff, g.cutoff);
                glBindTexture(GL_TEXTURE_2D, g.albedo);          /* unit 6 */
                if (!shadowPass) {
                    float mr[2]; mr[0] = g.metal; mr[1] = g.rough;
                    glUniform2fv(u_mr, 1, mr);
                    glUniform1i(u_hasNrm, (s_normalMaps && g.normal) ? 1 : 0);
                    x_glActiveTexture(GL_TEXTURE7);
                    glBindTexture(GL_TEXTURE_2D, g.normal ? g.normal : g.albedo);
                    x_glActiveTexture(GL_TEXTURE6);
                }
                x_glDrawArrays(GL_TRIANGLES, g.first, g.count);
                if (pass == npass - 1) { drawn++; tris += g.count / 3; }
            }
        }
    }
    glBindVertexArray(0);
    x_glActiveTexture(GL_TEXTURE0);
    if (s_log && (frame_counter % 60) == 0) {
        char b[256];
        _snprintf(b, sizeof b,
                  "hires: %s %d unit(s) %d draw(s) %d tri  anchor=%.2f sun=%.2f "
                  "amb=%.2f nrm=%d  game=%.0fx%.0f zoom=%.2f dscale=%.1f "
                  "u0=(%.1f,%.1f) enc=%.1f fog=%d gl=0x%x",
                  shadowPass ? "shadow" : "body", n, drawn, tris,
                  s_anchorMix, s_sun, s_amb, s_normalMaps,
                  v->game[0], v->game[1], v->zoom, v->depthScale,
                  u[0].ax, u[0].ay, u[0].enc, u[0].fog, (unsigned)glGetError());
        dlog(b);
    }
}

/* Classic++ shadows: the same program, the same pose upload and per-material
   draws as the body, the vertex shader's uDepthPass branch putting the posed
   point through the light matrix and the fragment shader stopping after the
   alpha cutout. 2-3 k depth-only triangles per unit: a few microseconds. */
void tagpu_hires_depth(const TAGPU_HVIEW* v, const TAGPU_HUNIT* u, int n,
                       const float* shadowMat)
{
    int i, k;
    (void)v;
    ensure();
    if (s_state != 1 || n <= 0) return;
    glUseProgram(s_prog);
    glUniform1i(u_depthPass, 1);
    glUniformMatrix4fv(u_shadowMat, 1, GL_FALSE, shadowMat);
    glUniform1i(u_slant, 0);
    glUniform1i(u_shadow, 0);
    x_glActiveTexture(GL_TEXTURE6);
    for (i = 0; i < n; i++) {
        const TAGPU_HUNIT* h = &u[i];
        GLuint vao;
        int ng;
        if (h->castSkip) continue;
        vao = tagpu_hires_vao(h->mesh);
        if (!vao) continue;
        ng = tagpu_hires_ngroup(h->mesh);
        glBindVertexArray(vao);
        {
            int np = tagpu_hires_npiece(h->mesh);
            if (np > TAGPU_HMAXPIECE) np = TAGPU_HMAXPIECE;
            if (np > 0)
                glUniform4fv(u_piece, np * 3,
                             (h->pose && h->npose >= np) ? h->pose : ident_pose());
        }
        {
            float anc[4]; anc[0] = h->ax; anc[1] = h->ay; anc[2] = h->wx0; anc[3] = h->wz0;
            float ang = (float)h->yaw * 6.2831853f / 65536.0f;
            float ye[3]; ye[0] = cosf(ang); ye[1] = sinf(ang); ye[2] = h->enc;
            glUniform4fv(u_anchor, 1, anc);
            glUniform3fv(u_yawEnc, 1, ye);
            glUniform3fv(u_cast, 1, h->cast);
        }
        for (k = 0; k < ng; k++) {
            TAGPU_HGROUP g;
            if (!tagpu_hires_group(h->mesh, k, &g)) continue;
            /* the material's alpha cutout punches the same holes in the map */
            glUniform4fv(u_base, 1, g.base);
            glUniform1f(u_cutoff, g.cutoff);
            glBindTexture(GL_TEXTURE_2D, g.albedo);          /* unit 6 */
            x_glDrawArrays(GL_TRIANGLES, g.first, g.count);
        }
    }
    glUniform1i(u_depthPass, 0);
    glBindVertexArray(0);
    x_glActiveTexture(GL_TEXTURE0);
}

void tagpu_hires_draw_glreset(void)
{
    s_state = 0;
    s_prog = 0;
    tagpu_hires_glreset();
}
