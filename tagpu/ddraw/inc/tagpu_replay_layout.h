/* tagpu_replay_layout.h -- GENERATED from tagpu/ddraw/replay-layout.tsv by
   tools/replay-layout-gen.py, which says what each class means. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_REPLAY_LAYOUT_H
#define TAGPU_REPLAY_LAYOUT_H

#define TAGPU_REPLAY_LAYOUT_SHA256 "a4774fc386057abdae9c65e561ce0a6a305a3edc561ea3a8db76d3dfe0eb2e6f"

enum { RL_UNIT, RL_PLAYER };
enum { RL_ID, RL_OBJECT, RL_CACHE, RL_TIMER, RL_DROP };
enum {              /* an id row's table: the ID is the pointer's offset in it */
    RL_T_NONE,
    RL_T_UNIT,     /* the slot: (pointer - the unit array's begin) / 0x118 */
    RL_T_TYPE,     /* (pointer - *(main+0x1439B)) / 0x249, the unit-type definitions */
    RL_T_WEAPON,   /* the index in the weapon definitions, stride 0x115 */
    RL_T_PLAYER,   /* (pointer - (main+0x1B63)) / 0x14B, eleven records */
    RL_T_BUCKET,   /* (pointer - *(main+0x1429F)) / 10; NULL, off-map *(main+0x142B7): own IDs */
};

typedef struct ReplayLayoutRow {
    unsigned char  record, cls, target, size;
    unsigned short offset;
    unsigned int   mask;      /* over the field as a little-endian integer; 0 = every byte */
    const char*    name;
} ReplayLayoutRow;

static const ReplayLayoutRow tagpu_replay_layout[] = {
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 4, 0x000, 0x0u, "mover" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 4, 0x010, 0x0u, "weapon1" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 4, 0x02c, 0x0u, "weapon2" },
    { RL_UNIT, RL_ID, RL_T_WEAPON, 4, 0x048, 0x0u, "weapon3" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 4, 0x05c, 0x0u, "orders" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 4, 0x060, 0x0u, "suborders" },
    { RL_UNIT, RL_ID, RL_T_BUCKET, 4, 0x082, 0x0u, "bucket" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 4, 0x086, 0x0u, "carrier" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 4, 0x08a, 0x0u, "cargo" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 4, 0x08e, 0x0u, "next" },
    { RL_UNIT, RL_ID, RL_T_TYPE, 4, 0x092, 0x0u, "type" },
    { RL_UNIT, RL_ID, RL_T_PLAYER, 4, 0x096, 0x0u, "owner" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 4, 0x09a, 0x0u, "cob" },
    { RL_UNIT, RL_OBJECT, RL_T_NONE, 4, 0x09e, 0x0u, "model" },
    { RL_UNIT, RL_CACHE, RL_T_NONE, 4, 0x0a2, 0x0u, "refs" },
    { RL_UNIT, RL_ID, RL_T_PLAYER, 4, 0x0ec, 0x0u, "store" },
    { RL_UNIT, RL_ID, RL_T_UNIT, 4, 0x0f0, 0x0u, "attacker" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 4, 0x110, 0x10u, "selected" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 4, 0x110, 0xc0u, "marks" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 4, 0x110, 0x700u, "seen" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 4, 0x110, 0x3c00000u, "buildpage" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 4, 0x004, 0x0u, "netid" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 4, 0x008, 0x0u, "jointime" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 4, 0x00c, 0x0u, "hostnum" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 4, 0x010, 0x0u, "msgcount" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 4, 0x014, 0x0u, "ping" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 4, 0x018, 0x0u, "peertime" },
    { RL_PLAYER, RL_TIMER, RL_T_NONE, 4, 0x01c, 0x0u, "lastrecv" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 1, 0x020, 0x0u, "loadpct" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 1, 0x021, 0x0u, "session" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 4, 0x027, 0x0u, "info" },
    { RL_PLAYER, RL_ID, RL_T_UNIT, 4, 0x067, 0x0u, "blockfirst" },
    { RL_PLAYER, RL_ID, RL_T_UNIT, 4, 0x06b, 0x0u, "blocklast" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 1, 0x073, 0x0u, "controller" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 4, 0x074, 0x0u, "ai" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 4, 0x078, 0x0u, "groups" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 4, 0x07c, 0x0u, "sight" },
    { RL_PLAYER, RL_CACHE, RL_T_NONE, 12, 0x080, 0x0u, "sightgeom" },
    { RL_PLAYER, RL_OBJECT, RL_T_NONE, 4, 0x0ec, 0x0u, "accumulator" },
    { RL_PLAYER, RL_DROP, RL_T_NONE, 4, 0x0f8, 0x0u, "displaytimer" },
};

#endif
