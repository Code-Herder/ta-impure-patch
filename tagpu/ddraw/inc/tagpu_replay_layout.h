/* tagpu_replay_layout.h -- GENERATED from tagpu/ddraw/replay-layout.tsv by
   tools/replay-layout-gen.py, which says what each class means. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_REPLAY_LAYOUT_H
#define TAGPU_REPLAY_LAYOUT_H

#define TAGPU_REPLAY_LAYOUT_SHA256 "fd07b323ebefb42b7bc7b69d71d5519493a0721440799d6e31fc0ea248ef6daa"
#define TAGPU_REPLAY_LAYOUT_GEN_SHA256 "551ccecd0e6cd2b14d046d3ebeec3e5b874f3a991e3d8d5cfdd667e8e9b21440"

enum { RL_UNIT, RL_PLAYER, RL_MOVER, RL_ORDER, RL_INFO };
enum { RL_ID, RL_OBJECT, RL_CACHE, RL_TIMER, RL_DROP, RL_PUPPET0, RL_PUPPET1 };
enum {              /* an id row's table and what its pointer names; a row's ID is one more than the row */
    RL_T_NONE,
    RL_T_UNIT,     /* the slot: (pointer - the unit array's begin) / 0x118 */
    RL_T_TYPE,     /* (pointer - *(main+0x1439B)) / 0x249, the unit-type definitions */
    RL_T_WEAPON,   /* (pointer - the weapon array's base) / 0x115, not the ID byte +0x10A, which holds IDs below 256 only */
    RL_T_PLAYER,   /* (pointer - (main+0x1B63)) / 0x14B, eleven records */
    RL_T_BUCKET,   /* (pointer - *(main+0x1429F)) / 10; NULL, off-map *(main+0x142B7): own IDs */
    RL_T_OWNED,    /* not a row: the class code of an order node's own object, by its vtable (the restore's table 0x43A918): 0x4FD3B8 2, 0x4FD3F8 3, 0x4FD328 4, 0x4FD358 5, 0x4FD388 6 */
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
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, (0x1u << 4), "selected" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, (0x3u << 6), "marks" },
    { RL_UNIT, RL_DROP, RL_T_NONE, 0x110, 4, (0xfu << 22), "buildpage" },
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
    { RL_MOVER, RL_DROP, RL_T_NONE, 0x000, 4, 0x0u, "course" },
    { RL_MOVER, RL_CACHE, RL_T_NONE, 0x004, 4, 0x0u, "moveclass" },
    { RL_MOVER, RL_DROP, RL_T_NONE, 0x02e, 1, (0x1fu << 3), "spare" },
    { RL_ORDER, RL_PUPPET0, RL_T_NONE, 0x006, 4, (0x1u << 1), "ondestroy" },
    { RL_ORDER, RL_CACHE, RL_T_NONE, 0x00e, 4, 0x0u, "owner" },
    { RL_ORDER, RL_ID, RL_T_UNIT, 0x016, 4, 0x0u, "target" },
    { RL_ORDER, RL_CACHE, RL_T_NONE, 0x01a, 4, 0x0u, "refnext" },
    { RL_ORDER, RL_CACHE, RL_T_NONE, 0x01e, 4, 0x0u, "refowner" },
    { RL_ORDER, RL_DROP, RL_T_NONE, 0x032, 2, 0x0u, "seenx" },
    { RL_ORDER, RL_DROP, RL_T_NONE, 0x034, 2, 0x0u, "seenz" },
    { RL_ORDER, RL_PUPPET1, RL_T_NONE, 0x042, 4, (0x1u << 16), "keeptargets" },
    { RL_ORDER, RL_DROP, RL_T_NONE, 0x042, 4, (0x1u << 21), "seen" },
    { RL_ORDER, RL_PUPPET0, RL_T_NONE, 0x042, 4, (0x1u << 22), "stopbuilding" },
    { RL_ORDER, RL_OBJECT, RL_T_NONE, 0x04a, 4, 0x0u, "next" },
    { RL_ORDER, RL_PUPPET0, RL_T_OWNED, 0x052, 4, 0x0u, "owned" },
};

#endif
