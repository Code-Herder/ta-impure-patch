/* impai_fitparams.c -- the fight predictor's coefficients (impai_fight.h, IMPAI_FIT), as
   `tools/impai/predfit.py emit --start` writes them: the fit's starting point, fitted to no reference. */
#include "impai_fight.h"

const IMPAI_FIT impai_fitparams = {
    {   /* hit: bias, lead */
        { 0x1p+1f, -0x1p+0f },   /* straight */
        { 0x1p+1f, -0x1p+0f },   /* ballistic */
        { 0x1p+1f, -0x1p+0f },   /* homing */
        { 0x1p+1f, -0x1p+0f },   /* dropped */
    },
    0x1p-1f,   /* focus */
    0x1p+0f,   /* splash */
    0x1.99999ap-3f,   /* tau */
};
