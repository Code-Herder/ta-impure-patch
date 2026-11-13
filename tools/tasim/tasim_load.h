#ifndef TASIM_LOAD_H
#define TASIM_LOAD_H
/* tasim_load -- the install loader: every definition of an install read from its files through the shared core's
   key table (tagpu/ddraw/src/tasim_keys.h, one rule per key), for every harness that needs an install's definitions:
   tasim_run's `defs`, `roles` and `values`, and through them the calibration tools (defcheck.py, duelgen.py,
   roles_check.py) and the key table's test.

   THE FILES are read through the engine's own file rules (tagpu_vfs: the loose files under the game folder, then the
   archives of the profile's mount list in mount order) and parsed with tagpu_tdf. Nothing is extracted to disk. The
   core itself reads no file: the wiki's host build links every src/tasim_*.c without tagpu_vfs or tagpu_tdf, so the
   loader is the harness's, and it hands the core's readers each section's values through a TS_GET.

   WHAT IT LOADS, each by its engine parser's rule (research/notes/impai/tasim.md, research/notes/engine/keys.md), every
   folder, pattern and extension the profile's (below):
     - <gamedata>\<moveinfo>.<tdf_ext>'s CLASS0..CLASS31 (0x42D328..0x42D3AA), each over 0x4402E0's defaults;
     - every file <units>\<all_pattern>.<unit_ext> lists with a [UNITINFO] unitname that the menu-time pre-scan
       0x42A8D0 keeps (its Version, an archive's file, its Copyright: tasim_load.c, prescan_dropped), defined by the
       file named after its UnitName as the level re-reads it (tasim_load.c, each_unit), through 0x42BF40's rows, its
       move class taken from the class record it names (the class record wins over the unit's own keys,
       0x42CD0C..0x42CD97); the def array sorted by _stricmp of the unitname (0x42D2E0, comparator 0x42DB60) after the
       engine's type 0, `None`, so a type is its row + 1;
     - every section of the files <weapon_pattern> lists, each opened from <weapons>, through 0x42E440's rows, kept in
       the slot its ID names, the later section of one ID replacing the earlier as Impure's loader keeps stock's rule
       (tagpu_patches.c, wpn_loader_id, which also skips an ID past the slots and a name longer than the record), and
       its [DAMAGE] table: the `default` into +0xD4 and every other key a row, a unit name and its damage
       (0x42EFC3..0x42F2EB, DISASSEMBLED: `default` skipped by _stricmp, each row's value through 0x4C46C0 with
       default 0, a dword);
     - every <features>\**\<feature_pattern> section, the first file enumerated holding a name keeping it
       (LoadFeature, engine/map.md);
     - each builder's build options by the sidebar's page rule (tagpu_buildmenu) over its <gui> pages and the
       <download> entries, the side's "<prefix>DL" page standing in for a missing one as 0x41ACE0 chooses;
     - the named-not-modelled report (TS_REPORT): the keys a later landing models, the keys no parser reads, the
       values the core refuses, and the engine behaviours tools/tasim/engine-features.tsv does not mark modelled.

   THE PROFILE. Every name above and the pre-scan's constants are the running image's, each read where the engine's
   code reads it by tools/wiki/tavfs.py (`profile`, *The running image and the profile*: the folder's TotalA.exe with
   each imported Patch Loader's list applied) and handed over as a `tasim-profile/1` file; TASIM_LOAD_IN carries no
   name of its own. The file is UTF-8: the line `tasim-profile/1`, then `key<TAB>value` lines -- `root` (absolute),
   `image` (its description), `language`, `mount` once per archive in mount order (absolute, 0..TAGPU_VFS_ARCSANE of
   them), `units`, `all_pattern`, `unit_ext`, `weapons`, `weapon_pattern`, `tdf_ext`, `gamedata`, `moveinfo`,
   `sidedata`, `download`, `gui`, `gui_ext`, `features`, `feature_pattern`, `version_major` and `version_minor`
   (decimal, -128..127), `archive_only` (0 or 1), `copyright`, `copyright_year` and `copyright_default`. Every key but
   `mount` appears exactly once. tasim_profile_read takes nothing on tavfs's word: it refuses an unknown, repeated or
   missing key, a line without a TAB, a CR in a value and a value past its bound -- empty or not shorter than its
   field, a relative root or mount, a number that is not one of its values, a `weapon_pattern` without something on
   both sides of its last `\`, a `copyright_year` that is not four bytes found in `copyright` at an offset of at most
   0x7B -- naming the key. */
#include <stdint.h>
#include <stdio.h>
#include "tagpu_vfs.h"
#include "tasim_keys.h"

#define TASIM_LOAD_UNITS     4096
#define TASIM_LOAD_WSECTIONS (2 * TS_WEAPON_IDS)    /* weapon sections read, replaced ones included */
#define TASIM_LOAD_FEATURES  8192
#define TASIM_LOAD_DMGNAME   32                     /* a row names a unit type: def +0x20 holds 31 characters */

#define TASIM_PROFILE_NAME 64      /* a folder, pattern, extension or language, its NUL included          */
#define TASIM_PROFILE_TEXT 0x80    /* the Copyright reference and its default: 0x42B0FD copies 0x80 bytes  */

typedef struct {
    char  root[TAGPU_VFS_PATHMAX];
    char  image[256];
    char  language[TASIM_PROFILE_NAME];
    char (*mount)[TAGPU_VFS_PATHMAX];          /* the archives in mount order, tasim_profile_free's  */
    int   nmount;
    char  units[TASIM_PROFILE_NAME], all_pattern[TASIM_PROFILE_NAME], unit_ext[TASIM_PROFILE_NAME];
    char  weapons[TASIM_PROFILE_NAME], weapon_pattern[TASIM_PROFILE_NAME], tdf_ext[TASIM_PROFILE_NAME];
    char  gamedata[TASIM_PROFILE_NAME], moveinfo[TASIM_PROFILE_NAME], sidedata[TASIM_PROFILE_NAME];
    char  download[TASIM_PROFILE_NAME], gui[TASIM_PROFILE_NAME], gui_ext[TASIM_PROFILE_NAME];
    char  features[TASIM_PROFILE_NAME], feature_pattern[TASIM_PROFILE_NAME];
    int   version_major, version_minor;        /* signed bytes, as 0x42B06C and 0x42B083 movsx them   */
    int   archive_only;                        /* 0x50289C nonzero                                    */
    char  copyright[TASIM_PROFILE_TEXT], copyright_year[5], copyright_default[TASIM_PROFILE_TEXT];
} TASIM_PROFILE;

/* 0, or -1 with the reason (the key, the line) in `why`; tasim_profile_free releases it either way. */
int  tasim_profile_read(TASIM_PROFILE* p, const char* path, char* why, size_t nwhy);
void tasim_profile_free(TASIM_PROFILE* p);

typedef struct {
    const TASIM_PROFILE* profile;
    const char*          features;     /* tools/tasim/engine-features.tsv for the report's fourth class, or NULL */
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
    uint32_t*               key;            /* by u's row: def +0x13E, 0x4B6BA0 over the pre-scan's file      */
    uint8_t*                keyed;          /* by u's row: 0 when a .OVR beside that file may replace the key */
    int                     nu;
    int                     dropped_version, dropped_loose, dropped_copyright;  /* FBIs the pre-scan drops */
    int                     reread;         /* kept types defined by a file other than the pre-scan's         */
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
   -1 when the install cannot be read (an archive list, an allocation, a bound passed, a kept type whose own file does
   not open, no unit at all), with the reason on stderr. */
int  tasim_load(TASIM_INSTALL* L, const TASIM_LOAD_IN* in);
void tasim_load_free(TASIM_INSTALL* L);

/* Every value of every key of the files tasim_load reads definitions from (moveinfo, the FBIs the pre-scan reads,
   the weapon and feature TDFs), one a line, in the order it reads them; a value holding a line break is left out.
   The sample tagpu/ddraw/tests/tasim_keys_test.py runs the number getters on. Returns 0, or -1 as tasim_load. */
int  tasim_load_values(const TASIM_LOAD_IN* in, FILE* out);

/* The damage of the weapon in slot `id` against the type named `type`, as 0x499CD0 takes it (DISASSEMBLED
   0x499CEA..0x499D6F): the row whose name equals `type` without case, else the default word +0xD4 zero-extended.
   0 for an empty slot. */
int32_t tasim_load_damage(const TASIM_INSTALL* L, int32_t id, const char* type);

/* 1 when `word` is one of the white-space separated words of `text`, compared as the engine's _stricmp. */
int tasim_load_has_word(const char* text, const char* word);

#endif
