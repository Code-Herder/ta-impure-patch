/* impai_fitparams.c -- the fight predictor's coefficients (impai_fight.h, IMPAI_FIT), as
   `tools/impai/predfit.py emit --start` writes them: the fit's starting point, fitted to no reference. */
#include "impai_fight.h"

const IMPAI_FIT impai_fitparams = {
    {   /* hit: bias, lead, spread, area */
        { 0x1p+1f, -0x1p+0f, -0x1p+0f, 0x1p+0f },   /* beam */
        { 0x1p+1f, -0x1p+0f, -0x1p+0f, 0x1p+0f },   /* direct */
        { 0x1p+1f, -0x1p+0f, -0x1p+0f, 0x1p+0f },   /* ballistic */
        { 0x1p+1f, -0x1p+0f, -0x1p+0f, 0x1p+0f },   /* guided */
        { 0x1p+1f, -0x1p+0f, -0x1p+0f, 0x1p+0f },   /* dropped */
    },
    0x1p+0f,   /* close */
    0x1p-1f,   /* focus */
    0x1p-1f,   /* splash */
    0x1.99999ap-3f,   /* tau */
};
