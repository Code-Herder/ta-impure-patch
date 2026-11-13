/* impai_seats.c -- which player records ImpAI plays on a live level, and the AI names (impai_seats.h). */
#include <string.h>
#include "impai_seats.h"

#define CTRL_AI     2
#define CTRL_REMOTE 3

void impai_seats_pick(const IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS], int network, unsigned seatai,
                      IMPAI_SEAT_PICK* out)
{
    unsigned i;
    memset(out, 0, sizeof *out);
    out->refused = -1;
    for (i = 0; i < IMPAI_SEAT_RECS; i++) {
        if (!rec[i].active) continue;
        out->live++;
        if (rec[i].control != CTRL_REMOTE) continue;
        if (network) {
            if (rec[i].info_ai) {
                out->remote_ai |= 1u << i;
                if (rec[i].named) out->remote_impai |= 1u << i;
            }
        } else if (out->refused < 0) {
            out->refused = (int)i;
        }
    }
    if (out->refused >= 0) return;
    for (i = 0; i < IMPAI_SEAT_RECS; i++)
        if (rec[i].active && rec[i].control == CTRL_AI && rec[i].ai_object &&
            (network ? rec[i].named : (seatai >> i & 1u)))
            out->marks |= 1u << i;
}

int impai_seats_named(const char* name, size_t cap)
{
    const size_t n = sizeof IMPAI_NAME_PREFIX - 1;
    return cap >= n && strncmp(name, IMPAI_NAME_PREFIX, n) == 0;
}

void impai_seats_ai_name(char* out, int impai, const char* human, size_t cap)
{
    const char* prefix = impai ? IMPAI_NAME_PREFIX : OTA_NAME_PREFIX;
    size_t n = strlen(prefix), k = 0;
    memcpy(out, prefix, n);
    while (n < IMPAI_NAME_CUT && k < cap && k < IMPAI_NAME_FIELD && human[k]) out[n++] = human[k++];
    out[n] = 0;
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

int impai_seats_level(int network, uint32_t difficulty)
{
    if (network) return IMPAI_LEVEL_HARD;
    return difficulty <= IMPAI_LEVEL_HARD ? (int)difficulty : -1;
}
