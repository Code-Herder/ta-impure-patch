/* impai_seats_test.c -- the level init's seat choice (src/impai_seats.h) over hand-made records: skirmishes, the
   network games the engine produces on each peer (engine/network.md, A computer seat in a network game) and the
   states the rule refuses, the AI names and the level rule. Engine-free, so the same source builds natively, under AddressSanitizer and
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
    unsigned seatai, named;     /* `named`: bit i, record i's name begins ImpAI: */
    const char* marks;          /* impai_seats_list's spelling */
    const char* remote_ai;
    const char* remote_impai;
    unsigned live;
    int refused;
} CASE;

static const CASE CASES[] = {
    { "skirmish, no seat chosen", "haa.......", 0, 0x000, 0x3FF, "none", "none", "none", 3, -1 },
    { "skirmish, seatai 2", "haa.......", 0, 0x004, 0x000, "2", "none", "none", 3, -1 },
    { "skirmish, seatai names a human and an empty record", "haa.......", 0, 0x009, 0x3FF, "none", "none", "none", 3, -1 },
    { "skirmish, every seat", "haaaaaaaaa", 0, 0x3FF, 0x000, "1,2,3,4,5,6,7,8,9", "none", "none", 10, -1 },
    { "skirmish, an AI seat with no AI object", "hao.......", 0, 0x006, 0x000, "1", "none", "none", 3, -1 },
    { "skirmish, bits past the ten records", "haa.......", 0, 0xFFFFFC00u, 0x000, "none", "none", "none", 3, -1 },
    { "skirmish, a remote record refuses", "har.......", 0, 0x002, 0x3FF, "none", "none", "none", 3, 2 },
    { "skirmish, a remote AI record refuses", "hRa.......", 0, 0x006, 0x3FF, "none", "none", "none", 3, 1 },
    /* recmp_create_begin_test.py's game: the host's rows 0 and 2, the joiner's 1 and 3 */
    { "host, its AI named ImpAI", "hraR......", 1, 0x000, 0x004, "2", "3", "none", 4, -1 },
    { "host, its AI named AI", "hraR......", 1, 0x3FF, 0x000, "none", "3", "none", 4, -1 },
    { "host, the joiner's AI named ImpAI", "hraR......", 1, 0x000, 0x008, "none", "3", "3", 4, -1 },
    { "joiner, its AI on another row", "rh.aR.....", 1, 0x000, 0x018, "3", "4", "4", 4, -1 },
    { "joiner, neither named", "rhRa......", 1, 0x000, 0x000, "none", "2", "none", 4, -1 },
    { "network, seatai is not read", "hraR......", 1, 0x3FF, 0x000, "none", "3", "none", 4, -1 },
    { "network, a human named ImpAI is no seat", "hraR......", 1, 0x000, 0x003, "none", "3", "none", 4, -1 },
    { "network, only the named rows", "haaraR....", 1, 0x000, 0x00A, "1", "5", "none", 6, -1 },
    { "network, no AI object", "hor.......", 1, 0x000, 0x3FF, "none", "none", "none", 3, -1 },
    { "network, every remote record", "hrrrRRRRRa", 1, 0x000, 0x3FF, "9", "4,5,6,7,8", "4,5,6,7,8", 10, -1 },
    { "network, empty", "..........", 1, 0x000, 0x3FF, "none", "none", "none", 0, -1 },
};

/* the names: the prefix test over a field, and the AI name the add and the toggle write */
static const struct { const char* field; size_t cap; int named; } NAMED[] = {
    { "ImpAI:Alice", 30, 1 }, { "AI:Alice", 30, 0 }, { "ImpAI", 30, 0 }, { "impai:Alice", 30, 0 },
    { "ImpAI:", 6, 1 }, { "ImpAI:", 5, 0 }, { "", 30, 0 }, { "Player 1", 30, 0 },
};
static const struct { int impai; const char* human; size_t cap; const char* name; } NAMES[] = {
    { 1, "Alice", 30, "ImpAI:Alice" },
    { 0, "Alice", 30, "AI:Alice" },
    { 1, "Bartholomew-the-Great", 30, "ImpAI:Bartholome" },
    { 0, "Bartholomew-the-Great", 30, "AI:Bartholomew-t" },
    { 1, "", 30, "ImpAI:" },
    /* a field with no terminator in its 30 bytes: only the field is read */
    { 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 30, "AI:ABCDEFGHIJKLM" },
    { 1, "Alice", 3, "ImpAI:Ali" },
};

int main(void)
{
    const unsigned n = (unsigned)(sizeof CASES / sizeof CASES[0]);
    unsigned c, i;
    for (c = 0; c < n; c++) {
        const CASE* k = &CASES[c];
        IMPAI_SEAT_REC rec[IMPAI_SEAT_RECS];
        IMPAI_SEAT_PICK pick;
        char marks[2 * IMPAI_SEAT_RECS], remote[2 * IMPAI_SEAT_RECS], rimpai[2 * IMPAI_SEAT_RECS];
        for (i = 0; i < IMPAI_SEAT_RECS; i++) {
            rec[i] = rec_of(k->recs[i]);
            rec[i].named = (uint8_t)(k->named >> i & 1u);
        }
        impai_seats_pick(rec, k->network, k->seatai, &pick);
        impai_seats_list(pick.marks, marks);
        impai_seats_list(pick.remote_ai, remote);
        impai_seats_list(pick.remote_impai, rimpai);
        printf("%-48s %s network=%d marks=%s remote_ai=%s remote_impai=%s live=%u refused=%d\n", k->name,
               k->recs, k->network, marks, remote, rimpai, pick.live, pick.refused);
        if (strcmp(marks, k->marks) || strcmp(remote, k->remote_ai) || strcmp(rimpai, k->remote_impai) ||
            pick.live != k->live || pick.refused != k->refused) {
            printf("FAIL %s: want marks=%s remote_ai=%s remote_impai=%s live=%u refused=%d\n", k->name, k->marks,
                   k->remote_ai, k->remote_impai, k->live, k->refused);
            return 1;
        }
    }
    {
        /* the level: a skirmish's Difficulty, Hard past 2, Hard in every network game */
        static const struct { int network; uint32_t difficulty; unsigned level; } L[] = {
            { 0, 0, 0 }, { 0, 1, 1 }, { 0, 2, 2 }, { 0, 3, 2 }, { 0, 0xFFFF, 2 }, { 0, 0xFFFFFFFFu, 2 },
            { 1, 0, 2 }, { 1, 1, 2 }, { 1, 2, 2 }, { 1, 7, 2 },
        };
        unsigned k;
        for (k = 0; k < sizeof L / sizeof L[0]; k++) {
            const unsigned got = impai_seats_level(L[k].network, L[k].difficulty);
            printf("level network=%d difficulty=%lu -> %u\n", L[k].network, (unsigned long)L[k].difficulty, got);
            if (got != L[k].level) {
                printf("FAIL level: want %u\n", L[k].level);
                return 1;
            }
        }
    }
    for (c = 0; c < sizeof NAMED / sizeof NAMED[0]; c++) {
        const int got = impai_seats_named(NAMED[c].field, NAMED[c].cap);
        printf("named \"%s\" cap=%u -> %d\n", NAMED[c].field, (unsigned)NAMED[c].cap, got);
        if (got != NAMED[c].named) {
            printf("FAIL named: want %d\n", NAMED[c].named);
            return 1;
        }
    }
    for (c = 0; c < sizeof NAMES / sizeof NAMES[0]; c++) {
        /* the human's name in a 30-byte field padded past its end, as +0x2B is laid out */
        char field[IMPAI_NAME_FIELD], out[IMPAI_NAME_CUT + 1];
        const size_t len = strlen(NAMES[c].human);
        memset(field, 'Z', sizeof field);
        memcpy(field, NAMES[c].human, len < sizeof field ? len : sizeof field);
        if (len < sizeof field) field[len] = 0;
        impai_seats_ai_name(out, NAMES[c].impai, field, NAMES[c].cap);
        printf("name impai=%d \"%s\" cap=%u -> \"%s\"\n", NAMES[c].impai, NAMES[c].human,
               (unsigned)NAMES[c].cap, out);
        if (strcmp(out, NAMES[c].name)) {
            printf("FAIL name: want \"%s\"\n", NAMES[c].name);
            return 1;
        }
    }
    printf("impai_seats_test: %u cases pass\n", n);
    return 0;
}
