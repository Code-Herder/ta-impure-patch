#ifndef TAGPU_BUILDMENU_H
#define TAGPU_BUILDMENU_H
/* tagpu_buildmenu -- the types a constructor's build menu lets the player arm, by the page
   loader's own rule, for Quick Build (tagpu_resource.c reads each selected constructor's menu
   through it). The engine's sidebar is research/notes/exe-reverse-engineering.md, *The left
   sidebar* and *The build pages*; every address below is DISASSEMBLED there.

   THE RULE, one page at a time, in the engine's order:
     1. The page file is read by position (tagpu_guifile, the reader 0x4AEAC0): S top-level
        sections, one gadget each; HEADER's count is S - 1 = N (0x4AED48), so the records are
        0..N. A record's COMMON (0x4AD350) gives id, active and commonattribs as bytes and the
        name cut to 15 characters; the section's own `grayedout` gives the greyed bit (0x4ADC70).
     2. The download fill (0x41ACE0, 0x41AE24..0x41AEAE): each of this builder's entries whose
        MENU - 1 is the page (MENU 0 never is) writes record BUTTON + 4 -- greyed bit cleared,
        commonattribs 4, the entry's whole name -- later entries over earlier ones. The engine
        writes a slot past N with no bound; nothing reads it, so it is skipped here.
     3. The greyed pass, numbered pages only (0x41B045..0x41B096): records 0..N-1, not N, each
        with commonattribs bit 2 greyed unless 0x488B10 knows its whole name.
     4. A press (the pump offers gadgets 1..N; 0x41AA00 handles it): a button (id 1), active,
        not greyed. Its name is cut to 16 characters (0x49FED0); a name containing PREV, NEXT,
        ORDERS or BUILD, case-sensitive, is a pager or a tab (0x41AA75..0x41AB30); otherwise
        0x488B10's type arms placement when the type's def+0x22F is 0 (0x41AB5D, 0x41AB7F).
   Gadget overlap (the pump's first taker) is not modelled: every active, ungreyed button counts
   as pressable. Nor is the fill's overrun of a name past 16 characters into the record's
   geometry, which this rule never reads.

   BOUNDS: every record index is one the parse made (0..N), a fill slot is tested against N,
   an entry's name is read only when it ends within its 32 bytes, and `offered` is written only
   below `ndefs`. A file's bytes are data: the worst one can make is a refusal.
   No engine memory, no Win32, no state: any thread. */
#include <stddef.h>

#define TAGPU_BM_PAGES 8                 /* page 0 and numbered 1..7: unit+0x110 bits 23..25 (0x41BE7F) */
#define TAGPU_BM_ENTRY_NAME 32           /* a download entry's name field, entry +8 */
#define TAGPU_BM_FILE_CAP (1u << 20)     /* a page file read whole */

/* One download entry of this builder: MENU and BUTTON as the engine's bytes (entry +6, +7). */
typedef struct { unsigned char menu, button; char name[TAGPU_BM_ENTRY_NAME]; } TAGPU_BM_ENTRY;

typedef struct {
    unsigned (*type_of)(void* ctx, const char* name);   /* 0x488B10; 0 unknown; < ndefs */
    int      (*armable)(void* ctx, unsigned type);      /* def(type)+0x22F == 0 (0x41AB7F) */
    void* ctx;
    unsigned ndefs;                                     /* `offered` has this many bytes */
} TAGPU_BM_ENGINE;

/* The pages the sidebar can show: 0 when `orders_page` (def+0x241 bit 31) and count != 0 (the
   router shows nothing for count 0, 0x41B2E0), then 1..min(count-1, 7) (NEXT's 3-bit field
   wraps 7 to 0, 0x41BE7F). Returns how many it wrote. */
unsigned tagpu_buildmenu_pages(int orders_page, unsigned count, unsigned char out[TAGPU_BM_PAGES]);

/* The router's file name "<NAME><page>.GUI", NAME = def+0x20 cut at 31 characters
   (0x41B7A6..0x41B7E9): at most 31 bytes of `def_name` are read. 0 when it does not fit. */
int tagpu_buildmenu_page_name(char* out, size_t cap, const char* def_name, unsigned page);

/* GUI_Load's file: `prefix` (main+0xECF, read to 0x100 bytes as 0x4AA99A copies it) + `name`,
   then 0x4BAFF0's extension rule: cut at the last '.' found scanning back before any '\', then
   ".GUI". 0 when the prefix has no NUL in 0x100 bytes or the path does not fit `cap`. */
int tagpu_buildmenu_gui_path(char* out, size_t cap, const char* prefix, const char* name);

/* One page: parse `blob`, fill it from `entries` (this builder's, in the engine's file order),
   the greyed pass when page != 0, then mark offered[type] = 1 for each type a press arms.
   Returns the types newly marked, or -1 with `why` when the file is refused (the page then
   offers nothing). */
int tagpu_buildmenu_page(const unsigned char* blob, size_t n, unsigned page,
                         const TAGPU_BM_ENTRY* entries, unsigned nentries,
                         const TAGPU_BM_ENGINE* eng, unsigned char* offered, char* why, size_t nwhy);

#endif
