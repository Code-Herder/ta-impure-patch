/* impai_seats.c -- which player records ImpAI plays on a live level (impai_seats.h). */
#include <string.h>
#include "impai_seats.h"

#define CTRL_AI     2
#define CTRL_REMOTE 3

void impai_seats_pick(const IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS], int network, unsigned seatai, unsigned netai,
                      IMPAI_SEAT_PICK* out)
{
    const unsigned want = network ? netai : seatai;
    unsigned i;
    memset(out, 0, sizeof *out);
    out->refused = -1;
    for (i = 0; i < IMPAI_SEAT_RECS; i++) {
        if (!rec[i].active) continue;
        out->live++;
        if (rec[i].control != CTRL_REMOTE) continue;
        if (network) {
            if (rec[i].info_ai) out->remote_ai |= 1u << i;
        } else if (out->refused < 0) {
            out->refused = (int)i;
        }
    }
    if (out->refused >= 0) return;
    for (i = 0; i < IMPAI_SEAT_RECS; i++)
        if (rec[i].active && rec[i].control == CTRL_AI && rec[i].ai_object && (want >> i & 1u))
            out->marks |= 1u << i;
}

void impai_seats_list(unsigned mask, char* out)
{
    unsigned i, n = 0;
    for (i = 0; i < IMPAI_SEAT_RECS; i++) {
        if (!(mask >> i & 1u)) continue;
        if (n) out[n++] = ',';
        out[n++] = (char)('0' + i);
    }
    if (!n) memcpy(out, "none", 5);
    else out[n] = 0;
}

unsigned impai_seats_level(int network, uint32_t difficulty)
{
    return network || difficulty > IMPAI_LEVEL_HARD ? IMPAI_LEVEL_HARD : (unsigned)difficulty;
}
