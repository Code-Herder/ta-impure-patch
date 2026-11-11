/* tasim_cob.c -- the shared simulation core's binding to the units' scripts: the facts every call of the in-game
   wiki's COB host (tagpu_wikicob.h, over tagpu_cobvm.c) reads, and a call on one unit's script.
   research/notes/impai/shared-core.md, tasim_cob. Contract: tasim.h.

   The host is the sim's own (WIKISIM.cob, NULL without scripts), so is the facts' scratch; no globals. */
#include <stdlib.h>
#include "tasim.h"

/* GET's facts: every unit with a script handle, in creation order -- its handle, owner, centre,
   the build fraction still to go as the float unit+0x104 holds, and HEALTH in percent, 100 since
   the wiki has no damage. The count, 0 when the scratch could not grow (oom set). */
int tasim_facts(WIKISIM* s)
{
    int i, n = 0;
    if (s->nunit > s->cfact) {
        WIKICOBFACT* p = (WIKICOBFACT*)realloc(s->fact, (size_t)s->nunit * sizeof *p);
        if (!p) { s->oom = 1; return 0; }
        s->fact = p;
        s->cfact = s->nunit;
    }
    for (i = 0; i < s->nunit; i++) {
        const WIKIUNIT* u = s->unit[i];
        WIKICOBFACT* f = &s->fact[n];
        if (!u->cob) continue;
        f->unit = u->cob;
        f->owner = u->owner;
        f->x = tasim_fix16(u->x);
        f->z = tasim_fix16(u->z);
        f->left = (float)(1.0 - u->built);
        f->health = 100;
        n++;
    }
    return n;
}

void tasim_cob_call(WIKISIM* s, const WIKIUNIT* u, const char* script, int how, int argc, const int32_t* args)
{
    int n;
    if (!s->cob || !u->cob) return;
    n = tasim_facts(s);
    tagpu_wikicob_call(s->cob, u->cob, script, how, argc, args, s->fact, n);
}
