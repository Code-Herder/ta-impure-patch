/* tagpu_replay_globals.h -- GENERATED from tagpu/ddraw/replay-globals.tsv by
   tools/replay-globals-gen.py, which says what each column means. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_REPLAY_GLOBALS_H
#define TAGPU_REPLAY_GLOBALS_H

#define TAGPU_REPLAY_GLOBALS_SHA256 "3166ecc0c911e30356e1ffb9686066d03a4bbeb3ac4f660bb90663d33b9dfa94"
#define TAGPU_REPLAY_GLOBALS_GEN_SHA256 "e51991b708bde1539c6b9a370ec7d004c6316255222727dca6f2d536be73d8dd"

#define REC_GLOBALS_DWORDS 78    /* the globals image */
#define REC_GLOBALS_SLOTS  60

enum { RG_MAIN, RG_DATA };   /* a slot's offset: from main, or from .data's start */

/* One field of the globals image. Its bits are the union of the three masks, each over the
   field as a little-endian integer; a field wider than four bytes is one row's, and that
   row's class's mask is then 0xFFFFFFFF, every byte. */
typedef struct ReplayGlobalSlot {
    unsigned char  where;
    unsigned char  dword;     /* its first dword in the image */
    unsigned short size;      /* bytes */
    unsigned int   offset;
    unsigned int   write, hold, react;   /* what a puppet host does with each bit */
    const char*    name;
} ReplayGlobalSlot;

static const ReplayGlobalSlot tagpu_replay_globals[REC_GLOBALS_SLOTS] = {
    { RG_MAIN, 0, 2, 0x14281, (0xfffu << 4), 0x0u, 0x7u, "lostype+losrest" },
    { RG_MAIN, 1, 4, 0x14353, 0xFFFFFFFFu, 0x0u, 0x0u, "unitcount" },
    { RG_MAIN, 2, 4, 0x37ec4, 0xFFFFFFFFu, 0x0u, 0x0u, "windnext" },
    { RG_MAIN, 3, 4, 0x37ec8, 0xFFFFFFFFu, 0x0u, 0x0u, "winddivisor" },
    { RG_MAIN, 4, 4, 0x37ecc, 0xFFFFFFFFu, 0x0u, 0x0u, "windx" },
    { RG_MAIN, 5, 4, 0x37ed0, 0xFFFFFFFFu, 0x0u, 0x0u, "windy" },
    { RG_MAIN, 6, 4, 0x37ed4, 0xFFFFFFFFu, 0x0u, 0x0u, "windz" },
    { RG_MAIN, 7, 2, 0x37ed8, 0xffffu, 0x0u, 0x0u, "windheading" },
    { RG_MAIN, 8, 4, 0x37eda, 0xFFFFFFFFu, 0x0u, 0x0u, "windspeed" },
    { RG_MAIN, 9, 4, 0x37ede, 0xFFFFFFFFu, 0x0u, 0x0u, "windratio" },
    { RG_MAIN, 10, 4, 0x37ee2, 0xFFFFFFFFu, 0x0u, 0x0u, "windchanged" },
    { RG_MAIN, 11, 2, 0x39239, 0x0u, 0xffffu, 0x0u, "endcountdown" },
    { RG_MAIN, 12, 2, 0x3923b, 0x3u | (0x1u << 3) | (0x7ffu << 5), (0x1u << 2) | (0x1u << 4), 0x0u, "endflags+endbits" },
    { RG_DATA, 13, 40, 0x109e8, 0xFFFFFFFFu, 0x0u, 0x0u, "pfbudget" },
    { RG_DATA, 23, 40, 0x10a10, 0xFFFFFFFFu, 0x0u, 0x0u, "pfsearches" },
    { RG_DATA, 33, 4, 0x10a38, 0xFFFFFFFFu, 0x0u, 0x0u, "pfcalls" },
    { RG_DATA, 34, 4, 0x112e8, 0xFFFFFFFFu, 0x0u, 0x0u, "meteornext" },
    { RG_DATA, 35, 4, 0x11318, 0xFFFFFFFFu, 0x0u, 0x0u, "meteoractive" },
    { RG_DATA, 36, 4, 0x1131c, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorend" },
    { RG_DATA, 37, 4, 0x11320, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorp2" },
    { RG_DATA, 38, 4, 0x1132c, 0xFFFFFFFFu, 0x0u, 0x0u, "meteoron" },
    { RG_DATA, 39, 4, 0x11330, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorstrike" },
    { RG_DATA, 40, 4, 0x11334, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorp1" },
    { RG_DATA, 41, 4, 0x11374, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark00" },
    { RG_DATA, 42, 4, 0x11394, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark01" },
    { RG_DATA, 43, 4, 0x113b4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark02" },
    { RG_DATA, 44, 4, 0x113d4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark03" },
    { RG_DATA, 45, 4, 0x113f4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark04" },
    { RG_DATA, 46, 4, 0x11414, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark05" },
    { RG_DATA, 47, 4, 0x11434, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark06" },
    { RG_DATA, 48, 4, 0x11454, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark07" },
    { RG_DATA, 49, 4, 0x11474, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark08" },
    { RG_DATA, 50, 4, 0x11494, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark09" },
    { RG_DATA, 51, 4, 0x114b4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark10" },
    { RG_DATA, 52, 4, 0x114d4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark11" },
    { RG_DATA, 53, 4, 0x114f4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark12" },
    { RG_DATA, 54, 4, 0x11514, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark13" },
    { RG_DATA, 55, 4, 0x11534, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark14" },
    { RG_DATA, 56, 4, 0x11554, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark15" },
    { RG_DATA, 57, 4, 0x11574, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark16" },
    { RG_DATA, 58, 4, 0x11594, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark17" },
    { RG_DATA, 59, 4, 0x115b4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark18" },
    { RG_DATA, 60, 4, 0x115d4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark19" },
    { RG_DATA, 61, 4, 0x115f4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark20" },
    { RG_DATA, 62, 4, 0x11614, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark21" },
    { RG_DATA, 63, 4, 0x11634, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark22" },
    { RG_DATA, 64, 4, 0x11654, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark23" },
    { RG_DATA, 65, 4, 0x11674, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark24" },
    { RG_DATA, 66, 4, 0x11694, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark25" },
    { RG_DATA, 67, 4, 0x116b4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark26" },
    { RG_DATA, 68, 4, 0x116d4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark27" },
    { RG_DATA, 69, 4, 0x116f4, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark28" },
    { RG_DATA, 70, 4, 0x11714, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark29" },
    { RG_DATA, 71, 4, 0x11734, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark30" },
    { RG_DATA, 72, 4, 0x11754, 0xFFFFFFFFu, 0x0u, 0x0u, "mcwatermark31" },
    { RG_DATA, 73, 4, 0x1d53c, 0xFFFFFFFFu, 0x0u, 0x0u, "resbroadcast" },
    { RG_DATA, 74, 4, 0x1d684, 0xFFFFFFFFu, 0x0u, 0x0u, "sitebyte" },
    { RG_DATA, 75, 4, 0x1d688, 0xFFFFFFFFu, 0x0u, 0x0u, "sitesum" },
    { RG_DATA, 76, 4, 0x1d6c4, 0xFFFFFFFFu, 0x0u, 0x0u, "cdtimer" },
    { RG_DATA, 77, 4, 0x1ec88, 0xFFFFFFFFu, 0x0u, 0x0u, "rngseed" },
};

#endif
