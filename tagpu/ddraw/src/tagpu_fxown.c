/* tagpu_fxown.c — own the effects draw (weapon fire, explosions, debris).

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
               sites, pure draw: the sim tick updates them elsewhere);
               stdcall, ret 8. Its own skip byte follows tagpu_sfx.on.

   The projectile pass is pure draw (its only side effect is the CRT rand()
   jitter of lightning bolts, not the sim RNG), so its call site is redirected
   to a stub that returns at once while the skip is on. The explosion pass
   must still run: it updates the debris particle systems (0x421550 emits
   their smoke and fire), so its call site is redirected to a stub that
   brackets the real call with an "inside" flag; the leaves above check the
   skip flag (0x46BAE0 / 0x4B8EC0 / 0x4211D0) or the inside flag
   (CopyGafToContext) and unwind exactly like the callee would.

   Two byte-matched call-site patches and four 5/6-byte prologue detours,
   installed once at DllMain if tagpu_fxown.on exists (all-or-nothing). The
   skip flag itself is a byte written by tagpu_fx.c each time it re-reads
   tagpu_fx.on, so an A/B (engine draws vs ours) is a file flip, no relaunch.
   Read-only over sim. Engine-only losses while skipping: the rendertype-2
   background-refraction "ball" sprite (not reproduced natively). */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_fxown.h"

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

volatile unsigned char g_fxown_skip = 0;     /* engine effects draw skipped */
volatile unsigned char g_fxown_in   = 0;     /* inside the explosion pass   */
volatile unsigned char g_fxown_skipSfx = 0;  /* particle layer draw skipped */
static int g_installed = 0;
static unsigned g_last = 0;

static void flog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

static unsigned char* stub_alloc(void)
{
    return (unsigned char*)VirtualAlloc(NULL, 0x80, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
}

static void put_rel(unsigned char* p, unsigned int target)
{
    int32_t rel = (int32_t)(target - ((unsigned int)p + 4));
    memcpy(p, &rel, 4);
}

/* 80 3D <flag> 00 ; cmp byte [flag],0   (7 bytes) */
static unsigned char* emit_cmp_flag(unsigned char* p, volatile unsigned char* flag)
{
    unsigned int a = (unsigned int)(size_t)flag;
    *p++ = 0x80; *p++ = 0x3D; memcpy(p, &a, 4); p += 4; *p++ = 0x00;
    return p;
}

/* C6 05 <flag> <v> ; mov byte [flag],v   (7 bytes) */
static unsigned char* emit_set_flag(unsigned char* p, volatile unsigned char* flag, unsigned char v)
{
    unsigned int a = (unsigned int)(size_t)flag;
    *p++ = 0xC6; *p++ = 0x05; memcpy(p, &a, 4); p += 4; *p++ = v;
    return p;
}

static int patch_bytes(unsigned int va, const unsigned char* bytes, int n)
{
    DWORD old;
    if (!VirtualProtect((void*)(size_t)va, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)(size_t)va, bytes, (size_t)n);
    VirtualProtect((void*)(size_t)va, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(size_t)va, (SIZE_T)n);
    return 1;
}

/* call-site -> stub: skip (ret 4) while g_fxown_skip, else jmp the pass */
static int install_site_proj(void)
{
    unsigned char* s = stub_alloc(); unsigned char* p = s;
    unsigned char call5[5];
    if (!s) return 0;
    p = emit_cmp_flag(p, &g_fxown_skip);
    *p++ = 0x74; *p++ = 0x03;                       /* jz +3            */
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;          /* ret 4            */
    *p++ = 0xE9; put_rel(p, PASS_PROJ_VA); p += 4;  /* jmp 0x49BE60     */
    call5[0] = 0xE8;
    { int32_t rel = (int32_t)((unsigned int)s - (SITE_PROJ_VA + 5)); memcpy(call5 + 1, &rel, 4); }
    return patch_bytes(SITE_PROJ_VA, call5, 5);
}

/* call-site -> stub: bracket the real pass with g_fxown_in = g_fxown_skip
   (so the shared CopyGafToContext leaf skips only while the skip is on;
   eax is caller-saved at a call site, so `mov al` is free) */
static int install_site_expl(void)
{
    unsigned char* s = stub_alloc(); unsigned char* p = s;
    unsigned char call5[5];
    unsigned int a;
    if (!s) return 0;
    a = (unsigned int)(size_t)&g_fxown_skip;
    *p++ = 0xA0; memcpy(p, &a, 4); p += 4;                /* mov al,[skip] */
    a = (unsigned int)(size_t)&g_fxown_in;
    *p++ = 0xA2; memcpy(p, &a, 4); p += 4;                /* mov [in],al   */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;   /* push [esp+4] */
    *p++ = 0xE8; put_rel(p, PASS_EXPL_VA); p += 4;        /* call 0x420B00 (ret 4) */
    p = emit_set_flag(p, &g_fxown_in, 0);
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                /* ret 4        */
    call5[0] = 0xE8;
    { int32_t rel = (int32_t)((unsigned int)s - (SITE_EXPL_VA + 5)); memcpy(call5 + 1, &rel, 4); }
    return patch_bytes(SITE_EXPL_VA, call5, 5);
}

/* prologue detour: skip (ret n) while *flag, else stolen bytes + resume */
static int install_leaf(unsigned int va, const unsigned char* stolen, int nst,
                        volatile unsigned char* flag, unsigned char retn)
{
    unsigned char* s = stub_alloc(); unsigned char* p = s;
    unsigned char jmp5[5];
    if (!s) return 0;
    p = emit_cmp_flag(p, flag);
    *p++ = 0x74; *p++ = 0x03;                       /* jz +3            */
    *p++ = 0xC2; *p++ = retn; *p++ = 0x00;          /* ret n            */
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; put_rel(p, va + (unsigned)nst); p += 4;
    jmp5[0] = 0xE9;
    { int32_t rel = (int32_t)((unsigned int)s - (va + 5)); memcpy(jmp5 + 1, &rel, 4); }
    if (!patch_bytes(va, jmp5, 5)) return 0;
    if (nst > 5) {                                  /* pad the tail of a 6-byte steal */
        unsigned char nop = 0x90;
        patch_bytes(va + 5, &nop, 1);
    }
    return 1;
}

void tagpu_fxown_init(void)
{
    char b[200];
    if (GetFileAttributesA("tagpu_fxown.on") == INVALID_FILE_ATTRIBUTES) return;
    int ok = memcmp((void*)SITE_PROJ_VA, SITE_PROJ_BYTES, 5) == 0 &&
             memcmp((void*)SITE_EXPL_VA, SITE_EXPL_BYTES, 5) == 0 &&
             memcmp((void*)LEAF_MODEL_VA, MODEL_STOLEN, 5) == 0 &&
             memcmp((void*)LEAF_FLASH_VA, FLASH_STOLEN, 6) == 0 &&
             memcmp((void*)LEAF_PIECE_VA, PIECE_STOLEN, 5) == 0 &&
             memcmp((void*)LEAF_COPY_VA,  COPY_STOLEN,  6) == 0 &&
             memcmp((void*)LEAF_SFX_VA,   SFX_STOLEN,   5) == 0;
    if (!ok) { flog("fxown: NOT armed — engine bytes differ at a patch site"); return; }
    int a = install_site_proj();
    int c = install_site_expl();
    int d = install_leaf(LEAF_MODEL_VA, MODEL_STOLEN, 5, &g_fxown_skip, 0x10);
    int e = install_leaf(LEAF_FLASH_VA, FLASH_STOLEN, 6, &g_fxown_skip, 0x10);
    int f = install_leaf(LEAF_PIECE_VA, PIECE_STOLEN, 5, &g_fxown_skip, 0x0C);
    int g = install_leaf(LEAF_COPY_VA,  COPY_STOLEN,  6, &g_fxown_in,   0x10);
    int h = install_leaf(LEAF_SFX_VA,   SFX_STOLEN,   5, &g_fxown_skipSfx, 0x08);
    g_installed = a && c && d && e && f && g && h;
    _snprintf(b, sizeof b,
        "fxown: %s site.proj=%d site.expl=%d model@0x46BAE0=%d flash@0x4B8EC0=%d piece@0x4211D0=%d copy@0x4B7F90=%d sfx@0x471F90=%d (skips follow tagpu_fx.on / tagpu_sfx.on)",
        g_installed ? "ARMED" : "PARTIAL", a, c, d, e, f, g, h);
    flog(b);
}

void tagpu_fxown_set_skip(int on)
{
    unsigned char v = (unsigned char)(on && g_installed);
    if (v != g_fxown_skip) {
        g_fxown_skip = v;
        flog(v ? "fxown: engine effects draw SKIPPED (ours live)" : "fxown: engine effects draw restored");
    }
}

/* the native effects pass reports each frame it actually gathered; if it
   stops (overlay off, GL failure, a frame path that never reaches it) the
   engine's draw comes back after 90 present frames instead of vanishing */
static unsigned g_beat = 0, g_beatSfx = 0;
void tagpu_fxown_beat(unsigned int frame_counter) { g_beat = frame_counter; }
void tagpu_fxown_beat_sfx(unsigned int frame_counter) { g_beatSfx = frame_counter; }

void tagpu_fxown_set_skip_sfx(int on)
{
    unsigned char v = (unsigned char)(on && g_installed);
    if (v != g_fxown_skipSfx) {
        g_fxown_skipSfx = v;
        flog(v ? "fxown: engine particle draw SKIPPED (ours live)" : "fxown: engine particle draw restored");
    }
}

int tagpu_fxown_installed(void) { return g_installed; }

void tagpu_fxown_flush(unsigned int frame_counter)
{
    if (!g_installed) return;
    if (g_fxown_skip && frame_counter - g_beat > 90) {
        flog("fxown: effects pass silent for 90 frames");
        tagpu_fxown_set_skip(0);
    }
    if (g_fxown_skipSfx && frame_counter - g_beatSfx > 90) {
        flog("fxown: particle pass silent for 90 frames");
        tagpu_fxown_set_skip_sfx(0);
    }
    if (frame_counter - g_last >= 300) {
        char b[96];
        g_last = frame_counter;
        _snprintf(b, sizeof b, "FXOWN skip=%u sfx=%u in=%u", (unsigned)g_fxown_skip,
                  (unsigned)g_fxown_skipSfx, (unsigned)g_fxown_in);
        flog(b);
    }
}
