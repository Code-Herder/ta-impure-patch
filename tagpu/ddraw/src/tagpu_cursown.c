/* tagpu_cursown.c — the engine's cursor blit, skipped while ours is on screen.
   Contract, and the argument for patching a call site rather than a function:
   tagpu_cursown.h and tagpu_detour.c's tagpu_detour_call_site. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_cursown.h"
#include "tagpu_detour.h"
#include "tagpu_opt.h"

/* THE FOUR SITES THAT BLIT THE CURSOR, and nothing else in the image does.
   `CopyGafToContext 0x4B7F90` is the engine's generic GAF blit with hundreds of
   callers; these are the two inside a cursor draw, so patching the SITES takes
   the cursor's pixels and leaves every other sprite in the game alone.
   ALL FOUR ARE PATCHED, and that is the point. The previous attempt at this
   picked the one site it believed drew in play, from a "fingerprint" that
   turned out not to exist (both paths subtract the sprite's hotspot; the note
   that said otherwise was wrong, see exe-reverse-engineering.md). Which of the
   four the engine selects depends on +0x1CE, on +0x1D2, on a counter at +0x1AE
   and on whether the mouse thread is up — so a design that has to KNOW is a
   design with a wrong answer in it. Covering every blit makes the question
   irrelevant: whichever arm runs, its sprite is the one thing skipped.

   [VERIFIED by disassembly of the pristine exe, 2026-09-14 — each function
    contains exactly one `call 0x4B7F90`:
      0x4C687D  in 0x4C67C0, the flip's draw. The last instruction before its
                epilogue: AFTER the +0x1B6/+0x1BA writes (0x4C683C/0x4C684E)
                and AFTER the background save (0x4C6862).
      0x4C2732  in 0x4C25E0, the body of the engine's MOUSE THREAD (started at
                0x4C2A9A by _beginthread with 0x4C2990 as its entry). Blits
                into the private context [obj+0x1C6].
      0x4C297E  in 0x4C2870, whose save is at 0x4C2937. Its first gate IS
                `cmp [+0x1CE], 1` (0x4C287D loads edi = 1, 0x4C2882 compares
                against it, 0x4C2888 exits when equal), so while the mouse
                thread is up — +0x1CE == 1 — this path early-outs and draws
                nothing, whatever its in-play callers do. Patched for the case
                it is NOT inert in: the mouse thread down, +0x1CE == 0, where
                this becomes a live draw path. Its second gate is a decrement
                of the counter at +0x1AE.
      0x4C258C  in 0x4C24B0, which has no call site we can find. Patched
                anyway: a byte-matched 5-byte patch on a function that never
                runs costs one page write at startup, and "no caller in the
                image" is exactly what was also true of 0x4C2990, which turned
                out to be a thread entry.
    All four encode `E8 rel32` reaching 0x4B7F90, stdcall `ret 0x10` (epilogue
    0x4B8144), so the skip path pops four dwords.] */
#define SITE_FLIP_VA   0x004C687Du
#define SITE_POLL_VA   0x004C2732u
#define SITE_HIDE_VA   0x004C297Eu
#define SITE_DEAD_VA   0x004C258Cu
#define COPYGAF_VA     0x004B7F90u
#define COPYGAF_ARGS   0x10

/* THE FLAG. Written by tagpu_cursown_publish on the render thread, read by the
   two stubs on the GAME thread (the flip) and on the engine's own MOUSE thread
   (0x4C25E0 is the body of the thread started at 0x4C2A9A — `_beginthread`
   with 0x4C2990 as its entry, which is why no `call 0x4C2990` exists in the
   image and why a caller search called it dead). A single byte: the compiler
   must emit one `mov byte` for the store, the stubs are a hand-written
   `cmp byte [abs],0`, and a byte store is atomic and store-ordered on x86, so
   a reader sees the old value or the new one and never a tear. Nothing here
   needs more: the worst a one-frame-stale read can do is draw the engine's
   cursor for a frame we also drew ours, or skip one we did not — and the first
   is what the composite's rect still covers. */
static volatile unsigned char s_skip = 0;
/* FRAMES OUR CURSOR WAS RECORDED BUT NOTHING COMPOSITED -- the frames gate 4's
   last landing changed. See `tagpu_cursown_note_held`. */
static unsigned s_held = 0;

static int s_flipArmed = 0;
static int s_pollArmed = 0;
static int s_restArmed = 0;   /* 0x4C297E + 0x4C258C: the two arms nothing was seen to take */
static int s_installed = 0;

static void cowlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

void tagpu_cursown_init(void)
{
    char b[420], tok[128];
    int wantFlip = 1, wantPoll = 1;
    /* OFF IS A REAL ANSWER: with the module unarmed the engine draws its own
       cursor exactly as it always did and the GL UI layer's rect exemption is
       the fallback it was before any of this — that is the A/B control for
       every claim below, and `nocursor` in tagpu_gui.on is the other half. */
    if (tagpu_opt_on("tagpu_cursown.off")) {
        cowlog("cursown: OFF (tagpu_cursown.off) — the engine draws its own cursor");
        return;
    }
    /* the A/B lever: `flip` or `poll` alone, to attribute a cursor on screen to
       one site. Empty (or absent) is both, which is the shipping arm. */
    if (tagpu_opt_read("tagpu_cursown.on", tok, sizeof tok) > 0) {
        int wFlip = strstr(tok, "flip") != NULL, wPoll = strstr(tok, "poll") != NULL;
        /* naming BOTH is the same as naming neither — it must not clear both
           wants and leave the log claiming "one site only" */
        if (wFlip != wPoll) { wantFlip = wFlip; wantPoll = wPoll; }
    }
    if (wantFlip)
        s_flipArmed = tagpu_detour_call_site(SITE_FLIP_VA, COPYGAF_VA, &s_skip, COPYGAF_ARGS);
    if (wantPoll)
        s_pollArmed = tagpu_detour_call_site(SITE_POLL_VA, COPYGAF_VA, &s_skip, COPYGAF_ARGS);
    /* the two arms nothing was ever seen to take. NOT nested under either
       lever: `flip` and `poll` name the two sites anyone has a reason to A/B,
       and hiding a third site behind one of them would silently disarm
       0x4C297E — which has in-play callers — while the log said "one site
       only". Each is independent, so one that does not byte-match arms nothing
       and leaves the others alone. [FROM REVIEW 2026-09-14.] */
    s_restArmed = tagpu_detour_call_site(SITE_HIDE_VA, COPYGAF_VA, &s_skip, COPYGAF_ARGS)
                + tagpu_detour_call_site(SITE_DEAD_VA, COPYGAF_VA, &s_skip, COPYGAF_ARGS);
    s_installed = s_flipArmed || s_pollArmed || s_restArmed;
    _snprintf(b, sizeof b,
              "cursown: armed %d of 4 (flip 0x4C687D=%d poll 0x4C2732=%d rest=%d/2)%s — the engine's cursor BLIT is skipped while "
              "ours is on screen; its position words, its background save and its caller's restore all still "
              "run, so nothing of the engine's own bookkeeping changes%s",
              s_flipArmed + s_pollArmed + s_restArmed, s_flipArmed, s_pollArmed, s_restArmed,
              (wantFlip && wantPoll) ? "" : " (A/B: one site only, tagpu_cursown.on)",
              s_installed ? "" : " — NOT ARMED: no site matched, the engine keeps drawing its cursor and the layer's rect exemption is the fallback");
    b[sizeof b - 1] = 0;
    cowlog(b);
}

void tagpu_cursown_publish(int oursDrawn)
{
    /* Nothing installed: never claim the engine is suppressed. A stale 1 here
       would be read by nobody, but the heartbeat reports this word. */
    s_skip = (unsigned char)((s_installed && oursDrawn) ? 1 : 0);
}

void tagpu_cursown_stats(int* armed, int* ofN, int* skipping, unsigned* held)
{
    if (armed)    *armed    = s_flipArmed + s_pollArmed + s_restArmed;
    if (ofN)      *ofN      = 4;
    if (skipping) *skipping = s_skip ? 1 : 0;
    if (held)     *held     = s_held;
}

/* THE FRAMES THIS COUNTER EXISTS FOR ARE THE ONES THE BUG WAS. Called by the
   render loop, which is the only place that knows both halves of the answer:
   our cursor reached the mirror record AND the frame it was for composited
   nothing. Before gate 4's last landing those frames published 1 -- the
   engine's cursor suppressed with nothing of ours to replace it, which is no
   cursor at all. They publish 0 now, and this is how many times that has
   happened, so the fix is a number an operator can read rather than a claim.
   [The vulkan-only plan, gate 4's last item.] */
void tagpu_cursown_note_held(void) { s_held++; }
