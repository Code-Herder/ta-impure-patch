/* tagpu_replay_layout.h -- GENERATED from tagpu/ddraw/replay-layout.tsv by
   tools/replay-layout-gen.py, which says what each class means. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_REPLAY_LAYOUT_H
#define TAGPU_REPLAY_LAYOUT_H

#define TAGPU_REPLAY_LAYOUT_SHA256 "4d740e0b4171507bdd5a5b24b616048cd619401df0e1874c8d46a3c6f6b9e0bd"
#define TAGPU_REPLAY_LAYOUT_GEN_SHA256 "d59f7f7421611ef5bff748f8b65b97dbeac5beaa610de3bbd8cb2af599ce5aa9"

enum { RL_UNIT, RL_PLAYER };
enum { RL_ID, RL_OBJECT, RL_CACHE, RL_TIMER, RL_DROP };
enum {              /* an id row's table and the row its pointer names; the recorded ID is one more */
    RL_T_NONE,
    RL_T_UNIT,     /* the slot: (pointer - the unit array's begin) / 0x118 */
    RL_T_TYPE,     /* (pointer - *(main+0x1439B)) / 0x249, the unit-type definitions */
    RL_T_WEAPON,   /* (pointer - the weapon array's base) / 0x115, not the ID byte +0x10A, which holds IDs below 256 only */
    RL_T_PLAYER,   /* (pointer - (main+0x1B63)) / 0x14B, eleven records */
    RL_T_BUCKET,   /* (pointer - *(main+0x1429F)) / 10; NULL, off-map *(main+0x142B7): own IDs */
};

typedef struct ReplayLayoutRow {
    unsigned char  record, cls, target;
    unsigned short offset, size;
    unsigned int   mask;      /* over the field as a little-endian integer; 0 = every byte */
    const char*    name;
} ReplayLayoutRow;

static const ReplayLayoutRow tagpu_replay_layout[] = {
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 0x000, 4, 0x0u, "mover" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 0x010, 4, 0x0u, "weapon1" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 0x02c, 4, 0x0u, "weapon2" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 0x048, 4, 0x0u, "weapon3" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 0x05c, 4, 0x0u, "orders" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 0x060, 4, 0x0u, "suborders" },
    { RL_UNIT, RL_ID, RL_T_BUCKET, 0x082, 4, 0x0u, "bucket" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 0x086, 4, 0x0u, "carrier" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 0x08a, 4, 0x0u, "cargo" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 0x08e, 4, 0x0u, "next" },
    { RL_UNIT, RL_ID, RL_T_TYPE, 0x092, 4, 0x0u, "type" },
    { RL_UNIT, RL_ID, RL_T_PLAYER, 0x096, 4, 0x0u, "owner" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 0x09a, 4, 0x0u, "cob" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 0x09e, 4, 0x0u, "model" },
    { RL_UNIT, RL_CACHE, RL_T_NONE, 0x0a2, 4, 0x0u, "refs" },
    { RL_UNIT, RL_ID, RL_T_PLAYER, 0x0ec, 4, 0x0u, "store" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 0x0f0, 4, 0x0u, "attacker" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, 0x10u, "selected" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, 0xc0u, "marks" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, 0x3c00000u, "buildpage" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x004, 4, 0x0u, "netid" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 0x008, 4, 0x0u, "jointime" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x00c, 4, 0x0u, "hostnum" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x010, 4, 0x0u, "msgcount" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 0x014, 4, 0x0u, "ping" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 0x018, 4, 0x0u, "peertime" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 0x01c, 4, 0x0u, "lastrecv" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x020, 1, 0x0u, "loadpct" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x021, 1, 0x0u, "session" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 0x027, 4, 0x0u, "info" },
    { RL_PLAYER, RL_ID, RL_T_UNIT, 0x067, 4, 0x0u, "blockfirst" },
    { RL_PLAYER, RL_ID, RL_T_UNIT, 0x06b, 4, 0x0u, "blocklast" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x073, 1, 0x0u, "controller" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 0x074, 4, 0x0u, "ai" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 0x078, 4, 0x0u, "groups" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 0x07c, 4, 0x0u, "sight" },
    { RL_PLAYER, RL_CACHE, RL_T_NONE, 0x080, 12, 0x0u, "sightgeom" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 0x0ec, 4, 0x0u, "accumulator" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 0x0f8, 4, 0x0u, "displaytimer" },
};

#endif
