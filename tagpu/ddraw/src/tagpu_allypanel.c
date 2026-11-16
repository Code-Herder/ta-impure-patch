/* The unit panel shows an ally's unit as the player's own.

   The panel 0x46A860 (stdcall(ctx), ret 4; one caller, DrawGameScreen's 0x469615, every frame, on
   the game thread) describes the HOVERED unit main+0x2CBA. Five of its tests compare an owner with
   the VIEWED player, byte main+0x2A43, and show more for the viewer's own units than for anyone
   else's [DISASSEMBLED 2026-11-15]:

     0x46AC85  the unit's +0xFF: its order target (0x439DD0) goes into the memo, so the target's
               name and health draw lower down
     0x46B013  the owner record's +0x146: the health bar even when the def hides damage
               (+0x241 & 0x4000)
     0x46B10E  the unit's +0xFF: the four resource numbers (+0xCC +0xD0 +0xE4 +0xE8), the kills
               line and the status text; stock also passes under the debug bit main+0x3923B & 2
     0x46B457  the owner record's +0x146: the stockpile bar and the target block after it
     0x46B611  the TARGET's owner record +0x146: the target's health bar when its def hides damage

   Each test passes here also for an owner the viewer shares with (tagpu_allysight_shares: the
   alliance is both ways, S of tagpu_allysight.c). S may name a former ally for one tick after an
   alliance breaks; this is a display, read again every frame.

   A REMOTE ally's resource numbers are left out: only the owner's economy pass 0x401360 fills them
   (its settle 0x401B37..0x401BAC; its one caller 0x46555A runs for records of type 1 or 2), and
   every create, the network's included, zeroes them (0x401070 at 0x485D0A), so on this machine a
   unit of a record of type 3 reads 0 for ever. Such a unit enters the panel at the kills line
   0x46B2B8 with the text colour the resource block leaves (0x4C13F0, then 0x4C13A0 with it
   twice); the block's calls leave the stack as they found it (sprintf cleaned by the caller,
   0x4C13A0 ret 8, 0x4C14F0 ret 0x14). A local AI ally's numbers are its own pass's, and shown.

   The panel writes nothing the simulation reads: its stack, the memo main+0x37E60 (a `leave` row
   of replay-globals.tsv), text and colour state and pixels. No branch in the image lands inside a
   replaced range but at its first byte and nothing points into one; the function has no indirect
   jump. All five sites are byte-matched before any is written, and written all or none. */

#include <string.h>

#include "tagpu_allypanel.h"
#include "tagpu_allysight.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"

#define TA_MAIN   0x00511DE8u

typedef struct PanelSite {
    unsigned va;
    unsigned char n;
    unsigned char was[32];
} PanelSite;

static const PanelSite SITES[5] = {
    { 0x0046AC85u, 27, { 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00, 0x8A, 0x8E, 0xFF, 0x00, 0x00, 0x00,
                         0x66, 0xC7, 0x44, 0x24, 0x5A, 0x00, 0x00, 0x3A, 0x8A, 0x43, 0x2A, 0x00,
                         0x00, 0x75, 0x28 } },
    { 0x0046B013u, 32, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00, 0x8B, 0x74, 0x24, 0x24, 0x8B, 0x96,
                         0x96, 0x00, 0x00, 0x00, 0x8A, 0x82, 0x46, 0x01, 0x00, 0x00, 0x8A, 0x91,
                         0x43, 0x2A, 0x00, 0x00, 0x3A, 0xC2, 0x74, 0x15 } },
    { 0x0046B10Eu, 32, { 0xA1, 0xE8, 0x1D, 0x51, 0x00, 0x8A, 0x8E, 0xFF, 0x00, 0x00, 0x00, 0x3A,
                         0x88, 0x43, 0x2A, 0x00, 0x00, 0x74, 0x0D, 0xF6, 0x80, 0x3B, 0x92, 0x03,
                         0x00, 0x02, 0x0F, 0x84, 0x17, 0x03, 0x00, 0x00 } },
    { 0x0046B457u, 32, { 0x8B, 0x86, 0x96, 0x00, 0x00, 0x00, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00,
                         0x8A, 0x88, 0x46, 0x01, 0x00, 0x00, 0x8A, 0x82, 0x43, 0x2A, 0x00, 0x00,
                         0x3A, 0xC8, 0x0F, 0x85, 0x73, 0x04, 0x00, 0x00 } },
    { 0x0046B611u, 28, { 0x8B, 0x86, 0x96, 0x00, 0x00, 0x00, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00,
                         0x8A, 0x88, 0x46, 0x01, 0x00, 0x00, 0x8A, 0x82, 0x43, 0x2A, 0x00, 0x00,
                         0x3A, 0xC8, 0x74, 0x15 } },
};

static unsigned char* rel(unsigned char* p, unsigned char op, unsigned target)
{
    *p++ = op;
    tagpu_detour_rel(p, target);
    return p + 4;
}

static unsigned char* jcc(unsigned char* p, unsigned char cc, unsigned target)
{
    *p++ = 0x0F;
    *p++ = cc;
    tagpu_detour_rel(p, target);
    return p + 4;
}

/* cmp eax,ecx; je PASS; push ecx; push eax; call shares; add esp,8; test eax,eax; jnz PASS;
   jmp FAIL -- eax the owner, ecx the viewer; eax, ecx, edx clobbered (cdecl) */
static unsigned char* tail(unsigned char* p, unsigned pass, unsigned fail)
{
    *p++ = 0x3B; *p++ = 0xC1;
    p = jcc(p, 0x84, pass);
    *p++ = 0x51; *p++ = 0x50;
    p = rel(p, 0xE8, (unsigned)(size_t)tagpu_allysight_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    p = jcc(p, 0x85, pass);
    return rel(p, 0xE9, fail);
}

/* movzx eax,byte [esi+0xFF]; movzx ecx,byte [edx+0x2A43] -- the unit's owner and the viewer,
   with edx = main */
static unsigned char* owner_unit(unsigned char* p)
{
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x86; *p++ = 0xFF; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x8A; *p++ = 0x43; *p++ = 0x2A; *p++ = 0x00; *p++ = 0x00;
    return p;
}

/* mov eax,[esi+0x96]; mov edx,[0x511DE8]; movzx eax,byte [eax+0x146]; movzx ecx,byte [edx+0x2A43]
   -- the owner record's index and the viewer, esi the unit */
static unsigned char* owner_rec(unsigned char* p)
{
    *p++ = 0x8B; *p++ = 0x86; *p++ = 0x96; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x8B; *p++ = 0x15; memcpy(p, &(unsigned){ TA_MAIN }, 4); p += 4;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x80; *p++ = 0x46; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x8A; *p++ = 0x43; *p++ = 0x2A; *p++ = 0x00; *p++ = 0x00;
    return p;
}

/* 0x46AC85: stock writes word [esp+0x5A] = 0 on both paths, and its failure path 0x46ACC8 reads
   edx as main; the shares call clobbers edx, so it is reloaded on the way out. */
static unsigned char* stub_target(unsigned char* s)
{
    unsigned char* p = s;
    *p++ = 0x66; *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = 0x5A; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x8B; *p++ = 0x15; memcpy(p, &(unsigned){ TA_MAIN }, 4); p += 4;
    p = owner_unit(p);
    *p++ = 0x3B; *p++ = 0xC1;
    p = jcc(p, 0x84, 0x0046ACA0u);
    *p++ = 0x51; *p++ = 0x50;
    p = rel(p, 0xE8, (unsigned)(size_t)tagpu_allysight_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x8B; *p++ = 0x15; memcpy(p, &(unsigned){ TA_MAIN }, 4); p += 4;
    *p++ = 0x85; *p++ = 0xC0;
    p = jcc(p, 0x85, 0x0046ACA0u);
    rel(p, 0xE9, 0x0046ACC8u);
    return s;
}

/* 0x46B013: both exits read esi as the unit [esp+0x24] (the hidedamage test 0x46B033 among
   them), so it is loaded first, as stock does. */
static unsigned char* stub_health(unsigned char* s)
{
    unsigned char* p = s;
    *p++ = 0x8B; *p++ = 0x74; *p++ = 0x24; *p++ = 0x24;
    p = owner_rec(p);
    tail(p, 0x0046B048u, 0x0046B033u);
    return s;
}

/* 0x46B10E: the viewer's own unit, or stock's debug bit, shows all (0x46B12E); an ally's local
   unit too; an ally's remote unit (owner record +0x73 == 3) enters at the kills line with the
   colour the resource block leaves; anyone else's is hidden (0x46B445). */
static unsigned char* stub_resources(unsigned char* s)
{
    unsigned char *p = s, *jremote;
    *p++ = 0x8B; *p++ = 0x15; memcpy(p, &(unsigned){ TA_MAIN }, 4); p += 4;
    p = owner_unit(p);
    *p++ = 0x3B; *p++ = 0xC1;
    p = jcc(p, 0x84, 0x0046B12Eu);
    *p++ = 0xF6; *p++ = 0x82; *p++ = 0x3B; *p++ = 0x92; *p++ = 0x03; *p++ = 0x00; *p++ = 0x02;
    p = jcc(p, 0x85, 0x0046B12Eu);                            /* test byte [edx+0x3923B],2; jnz */
    *p++ = 0x51; *p++ = 0x50;
    p = rel(p, 0xE8, (unsigned)(size_t)tagpu_allysight_shares);
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;
    *p++ = 0x85; *p++ = 0xC0;
    p = jcc(p, 0x84, 0x0046B445u);                            /* not an ally: hidden          */
    *p++ = 0x8B; *p++ = 0x86; *p++ = 0x96; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;   /* eax=[esi+0x96] */
    *p++ = 0x80; *p++ = 0x78; *p++ = 0x73; *p++ = 0x03;       /* cmp byte [eax+0x73],3        */
    *p++ = 0x74; jremote = p++;                               /* je remote                    */
    p = rel(p, 0xE9, 0x0046B12Eu);
    *jremote = (unsigned char)(p - (jremote + 1));
    p = rel(p, 0xE8, 0x004C13F0u);                            /* the colour                   */
    *p++ = 0x50; *p++ = 0x50;
    p = rel(p, 0xE8, 0x004C13A0u);
    rel(p, 0xE9, 0x0046B2B8u);
    return s;
}

/* 0x46B457: an owner that does not pass returns the panel (0x46B8EA) */
static unsigned char* stub_stockpile(unsigned char* s)
{
    unsigned char* p = owner_rec(s);
    tail(p, 0x0046B477u, 0x0046B8EAu);
    return s;
}

/* 0x46B611: esi is the target here */
static unsigned char* stub_target_health(unsigned char* s)
{
    unsigned char* p = owner_rec(s);
    tail(p, 0x0046B642u, 0x0046B62Du);
    return s;
}

static int s_installed;

int tagpu_allypanel_installed(void) { return s_installed; }

void tagpu_allypanel_init(void)
{
    unsigned char* (*build[5])(unsigned char*) = { stub_target, stub_health, stub_resources,
                                                   stub_stockpile, stub_target_health };
    unsigned char* stub[5] = { 0 };
    unsigned char b[32];
    int i, written;

    for (i = 0; i < 5; i++)
        if (memcmp((const void*)(size_t)SITES[i].va, SITES[i].was, SITES[i].n)) {
            tagpu_logf("allypanel: NOT installed: 0x%06X does not hold the retail bytes; the unit "
                       "panel shows an ally's unit as an enemy's", SITES[i].va);
            return;
        }
    for (i = 0; i < 5; i++)
        if (!(stub[i] = tagpu_detour_stub())) {
            for (i = 0; i < 5; i++) tagpu_detour_stub_free(stub[i]);
            tagpu_log("allypanel: NOT installed: no stub memory");
            return;
        }
    for (i = 0; i < 5; i++) build[i](stub[i]);
    for (written = 0; written < 5; written++) {
        unsigned r = (unsigned)(size_t)stub[written] - (SITES[written].va + 5);
        b[0] = 0xE9;
        memcpy(b + 1, &r, 4);
        memset(b + 5, 0x90, (size_t)(SITES[written].n - 5));
        if (!tagpu_detour_write(SITES[written].va, b, SITES[written].n)) break;
    }
    if (written < 5) {
        for (i = 0; i < written; i++)
            tagpu_detour_write(SITES[i].va, SITES[i].was, SITES[i].n);
        tagpu_logf("allypanel: NOT installed: 0x%06X could not be written", SITES[written].va);
        return;
    }
    s_installed = 1;
    tagpu_log("allypanel: installed: the unit panel's owner tests 0x46AC85 0x46B013 0x46B10E "
              "0x46B457 0x46B611 pass for an ally; a remote ally's resource numbers are left out");
}
