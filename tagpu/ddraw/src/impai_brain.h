#ifndef IMPAI_BRAIN_H
#define IMPAI_BRAIN_H
/* impai_brain.h -- the brain's own half, shared by impai_brain.c (the census, the job checks, the entry),
   impai_econ.c (the ladder, the type choice, the factories) and impai_build.c (the sites and the orders): one
   decision's working state, IMPAI_DEC, which lives in the caller's scratch for the call and nothing else. */
#include "impai.h"
#include "tagpu_placegeo.h"

typedef struct IMPAI_RECT { int16_t x0, z0, x1, z1; } IMPAI_RECT;        /* cells, inclusive               */
typedef struct IMPAI_CLAIM { uint16_t type; IMPAI_RECT r; } IMPAI_CLAIM;
typedef struct IMPAI_STRUCT { uint16_t slot, type; IMPAI_RECT r; uint8_t cat, finished; } IMPAI_STRUCT;
typedef struct IMPAI_UNITREF { uint16_t slot, type; int32_t pos[3]; } IMPAI_UNITREF;

typedef struct IMPAI_DEC {
    const IMPAI_BRAIN_IN* in;
    IMPAI_BRAIN_OUT*      out;
    const WORLDQ*         q;
    WQ_MAP                m;
    WQ_ECON               e;
    uint32_t              now;
    int                   act_build, act_vtol_build, act_queue;   /* the world's action bytes; -1 none  */
    /* the census */
    uint16_t      count[IMPAI_CATS], done[IMPAI_CATS], pending[IMPAI_CATS];
    IMPAI_UNITREF idle[IMPAI_IDLE_CAP], helpers[IMPAI_IDLE_CAP], jobbed[2 * IMPAI_IDLE_CAP];
    uint8_t       jobbed_idle[2 * IMPAI_IDLE_CAP];
    IMPAI_UNITREF fidle[IMPAI_IDLE_CAP], fbusy[IMPAI_IDLE_CAP];
    int           nidle, nhelpers, njobbed, nfidle, nfbusy;
    IMPAI_CLAIM   claims[IMPAI_CLAIM_MAX];
    int           nclaims;
    IMPAI_STRUCT  structs[IMPAI_STRUCT_MAX];
    int           nstructs;
    IMPAI_UNITREF nano[IMPAI_NANO_MAX];
    int           nnano;
    int32_t       queued_cons;
    IMPAI_UNITREF commander, lowest;            /* slot 0: none                                          */
    int           explorer_live;
    float         pe_extra;                     /* the energy the seat's claims and frames will add      */
    int           has_bbox;
    IMPAI_RECT    bbox;
    /* the economy: the level row's ratios with the decision's noise on them (impai_brain_decide) */
    float         e_per_m, factory_per_m, cons_per_m;
    int           e_stall, m_stall, want_factory;
    /* the army's production: each class's value built, building and queued; the highest tech of the seat's
       factories and how many it has of it; its anti-air structures, built, building and ordered */
    float         kspent[IMPAI_KS];
    uint8_t       ftop;
    uint16_t      ftop_n, aa_n;
    int           want_tech, want_aa;
    float         pe_in, pm_in;
    /* what the decision has spent of its budgets */
    int           sites, snaps, roots, queued, assigned, stopped, explored_once;
    /* scratch beside the struct: two build menus, a short order list, the ring's sites, the threat maps */
    uint16_t*     menu;
    uint16_t*     menu2;
    WQ_ORDER      olist[8];
    PlaceCell*    ring;
    IMPAI_MAPS    maps;
    int           maps_built;                   /* 0 not yet, 1 built, -1 none to build                  */
} IMPAI_DEC;

/* 1 when `type` is an anti-air structure: a building whose threat row's anti-air dps passes its ground dps;
   0 with no threat table */
static inline int impai_aa_struct(const IMPAI_BRAIN_IN* in, unsigned type)
{
    const IMPAI_THREAT* th;
    if (!in->view || !in->view->threat || type >= (unsigned)in->view->ntypes || type >= in->nti) return 0;
    th = &in->view->threat[type];
    return !(in->ti[type].flags & IMPAI_TF_MOBILE) && th->dps_air > th->dps_ground;
}

/* impai_brain.c */
void impai_fail_add(IMPAI_DEC* d, int cx, int cz);
int  impai_failed_in(const IMPAI_DEC* d, const IMPAI_RECT* r);
int  impai_claimed(const IMPAI_DEC* d, const IMPAI_RECT* r);
void impai_claim_add(IMPAI_DEC* d, int type, const IMPAI_RECT* r);
IMPAI_RECT impai_rect_at(const IMPAI_DEC* d, int type, int cx, int cz);
uint32_t impai_isqrt(uint64_t v);
uint32_t impai_dist_px(const int32_t a[3], int32_t bx_px, int32_t bz_px);

/* impai_econ.c */
void impai_econ_assign(IMPAI_DEC* d);
void impai_econ_factories(IMPAI_DEC* d);

/* impai_build.c: each 1 when the builder was given its order */
int  impai_build_spot(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
int  impai_build_ring(IMPAI_DEC* d, const IMPAI_UNITREF* b, int type);
int  impai_build_explore(IMPAI_DEC* d, const IMPAI_UNITREF* b);
int  impai_build_assist(IMPAI_DEC* d, const IMPAI_UNITREF* b);
const SNAP_PATCH* impai_spot_pick(IMPAI_DEC* d, const IMPAI_UNITREF* b);

#endif
