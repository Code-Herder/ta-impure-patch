/* impai_fight.c -- ImpAI's fight predictor (impai_fight.h). */
#include "impai_fight.h"

int impai_fight_kind(uint32_t flags)
{
    if (flags & TASIM_WF_BEAM) return IMPAI_FK_BEAM;
    if (flags & TASIM_WF_DROPPED) return IMPAI_FK_DROPPED;
    if (flags & (TASIM_WF_GUIDANCE | TASIM_WF_TRACKS | TASIM_WF_VLAUNCH)) return IMPAI_FK_GUIDED;
    if (flags & TASIM_WF_BALLISTIC) return IMPAI_FK_BALLISTIC;
    return IMPAI_FK_DIRECT;
}
