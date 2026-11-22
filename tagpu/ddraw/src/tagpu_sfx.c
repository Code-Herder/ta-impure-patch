/* tagpu_sfx.c — the particle pass: smoke, fire, the teleport flare, wakes and bubbles, the
   nanolathe spray.

   WHAT THE ENGINE DOES. It keeps ten "layer" vectors at *(main+0x38D77), each emitter appending
   its object to the layer it is given, and DrawGameScreen calls 0x471F90(ctx, n) for n = 0..9 at
   ten fixed points of the frame (terrain-depth.md §3): 0/1/2 before the flat-feature pre-pass,
   3/4 before the row sweep, 5/6 after it (before projectiles), 7 after explosions, 8 after
   aircraft, 9 before the fog overlay — so THE LAYER NUMBER IS THE DRAW DEPTH.

   WHAT A VULKAN LAUNCH DOES. The engine holds no particle object: every emission is an emitter
   event on the game thread (tagpu_patches.c, *The particles as events*), the frame packet carries
   the live ones that can reach the view, and the effects pass computes every particle from its
   event and the packet's GameTime on the GPU (tagpu_fx.c NVS, tagpu_vk_fx.c), layers 0..6 before
   the projectiles and 7..9 after the sprites, each at its layer's depth key. This file hands the
   packet's events to that pass once a frame, under the levers.

   Armed by tagpu_sfx.on (tokens log, passive, nosmoke, nofire, nowake, nonano): `nonano` draws no
   nano event, `nofire` no fire or flare event, `nowake` no wake or bubbles event, `nosmoke` no
   smoke event, `passive` none. Read-only over sim. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_sfx.h"
#include "tagpu_packet.h"
#include "tagpu_opt.h"
#include "tagpu_log.h"

static void flog(const char* s)
{
    tagpu_log(s);
}

/* ---- arming ---- */
static int  s_armed = -1;
static int  s_log = 0, s_passive = 0;
static int  s_smoke = 1, s_fire = 1, s_wake = 1, s_nano = 1;
static unsigned s_armCheck = 0;

int tagpu_sfx_armed(unsigned frame_counter)
{
    if (s_armed >= 0 && frame_counter - s_armCheck < 30) return s_armed > 0;
    s_armCheck = frame_counter;
    int was = s_armed;
    s_armed = 0;
    char buf[128];
    int n = tagpu_opt_read("tagpu_sfx.on", buf, sizeof buf);
    if (n < 0) {
        if (was > 0) flog("sfx: disarmed");
        return 0;
    }
    s_log = 0; s_passive = 0; s_smoke = s_fire = s_wake = s_nano = 1;
    if (n > 0) {
        buf[n] = 0;
        char* p = buf;
        while (*p) {
            while (*p && *p <= ' ') p++;
            char* q = p;
            while (*q && *q > ' ') q++;
            int last = (*q == 0);
            *q = 0;
            if (!lstrcmpiA(p, "log")) s_log = 1;
            else if (!lstrcmpiA(p, "passive")) s_passive = 1;
            else if (!lstrcmpiA(p, "nosmoke")) s_smoke = 0;
            else if (!lstrcmpiA(p, "nofire")) s_fire = 0;
            else if (!lstrcmpiA(p, "nowake")) s_wake = 0;
            else if (!lstrcmpiA(p, "nonano")) s_nano = 0;
            if (last) break;
            p = q + 1;
        }
    }
    s_armed = 1;
    if (was != 1) {
        char b[128];
        _snprintf(b, sizeof b, "sfx: ARMED (smoke=%d fire=%d wake=%d nano=%d log=%d passive=%d)",
                  s_smoke, s_fire, s_wake, s_nano, s_log, s_passive);
        flog(b);
    }
    return 1;
}

int tagpu_sfx_on(void) { return s_armed > 0; }

/* ---- per-frame counters ---- */
static int s_nEvt;

/* THE PARTICLE EVENTS, once a frame */
void tagpu_sfx_gather(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    if (s_armed != 1) return;
    s_nEvt += (int)pk->n_pevt;
    tagpu_fx_set_events(tagpu_pk_pevt(pk), (int)pk->n_pevt, pk->tick, pk->pevt_frame,
                        pk->pevt_nframe, tagpu_pk_pwind(pk), (int)pk->n_pwind, pk->pwind_base,
                        s_passive ? 0u : (s_nano ? 1u : 0u) | (s_fire ? 2u : 0u) |
                                         (s_wake ? 4u : 0u) | (s_smoke ? 8u : 0u));
}

void tagpu_sfx_frame_done(const TAGPU_FXVIEW* v)
{
    const TAGPU_PACKET* pk = v->packet;
    static unsigned last = 0;
    if (s_armed != 1) return;
    if (s_log && v->frame_counter - last >= 60) {
        char b[160];
        last = v->frame_counter;
        _snprintf(b, sizeof b, "sfx: events=%d (%u wind rows)%s%s", s_nEvt, pk->n_pwind,
                  (pk->truncated & TAGPU_PK_TRUNC_PEVT) ? " EVENTS-TRUNCATED" : "",
                  s_passive ? " (passive: nothing drawn)" : "");
        b[sizeof b - 1] = 0;
        flog(b);
    }
    s_nEvt = 0;
}
