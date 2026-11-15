/* impai_fitparams.c -- the fight predictor's coefficients (impai_fight.h, IMPAI_FIT), as
   `tools/impai/predfit.py emit --fit` writes them: fitted to the training half (142 fights) of reference set defcfcdb2359, content 3D002F542F3F142E. */
#include "impai_fight.h"
#include "tagpu_simreg.h"

/* The predictor's coefficients, OWNER with the predictor that reads them (impai_fight.c). A table and no
   function: a change to it runs these test groups and asks no function of it to be reached. */
SIM_FEATURE(impai_fitparams, SIM_PUPPET_OWNER, "impai-fight stock-impai-fight impai-eco impai-observe stock-impai-observe impai-naval stock-impai-naval");

const IMPAI_FIT impai_fitparams = {
    {   /* hit: bias, lead */
        { 0x1.0943fep+1f, -0x1.793496p-1f },   /* straight */
        { 0x1.ad094cp+0f, -0x1.a24dc6p+0f },   /* ballistic */
        { 0x1p+1f, -0x1p+0f },   /* homing */
        { 0x1p+1f, -0x1p+0f },   /* dropped */
    },
    0x1.154c3ap-2f,   /* focus */
    0x1.a0753cp-1f,   /* splash */
    0x1.d8ab5ep-3f,   /* tau */
};
