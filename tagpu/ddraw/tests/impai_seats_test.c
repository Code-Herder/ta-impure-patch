/* impai_seats_test.c -- the level init's seat choice (src/impai_seats.h) over hand-made records: skirmishes, the
   network games the engine produces on each peer (engine/network.md, A computer seat in a network game) and the
   states the rule refuses. Engine-free, so the same source builds natively, under AddressSanitizer and
   UndefinedBehaviorSanitizer, and with the DLL's compiler under Wine (tests/impai_seats_test.py); each build
   prints the same transcript, which the driver compares. Exit 1 at the first case whose marks, remote AI seats,
   live count or refusal differ from what the case expects. */
#include <stdio.h>
#include <string.h>
#include "impai_seats.h"

enum { OFF, HUMAN, AI, REMOTE };

/* One record: `h` a human with its AI object (0x464700 builds one for every record but an active type 3), `a` a
   local AI seat, `r` a remote human, `R` a remote AI seat (PlayerInfo type 2), `o` an AI seat with no AI object,
   `.` an empty record. */
static IMPAI_SEAT_REC rec_of(char c)
{
    IMPAI_SEAT_REC r;
    memset(&r, 0, sizeof r);
    switch (c) {
    case 'h': r.active = 1; r.control = HUMAN; r.ai_object = 1; r.info_ai = 0; break;
    case 'a': r.active = 1; r.control = AI; r.ai_object = 1; r.info_ai = 1; break;
    case 'o': r.active = 1; r.control = AI; r.ai_object = 0; r.info_ai = 1; break;
    case 'r': r.active = 1; r.control = REMOTE; r.ai_object = 0; r.info_ai = 0; break;
    case 'R': r.active = 1; r.control = REMOTE; r.ai_object = 0; r.info_ai = 1; break;
    default: break;
    }
    return r;
}

typedef struct CASE {
    const char* name;
    const char* recs;           /* IMPAI_SEAT_RECS characters, rec_of's */
    int network;
    unsigned seatai, netai;
    const char* marks;          /* impai_seats_list's spelling */
    const char* remote_ai;
    unsigned live;
    int refused;
} CASE;

static const CASE CASES[] = {
    { "skirmish, no seat chosen", "haa.......", 0, 0x000, 0x3FF, "none", "none", 3, -1 },
    { "skirmish, seatai 2", "haa.......", 0, 0x004, 0x000, "2", "none", 3, -1 },
    { "skirmish, seatai names a human and an empty record", "haa.......", 0, 0x009, 0x3FF, "none", "none", 3, -1 },
    { "skirmish, every seat", "haaaaaaaaa", 0, 0x3FF, 0x000, "1,2,3,4,5,6,7,8,9", "none", 10, -1 },
    { "skirmish, an AI seat with no AI object", "hao.......", 0, 0x006, 0x000, "1", "none", 3, -1 },
    { "skirmish, bits past the ten records", "haa.......", 0, 0xFFFFFC00u, 0x000, "none", "none", 3, -1 },
    { "skirmish, a remote record refuses", "har.......", 0, 0x002, 0x3FF, "none", "none", 3, 2 },
    { "skirmish, a remote AI record refuses", "hRa.......", 0, 0x006, 0x3FF, "none", "none", 3, 1 },
    /* recmp_create_begin_test.py's game: the host's rows 0 and 2, the joiner's 1 and 3 */
    { "host, netai impai", "hraR......", 1, 0x000, 0x3FF, "2", "3", 4, -1 },
    { "host, netai ota", "hraR......", 1, 0x3FF, 0x000, "none", "3", 4, -1 },
    { "joiner, netai impai, the AI on another row", "rh.aR.....", 1, 0x000, 0x3FF, "3", "4", 4, -1 },
    { "joiner, netai ota", "rhRa......", 1, 0x000, 0x000, "none", "2", 4, -1 },
    { "network, seatai is not read", "hraR......", 1, 0x3FF, 0x000, "none", "3", 4, -1 },
    { "network, only the rows netai names", "haaraR....", 1, 0x000, 0x00A, "1", "5", 6, -1 },
    { "network, no AI object", "hor.......", 1, 0x000, 0x3FF, "none", "none", 3, -1 },
    { "network, every remote record", "hrrrRRRRRa", 1, 0x000, 0x3FF, "9", "4,5,6,7,8", 10, -1 },
    { "network, empty", "..........", 1, 0x000, 0x3FF, "none", "none", 0, -1 },
};

int main(void)
{
    const unsigned n = (unsigned)(sizeof CASES / sizeof CASES[0]);
    unsigned c, i;
    for (c = 0; c < n; c++) {
        const CASE* k = &CASES[c];
        IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS];
        IMPAI_SEAT_PICK pick;
        char marks[2 * IMPAI_SEAT_RECS], remote[2 * IMPAI_SEAT_RECS];
        for (i = 0; i < IMPAI_SEAT_RECS; i++) rec[i] = rec_of(k->recs[i]);
        impai_seats_pick(rec, k->network, k->seatai, k->netai, &pick);
        impai_seats_list(pick.marks, marks);
        impai_seats_list(pick.remote_ai, remote);
        printf("%-48s %s network=%d marks=%s remote_ai=%s live=%u refused=%d\n", k->name, k->recs, k->network,
               marks, remote, pick.live, pick.refused);
        if (strcmp(marks, k->marks) || strcmp(remote, k->remote_ai) || pick.live != k->live ||
            pick.refused != k->refused) {
            printf("FAIL %s: want marks=%s remote_ai=%s live=%u refused=%d\n", k->name, k->marks, k->remote_ai,
                   k->live, k->refused);
            return 1;
        }
    }
    printf("impai_seats_test: %u cases pass\n", n);
    return 0;
}
