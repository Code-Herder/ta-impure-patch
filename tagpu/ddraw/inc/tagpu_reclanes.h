/* tagpu_reclanes.h -- GENERATED from tagpu/ddraw/replay-lanes.tsv by
   tools/replay-lanes-gen.py, which says what the lanes and roles are. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_RECLANES_H
#define TAGPU_RECLANES_H

#include <stdint.h>

#define TAGPU_RECLANES_SHA256 "19cb7888b7d1fd47bc5cdc24832c471eb2e4107544ed00ff72320f9d54e1b840"
#define TAGPU_RECLANES_GEN_SHA256 "759e037d327c0e235ad68e97af0ef7d2fa342f5c207235e59279243142e6c65e"

enum RecLaneRole {      /* a format constant: RT_CODEC's role byte */
    REC_LR_PLAIN,       /* any width */
    REC_LR_HEADING,     /* 2 bytes */
    REC_LR_TURN,        /* 2 bytes */
    REC_LR_SPEED,       /* 4 bytes */
    REC_LR_VX,          /* 4 bytes */
    REC_LR_VY,          /* 4 bytes */
    REC_LR_VZ,          /* 4 bytes */
    REC_LR_AX,          /* 4 bytes */
    REC_LR_AY,          /* 4 bytes */
    REC_LR_AZ,          /* 4 bytes */
    REC_LR_X,           /* 4 bytes */
    REC_LR_Y,           /* 4 bytes */
    REC_LR_Z,           /* 4 bytes */
    REC_LR_BANK,        /* 2 bytes */
    REC_LR_ROLL,        /* 2 bytes */
    REC_LR_FX,          /* 2 bytes */
    REC_LR_FZ,          /* 2 bytes */
    REC_LR_FW,          /* 2 bytes */
    REC_LR_FH,          /* 2 bytes */
    REC_LR_SX,          /* 2 bytes */
    REC_LR_SZ,          /* 2 bytes */
    REC_LR_CLAIM,       /* 4 bytes */
    REC_LR_STAMP,       /* 4 bytes */
    REC_LR_LAYER,       /* 1 byte */
    REC_LR_TYPE,        /* 2 bytes */
    REC_LR_COUNT_,      /* any width */
    REC_LR_OWNER,       /* 4 bytes */
    REC_LR_EREQ,        /* 4 bytes */
    REC_LR_EGRANT,      /* 4 bytes */
    REC_LR_MREQ,        /* 4 bytes */
    REC_LR_MGRANT,      /* 4 bytes */
    REC_LR_HP,          /* 2 bytes */
    REC_LR_BUILD_LEFT,  /* 4 bytes */
    REC_LR_HPPCT,       /* 1 byte */
    REC_LR_HPPCT30,     /* 1 byte */
    REC_LR_BUCKET,      /* 4 bytes */
    REC_LR_N
};

/* the width a role's rule computes, in bytes; 0 for any of 1, 2 and 4 */
static const unsigned char rec_role_width[REC_LR_N] = { 0, 2, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 1, 2, 0, 4, 4, 4, 4, 4, 2, 4, 1, 1, 4 };

typedef struct RecLaneRow { uint16_t off; uint8_t width, role; } RecLaneRow;

#define REC_LANES_IMAGE 0x148
#define REC_LANES_N 112
static const RecLaneRow rec_lanes[REC_LANES_N] = {
    { 0x000, 4, REC_LR_PLAIN },      /* mover */
    { 0x004, 2, REC_LR_PLAIN },      /* w1_target0 */
    { 0x006, 2, REC_LR_PLAIN },      /* w1_target1 */
    { 0x008, 4, REC_LR_PLAIN },      /* w1_callback */
    { 0x00c, 4, REC_LR_PLAIN },      /* w1_aimed */
    { 0x010, 4, REC_LR_PLAIN },      /* weapon1 */
    { 0x014, 4, REC_LR_PLAIN },      /* w1_distance */
    { 0x018, 2, REC_LR_COUNT_ },     /* w1_reload */
    { 0x01a, 2, REC_LR_PLAIN },      /* w1_aim_heading */
    { 0x01c, 2, REC_LR_PLAIN },      /* w1_aim_pitch */
    { 0x01e, 1, REC_LR_PLAIN },      /* w1_stockpile */
    { 0x01f, 1, REC_LR_PLAIN },      /* w1_flags */
    { 0x020, 2, REC_LR_PLAIN },      /* w2_target0 */
    { 0x022, 2, REC_LR_PLAIN },      /* w2_target1 */
    { 0x024, 4, REC_LR_PLAIN },      /* w2_callback */
    { 0x028, 4, REC_LR_PLAIN },      /* w2_aimed */
    { 0x02c, 4, REC_LR_PLAIN },      /* weapon2 */
    { 0x030, 4, REC_LR_PLAIN },      /* w2_distance */
    { 0x034, 2, REC_LR_COUNT_ },     /* w2_reload */
    { 0x036, 2, REC_LR_PLAIN },      /* w2_aim_heading */
    { 0x038, 2, REC_LR_PLAIN },      /* w2_aim_pitch */
    { 0x03a, 1, REC_LR_PLAIN },      /* w2_stockpile */
    { 0x03b, 1, REC_LR_PLAIN },      /* w2_flags */
    { 0x03c, 2, REC_LR_PLAIN },      /* w3_target0 */
    { 0x03e, 2, REC_LR_PLAIN },      /* w3_target1 */
    { 0x040, 4, REC_LR_PLAIN },      /* w3_callback */
    { 0x044, 4, REC_LR_PLAIN },      /* w3_aimed */
    { 0x048, 4, REC_LR_PLAIN },      /* weapon3 */
    { 0x04c, 4, REC_LR_PLAIN },      /* w3_distance */
    { 0x050, 2, REC_LR_COUNT_ },     /* w3_reload */
    { 0x052, 2, REC_LR_PLAIN },      /* w3_aim_heading */
    { 0x054, 2, REC_LR_PLAIN },      /* w3_aim_pitch */
    { 0x056, 1, REC_LR_PLAIN },      /* w3_stockpile */
    { 0x057, 1, REC_LR_PLAIN },      /* w3_flags */
    { 0x058, 4, REC_LR_PLAIN },      /* u058 */
    { 0x05c, 4, REC_LR_PLAIN },      /* orders */
    { 0x060, 4, REC_LR_PLAIN },      /* suborders */
    { 0x064, 2, REC_LR_BANK },       /* bank */
    { 0x066, 2, REC_LR_HEADING },    /* heading */
    { 0x068, 2, REC_LR_ROLL },       /* roll */
    { 0x06a, 4, REC_LR_X },          /* x */
    { 0x06e, 4, REC_LR_Y },          /* y */
    { 0x072, 4, REC_LR_Z },          /* z */
    { 0x076, 2, REC_LR_FX },         /* foot_x */
    { 0x078, 2, REC_LR_FZ },         /* foot_z */
    { 0x07a, 2, REC_LR_SX },         /* sight_x */
    { 0x07c, 2, REC_LR_SZ },         /* sight_z */
    { 0x07e, 2, REC_LR_FW },         /* foot_w */
    { 0x080, 2, REC_LR_FH },         /* foot_h */
    { 0x082, 4, REC_LR_BUCKET },     /* bucket */
    { 0x086, 4, REC_LR_PLAIN },      /* carrier */
    { 0x08a, 4, REC_LR_PLAIN },      /* cargo */
    { 0x08e, 4, REC_LR_PLAIN },      /* next */
    { 0x092, 4, REC_LR_PLAIN },      /* type */
    { 0x096, 4, REC_LR_PLAIN },      /* owner */
    { 0x09a, 4, REC_LR_PLAIN },      /* cob */
    { 0x09e, 4, REC_LR_PLAIN },      /* model */
    { 0x0a2, 4, REC_LR_PLAIN },      /* refs */
    { 0x0a6, 2, REC_LR_TYPE },       /* type_index */
    { 0x0a8, 2, REC_LR_PLAIN },      /* slot */
    { 0x0aa, 2, REC_LR_PLAIN },      /* hover_phase */
    { 0x0ac, 4, REC_LR_PLAIN },      /* u0ac */
    { 0x0b0, 4, REC_LR_PLAIN },      /* deadline */
    { 0x0b4, 4, REC_LR_PLAIN },      /* u0b4 */
    { 0x0b8, 2, REC_LR_PLAIN },      /* kills */
    { 0x0ba, 2, REC_LR_PLAIN },      /* u0ba */
    { 0x0bc, 4, REC_LR_PLAIN },      /* res00 */
    { 0x0c0, 4, REC_LR_EREQ },       /* e_requested */
    { 0x0c4, 4, REC_LR_EGRANT },     /* e_granted */
    { 0x0c8, 4, REC_LR_PLAIN },      /* res03 */
    { 0x0cc, 4, REC_LR_PLAIN },      /* res04 */
    { 0x0d0, 4, REC_LR_PLAIN },      /* res05 */
    { 0x0d4, 4, REC_LR_PLAIN },      /* res06 */
    { 0x0d8, 4, REC_LR_MREQ },       /* m_requested */
    { 0x0dc, 4, REC_LR_MGRANT },     /* m_granted */
    { 0x0e0, 4, REC_LR_PLAIN },      /* res09 */
    { 0x0e4, 4, REC_LR_PLAIN },      /* res10 */
    { 0x0e8, 4, REC_LR_PLAIN },      /* res11 */
    { 0x0ec, 4, REC_LR_OWNER },      /* store */
    { 0x0f0, 4, REC_LR_PLAIN },      /* attacker */
    { 0x0f4, 1, REC_LR_PLAIN },      /* attacker_player */
    { 0x0f5, 1, REC_LR_PLAIN },      /* hit_kind */
    { 0x0f6, 1, REC_LR_HPPCT },      /* hp_pct */
    { 0x0f7, 1, REC_LR_HPPCT30 },    /* hp_pct_30 */
    { 0x0f8, 1, REC_LR_PLAIN },      /* sight_h */
    { 0x0f9, 1, REC_LR_PLAIN },      /* attach_piece */
    { 0x0fa, 1, REC_LR_COUNT_ },     /* damage_timer */
    { 0x0fb, 4, REC_LR_COUNT_ },     /* give_lock */
    { 0x0ff, 1, REC_LR_PLAIN },      /* player */
    { 0x100, 4, REC_LR_PLAIN },      /* u100 */
    { 0x104, 4, REC_LR_BUILD_LEFT }, /* build_left */
    { 0x108, 2, REC_LR_HP },         /* hp */
    { 0x10a, 2, REC_LR_PLAIN },      /* water */
    { 0x10c, 2, REC_LR_PLAIN },      /* u10c */
    { 0x10e, 1, REC_LR_PLAIN },      /* activation */
    { 0x10f, 1, REC_LR_PLAIN },      /* bits_10f */
    { 0x110, 4, REC_LR_PLAIN },      /* state */
    { 0x114, 4, REC_LR_PLAIN },      /* u114 */
    { 0x118, 4, REC_LR_PLAIN },      /* course */
    { 0x11c, 4, REC_LR_PLAIN },      /* moveclass */
    { 0x120, 4, REC_LR_VX },         /* vx */
    { 0x124, 4, REC_LR_VY },         /* vy */
    { 0x128, 4, REC_LR_VZ },         /* vz */
    { 0x12c, 4, REC_LR_AX },         /* ax */
    { 0x130, 4, REC_LR_AY },         /* ay */
    { 0x134, 4, REC_LR_AZ },         /* az */
    { 0x138, 4, REC_LR_SPEED },      /* speed */
    { 0x13c, 2, REC_LR_TURN },       /* turn */
    { 0x13e, 4, REC_LR_CLAIM },      /* claim */
    { 0x142, 4, REC_LR_STAMP },      /* stamp */
    { 0x146, 1, REC_LR_LAYER },      /* layer */
    { 0x147, 1, REC_LR_PLAIN },      /* pad */
};

#endif
