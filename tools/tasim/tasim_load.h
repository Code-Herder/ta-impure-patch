#ifndef TASIM_LOAD_H
#define TASIM_LOAD_H
/* tasim_load -- the install loader: every definition of an install read from its files through the shared core's
   key table (tagpu/ddraw/src/tasim_keys.h, one rule per key), for every harness that needs an install's definitions:
   tasim_run's `defs`, `roles` and `values`, and through them the calibration tools (defcheck.py, duelgen.py,
   roles_check.py) and the key table's test.

   THE FILES are read through the engine's own file rules (tagpu_vfs: the loose files under the game folder, then the
   archives of a mount list, one full path a line, in mount order, as tools/tasim/native.py writes it from
   tools/wiki/tavfs.py) and parsed with tagpu_tdf. Nothing is extracted to disk. The core itself reads no file: the
   wiki's host build links every src/tasim_*.c without tagpu_vfs or tagpu_tdf, so the loader is the harness's, and it
   hands the core's readers each section's values through a TS_GET.

   WHAT IT LOADS, each by its engine parser's rule (research/notes/impai/tasim.md, research/notes/engine/keys.md):
     - gamedata\moveinfo.tdf's CLASS0..CLASS31 (0x42D328..0x42D3AA), each over 0x4402E0's defaults;
     - every units\*.fbi with a [UNITINFO] unitname that the menu-time pre-scan 0x42A8D0 keeps (its Version, an
       archive's file, its Copyright: tasim_load.c, prescan_dropped), through 0x42BF40's rows, its move class taken
       from the class record it names (the class record wins over the unit's own keys, 0x42CD0C..0x42CD97); the def
       array sorted by _stricmp of the unitname (0x42D2E0, comparator 0x42DB60) after the engine's type 0, `None`,
       so a type is its row + 1;
     - every section of weapons\*.tdf through 0x42E440's rows, kept in the slot its ID names, the later section of
       one ID replacing the earlier as Impure's loader keeps stock's rule (tagpu_patches.c, wpn_loader_id, which
       also skips an ID past the slots and a name longer than the record), and its [DAMAGE] table: the `default` into +0xD4 and every other key a row,
       a unit name and its damage (0x42EFC3..0x42F2EB, DISASSEMBLED: `default` skipped by _stricmp, each row's
       value through 0x4C46C0 with default 0, a dword);
     - every features\**.tdf section, the first file enumerated holding a name keeping it (LoadFeature, engine/map.md);
     - each builder's build options by the sidebar's page rule (tagpu_buildmenu) over its GUI pages and the
       download entries, the side's "<prefix>DL" page standing in for a missing one as 0x41ACE0 chooses;
     - the named-not-modelled report (TS_REPORT): the keys a later landing models, the keys no parser reads, the
       values the core refuses, and the engine behaviours tools/tasim/engine-features.tsv does not mark modelled.

   THE FOLDERS a mod's loader renames in memory (ProTA's gamedatP, downloadP and guiP: tools/wiki/sidepacks.py,
   RECORDS) come in TASIM_LOAD_IN; the first mount pattern a mod renames is the mount list's own (native.py
   --archive). */
#include <stdint.h>
#include <stdio.h>
#include "tagpu_vfs.h"
#include "tasim_keys.h"

#define TASIM_LOAD_UNITS     4096
#define TASIM_LOAD_WSECTIONS (2 * TS_WEAPON_IDS)    /* weapon sections read, replaced ones included */
#define TASIM_LOAD_FEATURES  8192
#define TASIM_LOAD_DMGNAME   32                     /* a row names a unit type: def +0x20 holds 31 characters */

typedef struct {
    const char* cwd;                /* the game folder: its loose files                                        */
    const char* mounts;             /* a file of the archives' full paths, one a line, in mount order           */
    const char* language;           /* the engine's language value: the folder variant opened first            */
    const char* gui;                /* the GUI pages' folder, NULL for the executable's `guis`                  */
    const char* gamedata;           /* NULL for `gamedata`                                                      */
    const char* download;           /* NULL for `download`                                                      */
    const char* features;           /* tools/tasim/engine-features.tsv for the report's fourth class, or NULL   */
} TASIM_LOAD_IN;

/* One row of a weapon's [DAMAGE] table: the engine's 8-byte entry {char* name, i32 damage} at +0x64. */
typedef struct {
    char    name[TASIM_LOAD_DMGNAME];
    int32_t damage;
} TASIM_LOAD_DMG;

typedef struct {
    TAGPU_VFS*              vfs;
    TASIM_MOVECLASS         cls[TS_CLASSES];
    TASIM_UNITDEF*          u;              /* the def array past None, sorted: a type is its row + 1         */
    int                     nu;
    int                     dropped_version, dropped_loose, dropped_copyright;  /* FBIs the pre-scan drops */
    TASIM_WEAPONDEF*        w;              /* every section kept, in load order                              */
    int                     nw, replaced, refused;
    const TASIM_WEAPONDEF** ids;            /* the slot table, TS_WEAPON_IDS: an ID's record, NULL for none   */
    int32_t*                dmg_first;      /* by w's index: its first row in `dmg`, and how many             */
    int32_t*                dmg_count;
    TASIM_LOAD_DMG*         dmg;
    int                     ndmg, capdmg;
    TASIM_FEATUREDEF*       f;
    int                     nf;
    int32_t*                opt_first;      /* by u's row: its build options' first in `opt`, and how many    */
    int32_t*                opt_count;
    uint16_t*               opt;            /* rows of `u`                                                    */
    int                     nopt, capopt;
    TS_REPORT*              rep;
} TASIM_INSTALL;

/* Loads the install `in` names into `L`, which tasim_load_free releases whether or not this succeeded. Returns 0, or
   -1 when the install cannot be read (the mount list, an allocation, a bound passed, no unit at all), with the
   reason on stderr. */
int  tasim_load(TASIM_INSTALL* L, const TASIM_LOAD_IN* in);
void tasim_load_free(TASIM_INSTALL* L);

/* Every value of every key of the files tasim_load reads definitions from (moveinfo.tdf, the FBIs, the weapon and
   feature TDFs), one a line, in the order it reads them; a value holding a line break is left out. The sample
   tagpu/ddraw/tests/tasim_keys_test.py runs the number getters on. Returns 0, or -1 as tasim_load. */
int  tasim_load_values(const TASIM_LOAD_IN* in, FILE* out);

/* The damage of the weapon in slot `id` against the type named `type`, as 0x499CD0 takes it (DISASSEMBLED
   0x499CEA..0x499D6F): the row whose name equals `type` without case, else the default word +0xD4 zero-extended.
   0 for an empty slot. */
int32_t tasim_load_damage(const TASIM_INSTALL* L, int32_t id, const char* type);

/* 1 when `word` is one of the white-space separated words of `text`, compared as the engine's _stricmp. */
int tasim_load_has_word(const char* text, const char* word);

#endif
