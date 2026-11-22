/* tagpu_fxown.c — the engine draws no effect on a Vulkan launch (weapon fire,
   explosions, debris, the particle layers).

   The engine draws effects in two passes called from DrawGameScreen
   (research/notes/effects.md), and every pixel they emit goes through a
   handful of leaves whose only callers are those passes — except one:

     0x469B22  call 0x49BE60  projectile pass   (stdcall(ctx), ret 4)
     0x469B2C  call 0x420B00  explosion pass    (stdcall(ctx), ret 4)
     0x46BAE0  generic 3DO draw (projectile models, debris) — callers: the
               two passes only; thiscall-shaped, 4 args, ret 0x10
     0x4B8EC0  LHT flash blit — caller: explosion pass (+ itself); ret 0x10
     0x4211D0  flying-debris piece draw — caller: 0x421550 (particle update
               inside the explosion pass); ret 0xC
     0x4B7F90  CopyGafToContext — the explosion SPRITE blit, but shared with
               63 other callers (features, UI): skipped only while the
               explosion pass is running.
     0x471F90  particle-layer draw walker (ctx, n) — the ONLY draw path of
               the smoke/fire/wake/nanolathe objects (ten DrawGameScreen
               sites, pure draw: the sim tick updates them through vtbl+4,
               this walker calls vtbl+8); stdcall, ret 8. Without
               tagpu_particlecheck.on the layers are empty on Vulkan (every
               particle is an event, tagpu_patches.c); under it the engine's
               objects are made in full, and this keeps them off the screen.

   WHY NOTHING IS LOST. The engine's world draws never reach the presented
   frame on the Vulkan backend: the world phase stamps them and `publish`
   refuses them (tagpu_gui_hook.c), so they land only in the golden source
   `tacli shot` reads. Every effect on screen is the Vulkan effects and
   particle passes'. What these patches remove is the CPU the engine spends
   rasterising them, and the golden source's copy of them; the picture with
   every engine draw in it is GDI's, the tagpu_gdi.on oracle, where nothing
   here installs. A launch is Vulkan or GDI for its life (g_config.gdi is
   fixed by cfg_load before DllMain reaches this, and the Vulkan lane never
   hands a session to GDI), so the patches install at attach or never.

   WHAT THE SIMULATION SEES. The projectile pass is pure draw but for the
   CRT's rand 0x4E4870, which it draws from; the meteor tick reads that stream
   too, and the draw path already draws from it a varying number of times a
   tick, so its strike points depend on the frames drawn between ticks with or
   without this pass (engine/sim.md, *The meteor shower and the CRT rand*:
   a timing dependence, not a write; a puppet replays the recorded shower).
   The simulation RNG 0x4B6C30 is not drawn here. The explosion pass must
   still run: it updates the debris particle systems (0x421550 emits their
   smoke and fire), so its call site is redirected to a stub that brackets
   the real call with an "inside" flag; the leaves above return at once, and
   CopyGafToContext returns at once only while that flag is up, each unwinding
   exactly like the callee would.

   Two byte-matched call-site patches and five prologue detours, all-or-nothing
   in the byte checks. Engine-only losses: the rendertype-2 background-refraction
   "ball" sprite (not reproduced natively). */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_fxown.h"
#include "tagpu_detour.h"
#include "config.h"         /* g_config.gdi: the renderer chosen at attach */
#include "tagpu_log.h"

#define SITE_PROJ_VA   0x00469B22u
#define SITE_EXPL_VA   0x00469B2Cu
#define PASS_PROJ_VA   0x0049BE60u
#define PASS_EXPL_VA   0x00420B00u
#define LEAF_MODEL_VA  0x0046BAE0u   /* ret 0x10 */
#define LEAF_FLASH_VA  0x004B8EC0u   /* ret 0x10 */
#define LEAF_PIECE_VA  0x004211D0u   /* ret 0x0C */
#define LEAF_COPY_VA   0x004B7F90u   /* ret 0x10 */
#define LEAF_SFX_VA    0x00471F90u   /* ret 8    */

static const unsigned char SITE_PROJ_BYTES[5] = { 0xE8, 0x39, 0x23, 0x03, 0x00 };
static const unsigned char SITE_EXPL_BYTES[5] = { 0xE8, 0xCF, 0x6F, 0xFB, 0xFF };
static const unsigned char MODEL_STOLEN[5]    = { 0x51, 0x8B, 0x44, 0x24, 0x10 };
static const unsigned char FLASH_STOLEN[6]    = { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
static const unsigned char PIECE_STOLEN[5]    = { 0xB8, 0x58, 0x3F, 0x00, 0x00 };
static const unsigned char COPY_STOLEN[6]     = { 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00 };
static const unsigned char SFX_STOLEN[5]      = { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }; /* mov eax,[0x511DE8] */

/* THE BYTE THE STUBS TEST, AND IT IS NEVER WRITTEN: tagpu_detour's leaf stub
   is `cmp byte [flag],0; jz resume; ret N`, so a flag that is always 1 makes
   each leaf return at once. */
static volatile unsigned char s_removed = 1;
static volatile unsigned char g_fxown_in = 0;    /* inside the explosion pass */

static void flog(const char* s)
{
    tagpu_log(s);
}

/* the stub/patch helpers are shared with tagpu_featown.c (tagpu_detour.c) */

/* call-site -> stub: ret 4 (the pass's own unwind) while s_removed */
static int install_site_proj(void)
{
    unsigned char* s = tagpu_detour_stub(); unsigned char* p = s;
    unsigned char call5[5];
    if (!s) return 0;
    p = tagpu_detour_cmp_flag(p, &s_removed);
    *p++ = 0x74; *p++ = 0x03;                       /* jz +3            */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;          /* ret 4            */
    *p++ = 0xE9; tagpu_detour_rel(p, PASS_PROJ_VA); p += 4;  /* jmp 0x49BE60 */
    call5[0] = 0xE8;
    { int32_t rel = (int32_t)((unsigned int)(size_t)s - (SITE_PROJ_VA + 5)); memcpy(call5 + 1, &rel, 4); }
    return tagpu_detour_write(SITE_PROJ_VA, call5, 5);
}

/* call-site -> stub: bracket the real pass with g_fxown_in = s_removed, so the
   shared CopyGafToContext leaf returns at once only inside it (eax is
   caller-saved at a call site, so `mov al` is free) */
static int install_site_expl(void)
{
    unsigned char* s = tagpu_detour_stub(); unsigned char* p = s;
    unsigned char call5[5];
    unsigned int a;
    if (!s) return 0;
    a = (unsigned int)(size_t)&s_removed;
    *p++ = 0xA0; memcpy(p, &a, 4); p += 4;                /* mov al,[removed] */
    a = (unsigned int)(size_t)&g_fxown_in;
    *p++ = 0xA2; memcpy(p, &a, 4); p += 4;                /* mov [in],al   */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;   /* push [esp+4] */
    *p++ = 0xE8; tagpu_detour_rel(p, PASS_EXPL_VA); p += 4; /* call 0x420B00 (ret 4) */
    p = tagpu_detour_set_flag(p, &g_fxown_in, 0);
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                /* ret 4        */
    call5[0] = 0xE8;
    { int32_t rel = (int32_t)((unsigned int)(size_t)s - (SITE_EXPL_VA + 5)); memcpy(call5 + 1, &rel, 4); }
    return tagpu_detour_write(SITE_EXPL_VA, call5, 5);
}

void tagpu_fxown_init(void)
{
    char b[256];
    int installed;
    if (g_config.gdi) return;
    int ok = memcmp((void*)SITE_PROJ_VA, SITE_PROJ_BYTES, 5) == 0 &&
             memcmp((void*)SITE_EXPL_VA, SITE_EXPL_BYTES, 5) == 0 &&
             memcmp((void*)LEAF_MODEL_VA, MODEL_STOLEN, 5) == 0 &&
             memcmp((void*)LEAF_FLASH_VA, FLASH_STOLEN, 6) == 0 &&
             memcmp((void*)LEAF_PIECE_VA, PIECE_STOLEN, 5) == 0 &&
             memcmp((void*)LEAF_COPY_VA,  COPY_STOLEN,  6) == 0 &&
             memcmp((void*)LEAF_SFX_VA,   SFX_STOLEN,   5) == 0;
    if (!ok) { flog("fxown: the engine's effects draw is KEPT — engine bytes differ at a patch site"); return; }
    int a = install_site_proj();
    int c = install_site_expl();
    int d = tagpu_detour_leaf(LEAF_MODEL_VA, MODEL_STOLEN, 5, &s_removed,  0x10);
    int e = tagpu_detour_leaf(LEAF_FLASH_VA, FLASH_STOLEN, 6, &s_removed,  0x10);
    int f = tagpu_detour_leaf(LEAF_PIECE_VA, PIECE_STOLEN, 5, &s_removed,  0x0C);
    int g = tagpu_detour_leaf(LEAF_COPY_VA,  COPY_STOLEN,  6, &g_fxown_in, 0x10);
    int h = tagpu_detour_leaf(LEAF_SFX_VA,   SFX_STOLEN,   5, &s_removed,  0x08);
    installed = a && c && d && e && f && g && h;
    _snprintf(b, sizeof b,
        "fxown: the engine's effects draw is %s (Vulkan) site.proj=%d site.expl=%d model@0x46BAE0=%d flash@0x4B8EC0=%d piece@0x4211D0=%d copy@0x4B7F90=%d sfx@0x471F90=%d",
        installed ? "removed" : "PARTLY removed", a, c, d, e, f, g, h);
    flog(b);
}

/* the render thread's standing request for the packet's effect tables. Written
   on the render thread, read on the game thread, one writer, no ordering owed:
   a frame either side of the change costs one frame of empty or unused tables
   and nothing else — the publisher's watchdog below is what makes a pass that
   stops paying stop being paid for. They are not tied to the patches above:
   the tables are ours and the passes draw from them whether or not the
   engine's own draw could be removed. */
static volatile unsigned char g_wantFx, g_wantSfx;
static unsigned g_beatWant;
void tagpu_fxown_set_want(int fx, int sfx, unsigned int frame_counter)
{
    g_wantFx = (unsigned char)(fx != 0);
    g_wantSfx = (unsigned char)(sfx != 0);
    if (fx || sfx) g_beatWant = frame_counter;
}
int tagpu_fxown_want_fx(void)  { return g_wantFx != 0; }
int tagpu_fxown_want_sfx(void) { return g_wantSfx != 0; }

void tagpu_fxown_flush(unsigned int frame_counter)
{
    /* the effect tables cost the publisher a walk of four engine arrays; a
       pass that has stopped asking for them stops being charged for them */
    if ((g_wantFx || g_wantSfx) && frame_counter - g_beatWant > 90) {
        flog("fxown: neither effects pass asked for the packet's tables for 90 frames — they go idle");
        tagpu_fxown_set_want(0, 0, frame_counter);
    }
}
