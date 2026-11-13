/* fightlib.c -- ImpAI's fight predictor (tagpu/ddraw/src/impai_fight.c) as a native shared library, for
   tools/impai/predfit.py through ctypes: the fit and the check call the DLL's own C, never a Python copy.
   Built by tools/tasim/native.py fightlib with the predictor's flags, so it rounds as the DLL does, which
   tagpu/ddraw/tests/impai_fight_test.py holds to one transcript in three builds. It links the buy
   (src/impai_table.c) too, so impai_buy and impai_class_of are the library's exports and the tuning tools buy
   the armies the DLL would, its type choice reading this predictor's pair rate and exponential. */
#include <stdint.h>
#include "impai_fight.h"
#include "impai_table.h"

/* The sizes of the structs the caller packs, so it refuses a library whose layout it does not know:
   IMPAI_FTYPE, IMPAI_FWEAPON, IMPAI_FDMG, IMPAI_FSIDE, IMPAI_FIT, IMPAI_FRESULT. */
void fightlib_sizes(uint32_t out[6])
{
    out[0] = sizeof(IMPAI_FTYPE);
    out[1] = sizeof(IMPAI_FWEAPON);
    out[2] = sizeof(IMPAI_FDMG);
    out[3] = sizeof(IMPAI_FSIDE);
    out[4] = sizeof(IMPAI_FIT);
    out[5] = sizeof(IMPAI_FRESULT);
}

/* n fights of the content (t, w, d) under the coefficients p: fight k is a[k] against b[k] at dist[k],
   its result in out[k] and impai_fight_predict's answer in rc[k]. Returns how many were refused. */
unsigned fightlib_batch(const IMPAI_FTYPE* t, uint32_t nt, const IMPAI_FWEAPON* w, uint32_t nw, const IMPAI_FDMG* d,
                        uint32_t nd, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                        const int32_t* dist, uint32_t n, IMPAI_FRESULT* out, int32_t* rc)
{
    const IMPAI_FCONTENT c = { t, nt, w, nw, d, nd };
    unsigned refused = 0;
    uint32_t k;
    for (k = 0; k < n; k++) {
        rc[k] = impai_fight_predict(&c, p, &a[k], &b[k], dist[k], &out[k]);
        refused += rc[k] != 0;
    }
    return refused;
}

/* the fitted coefficients the DLL was built with (impai_fitparams.c) */
void fightlib_params(IMPAI_FIT* out)
{
    *out = impai_fitparams;
}

/* impai_buy with this predictor: the army table `tab` buys for value V against `pic` from the menu
   menu[0 .. nmenu), the type choice reading the content's pair rates under the coefficients p. impai_buy's
   answer. */
int fightlib_buy(const IMPAI_FTYPE* t, uint32_t nt, const IMPAI_FWEAPON* w, uint32_t nw, const IMPAI_FDMG* d,
                 uint32_t nd, const IMPAI_FIT* p, const IMPAI_TABLE* tab, const IMPAI_PICTURE* pic, float V,
                 const IMPAI_MENU_TYPE* menu, uint32_t nmenu, IMPAI_ARMY* out)
{
    const IMPAI_FCONTENT c = { t, nt, w, nw, d, nd };
    const IMPAI_FCTX ctx = { &c, p };
    const IMPAI_PREDICTOR pred = { impai_fight_rate_of, impai_expf, &ctx };
    const IMPAI_MENU m = { menu, nmenu };
    return impai_buy(tab, pic, V, &m, &pred, out);
}
