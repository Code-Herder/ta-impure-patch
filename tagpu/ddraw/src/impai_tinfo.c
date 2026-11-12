/* impai_tinfo.c -- what ImpAI's brain knows of each unit type (impai.h, IMPAI_TINFO): its category, its gains
   per economy pass on this map, its value. Engine-free: a function of the WORLDQ's def, def_econ and map and of
   the role classifier's answer, so the DLL and tasim build the same table. Single-precision floats, each
   operation rounded once on SSE2 (the Makefile's rule for impai_*.c). */
#include <string.h>
#include "impai.h"

/* A building with no weapon: the classifier makes a standing type with a regular weapon DEFENCE
   (tasim_roles.c, role_of), and counts damage only for regular weapons. */
static int unarmed(const TASIM_ROLE_OUT* r)
{
    return !r || (r->role != TASIM_ROLE_DEFENCE && r->dps_ground == 0.0 && r->dps_air == 0.0);
}

/* THE CATEGORY, the first that holds: a commander (the def's commander tag); an extractor or a geothermal
   plant (WQ_DEF.kind, the snap's own rule); a factory (a standing builder); a constructor (a mobile builder
   that works); a metal maker (a building with makesmetal that uses energy); an energy building (a building
   with no weapon and no makesmetal whose gain is above 0); anything else. */
static int category(const WQ_DEF* d, const WQ_DEF_ECON* e, const TASIM_ROLE_OUT* r, float egain)
{
    const uint32_t rf = r ? r->flags : 0;
    if (rf & TASIM_RF_COMMANDER) return IMPAI_C_COMMANDER;
    if (d->kind == WQ_KIND_MEX) return IMPAI_C_MEX;
    if (d->kind == WQ_KIND_GEO) return IMPAI_C_GEO;
    if (rf & TASIM_RF_FACTORY) return IMPAI_C_FACTORY;
    if (r && r->role == TASIM_ROLE_BUILDER && (rf & TASIM_RF_MOBILE) && d->workertime > 0) return IMPAI_C_CONS;
    if (e->building && e->makesmetal > 0 && e->energyuse > 0) return IMPAI_C_MAKER;
    if (e->building && unarmed(r) && e->makesmetal == 0 && egain > 0) return IMPAI_C_ENERGY;
    return IMPAI_C_OTHER;
}

int impai_tinfo_build(const WORLDQ* q, const TASIM_ROLE_OUT* roles, int nroles, IMPAI_TINFO* ti, int nti)
{
    const int n = q->ops->ndefs(q);
    WQ_MAP m;
    int t;
    if (n < 1 || n > nti) return -1;
    memset(ti, 0, (size_t)n * sizeof *ti);
    q->ops->map(q, &m);
    for (t = 1; t < n; t++) {
        const TASIM_ROLE_OUT* r = roles && t < nroles ? &roles[t] : NULL;
        IMPAI_TINFO* o = &ti[t];
        WQ_DEF d;
        WQ_DEF_ECON e;
        float egain;
        if (!q->ops->def(q, t, &d) || !q->ops->def_econ(q, t, &e)) continue;
        egain = e.energymake + (e.energyuse < 0 ? -e.energyuse : 0.0f) + e.wind * m.wind_mean + e.tidal * m.tide;
        o->cat = (uint8_t)category(&d, &e, r, egain);
        o->tech = (uint8_t)(r && r->tech > 0 ? (r->tech < 255 ? r->tech : 255) : 0);
        o->flags = (uint8_t)(((r ? (r->flags & TASIM_RF_MOBILE) != 0 : (d.flags & WQ_DF_MOVER) != 0) ? IMPAI_TF_MOBILE : 0) |
                             ((d.flags & WQ_DF_VTOL) ? IMPAI_TF_VTOL : 0) |
                             (e.minwater > 0 || e.tidal > 0 ? IMPAI_TF_WATER : 0));
        o->mclass = d.mclass;
        o->egain = o->cat == IMPAI_C_MAKER ? e.energyuse : egain;
        o->mgain = o->cat == IMPAI_C_MEX ? e.extracts : e.metalmake + (float)e.makesmetal;
        o->value = e.cost_m + e.cost_e / (float)TASIM_ROLES_R;
        o->workertime = d.workertime;
        o->fx = d.fx;
        o->fz = d.fz;
    }
    return n;
}

void impai_tinfo_count(const IMPAI_TINFO* ti, int n, unsigned count[IMPAI_CATS])
{
    int t;
    memset(count, 0, IMPAI_CATS * sizeof *count);
    for (t = 1; t < n; t++) count[ti[t].cat < IMPAI_CATS ? ti[t].cat : IMPAI_C_OTHER]++;
}
