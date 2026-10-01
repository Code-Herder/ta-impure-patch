/* tagpu_replay_globals.h -- GENERATED from tagpu/ddraw/replay-globals.tsv by
   tools/replay-globals-gen.py, which says what each column means. Edit the file and
   regenerate, never this header: the build re-derives it and fails when the two disagree. */
#ifndef TAGPU_REPLAY_GLOBALS_H
#define TAGPU_REPLAY_GLOBALS_H

#define TAGPU_REPLAY_GLOBALS_SHA256 "07efd13967ae31d68559e8a3c6a2da73e5c985d21a2313f49bce8e7abe1c9c0f"
#define TAGPU_REPLAY_GLOBALS_GEN_SHA256 "bb9e21b395feac70b5b3bc21f12ce745b6093ce328e9efdaf2bd58a496e62989"

#define REC_GLOBALS_DWORDS 45    /* the globals image */
#define REC_GLOBALS_SLOTS  27

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
    { RG_MAIN, 1, 4, 0x37ec4, 0xFFFFFFFFu, 0x0u, 0x0u, "windnext" },
    { RG_MAIN, 2, 4, 0x37ec8, 0xFFFFFFFFu, 0x0u, 0x0u, "winddivisor" },
    { RG_MAIN, 3, 4, 0x37ecc, 0xFFFFFFFFu, 0x0u, 0x0u, "windx" },
    { RG_MAIN, 4, 4, 0x37ed0, 0xFFFFFFFFu, 0x0u, 0x0u, "windy" },
    { RG_MAIN, 5, 4, 0x37ed4, 0xFFFFFFFFu, 0x0u, 0x0u, "windz" },
    { RG_MAIN, 6, 2, 0x37ed8, 0xffffu, 0x0u, 0x0u, "windheading" },
    { RG_MAIN, 7, 4, 0x37eda, 0xFFFFFFFFu, 0x0u, 0x0u, "windspeed" },
    { RG_MAIN, 8, 4, 0x37ede, 0xFFFFFFFFu, 0x0u, 0x0u, "windratio" },
    { RG_MAIN, 9, 4, 0x37ee2, 0xFFFFFFFFu, 0x0u, 0x0u, "windchanged" },
    { RG_MAIN, 10, 2, 0x39239, 0x0u, 0xffffu, 0x0u, "endcountdown" },
    { RG_MAIN, 11, 2, 0x3923b, 0x3u | (0x1u << 3) | (0x7ffu << 5), (0x1u << 2) | (0x1u << 4), 0x0u, "endflags+endbits" },
    { RG_DATA, 12, 40, 0x109e8, 0xFFFFFFFFu, 0x0u, 0x0u, "pfbudget" },
    { RG_DATA, 22, 40, 0x10a10, 0xFFFFFFFFu, 0x0u, 0x0u, "pfsearches" },
    { RG_DATA, 32, 4, 0x10a38, 0xFFFFFFFFu, 0x0u, 0x0u, "pfcalls" },
    { RG_DATA, 33, 4, 0x112e8, 0xFFFFFFFFu, 0x0u, 0x0u, "meteornext" },
    { RG_DATA, 34, 4, 0x11318, 0xFFFFFFFFu, 0x0u, 0x0u, "meteoractive" },
    { RG_DATA, 35, 4, 0x1131c, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorend" },
    { RG_DATA, 36, 4, 0x11320, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorp2" },
    { RG_DATA, 37, 4, 0x1132c, 0xFFFFFFFFu, 0x0u, 0x0u, "meteoron" },
    { RG_DATA, 38, 4, 0x11330, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorstrike" },
    { RG_DATA, 39, 4, 0x11334, 0xFFFFFFFFu, 0x0u, 0x0u, "meteorp1" },
    { RG_DATA, 40, 4, 0x1d53c, 0xFFFFFFFFu, 0x0u, 0x0u, "resbroadcast" },
    { RG_DATA, 41, 4, 0x1d684, 0xFFFFFFFFu, 0x0u, 0x0u, "sitebyte" },
    { RG_DATA, 42, 4, 0x1d688, 0xFFFFFFFFu, 0x0u, 0x0u, "sitesum" },
    { RG_DATA, 43, 4, 0x1d6c4, 0xFFFFFFFFu, 0x0u, 0x0u, "cdtimer" },
    { RG_DATA, 44, 4, 0x1ec88, 0xFFFFFFFFu, 0x0u, 0x0u, "rngseed" },
};

#endif
