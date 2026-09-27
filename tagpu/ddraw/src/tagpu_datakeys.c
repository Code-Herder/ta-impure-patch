/* tagpu_datakeys.c — TADR section C's keys: the FBI reader, the build
   ghost's piece mask, the weapon keys' store and decisions, and
   nomapweaponalert's silence. The plan is research/notes/tadr-port/data-keys.md; the
   engine facts each block rests on are in data-keys-evidence.md (Part 3 §1,
   Part 4 §2) and in exe-reverse-engineering.md. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "tagpu_datakeys.h"
#include "tagpu_engine.h"
#include "tagpu_detour.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_model3do.h"
#include "tagpu_packet_pub.h"

static void dlog(const char* fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    b[sizeof b - 1] = 0;
    tagpu_logf("datakeys: %s", b);
}

static int ptr_ok(const void* p) { return tagpu_m3_ptr_ok(p); }

/* =========================================================================
   1. The unit-key records
   ========================================================================= */

/* ONE RECORD PER UnitDef SLOT, holding only what THIS load's FBI loader
   wrote. Three observers keep it so (section 2): the start of the unit-data
   load 0x42D2E0 empties every record; the FBI loader's entry 0x42BF40 resets
   its slot's record -- a fresh serial and the def it is loading -- whether or
   not the file then opens; and the read site 0x42BF97, reached only when it
   does, fills in the keys. So a slot the load skips (no FBI file, 0x42D71A)
   has no record rather than an earlier game's at the same def address, and a
   console reload moves the serial even when its FBI fails to open -- the
   reload that still replaces the COB (0x42D269 runs before 0x42D275). A
   reader asks for slot `type` and takes the row only when its `def` is the
   def it asked about.

   WHO WRITES AND WHO READS, AND WHY THEY NEVER OVERLAP. The level's unit-data
   load 0x42D2E0 runs the FBI loader on the LOADER thread (LoadGameData_Main
   0x4917D0, called from the loader body 0x497180); the console's one-type
   reload 0x417490 -> 0x42D1F0 runs it on the GAME thread. Every reader is on
   the game thread: the in-play packet fill, the veterancy sites of the
   simulation and the two kill lines of the unit panels. The loader thread writes
   between a level's teardown and its first in-play draw, which is the same
   ordering every game-thread read of the def array itself rests on (the
   in-play publish point, exe-reverse-engineering.md); the reload is on the
   reader's own thread. The render thread never reads a record.

   The veterancy keys make these records simulation state: the reader is
   installed before the fail-closed table is written, and the table refuses
   to install without it (tagpu_patches.c, fix_veterancy). */
#define DK_VET_MAX 32       /* thresholds a list may carry                    */

typedef struct DkUnit {
    const char* def;        /* the UnitDef this row was written for           */
    unsigned    serial;     /* a fresh number at every write: the mask cache's
                               key, so a reloaded FBI is re-read              */
    char*       pp;         /* PreviewPieces=, parsed: pp_n lower-case names,
                               each NUL-ended, back to back; NULL = absent    */
    unsigned    pp_n;
    unsigned short vthr[DK_VET_MAX]; /* VeterancyThresholds=, strictly
                               increasing, each 1..65535                      */
    unsigned char  vthr_n;  /* 0 = absent or refused: the sites run stock's
                               own instructions for this type                 */
    unsigned char  vrate_on;/* VeterancyAccuracyBuffRate= accepted           */
    unsigned short vrate;   /* 0 = no accuracy buff                           */
} DkUnit;

static DkUnit   s_unit[TAGPU_LIM_TYPES];
static unsigned s_serial;

static void unit_clear(DkUnit* r)
{
    free(r->pp);
    r->pp = NULL; r->pp_n = 0;
    r->vthr_n = 0; r->vrate_on = 0; r->vrate = 0;
}

/* THE COB'S LENGTH, which the loaded script does not carry. 0x4B2450 reads
   the file whole through 0x4BBE50, which sizes the block from the file's
   archive entry or its filelength, and then checksums it with
   0x4B6BA0(buf, size) at 0x4B2477, `size` being 0x4BBC40's answer for the
   same path -- the same lookup 0x4BBE50 sized the block with. So (buf, size)
   at that call is the block and its length, by the engine's own contract: it
   reads every byte of it there. DISASSEMBLED. The two answers are two opens
   of one file, so they agree while the game's files hold still; a loose
   script rewritten by another program between them would make the engine's
   own checksum read past the block before any read of ours.

   Keyed by the block's address, so a pointer that did not come out of
   0x4B2450 has no length and gets no mask. Emptied at the start of every
   unit-data load, which loads every script afresh; a reload's freed block
   keeps its row until its address comes back, when the new block's row
   replaces it. The same threads and the same ordering as the records. */
#define DK_COB_SLOTS (TAGPU_LIM_TYPES * 2)
typedef char dk_cob_slots_pow2[(DK_COB_SLOTS & (DK_COB_SLOTS - 1)) == 0 ? 1 : -1];
typedef struct DkCob { const char* cob; unsigned size; } DkCob;
static DkCob s_cob[DK_COB_SLOTS];

static unsigned cob_hash(const char* cob)
{
    unsigned h = (unsigned)(size_t)cob * 2654435761u;
    return h ^ (h >> 15);
}

static void cob_put(const char* cob, unsigned size)
{
    unsigned h = cob_hash(cob), i;
    for (i = 0; i < DK_COB_SLOTS; i++) {
        DkCob* e = &s_cob[(h + i) & (DK_COB_SLOTS - 1)];
        if (!e->cob || e->cob == cob) { e->cob = cob; e->size = size; return; }
    }
    {
        static int once;
        if (!once) { once = 1; dlog("COB lengths: the table is full; a script loaded now gets no mask (logged once)"); }
    }
}

/* the recorded length of the block at `cob`, 0 when none was recorded */
static unsigned cob_size(const char* cob)
{
    unsigned h = cob_hash(cob), i;
    for (i = 0; i < DK_COB_SLOTS; i++) {
        const DkCob* e = &s_cob[(h + i) & (DK_COB_SLOTS - 1)];
        if (e->cob == cob) return e->size;
        if (!e->cob) return 0;
    }
    return 0;
}

/* the slot a def pointer names: a stride-aligned member of the engine's def
   array, inside the slots this DLL raised the array to */
static int def_slot(const char* def, unsigned* slot)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    const char* base;
    size_t d;
    if (!ptr_ok(ta)) return 0;
    base = *(const char* const*)(ta + OFF_UNITDEFS);
    if (!ptr_ok(base) || def < base) return 0;
    d = (size_t)(def - base);
    if (d % UDEF_STRIDE) return 0;
    d /= UDEF_STRIDE;
    if (d >= TAGPU_LIM_TYPES) return 0;
    *slot = (unsigned)d;
    return 1;
}

/* =========================================================================
   2. The observers: the load's start, the FBI loader, the COB checksum and
      the FBI reader
   ========================================================================= */

/* THE LOAD'S START, 0x42D2E0's entry (`sub esp,0x610`): the unit-data load's
   one caller is 0x4918CA on the LOADER thread, and its per-type loop runs
   every FBI and every COB of the game after this point. */
#define VA_UNIT_LOAD  0x0042D2E0u
static const unsigned char UNIT_LOAD_STOLEN[6] = { 0x81, 0xEC, 0x10, 0x06, 0x00, 0x00 };

static int __cdecl unit_load_begin(void* esp)
{
    unsigned i;
    (void)esp;
    for (i = 0; i < TAGPU_LIM_TYPES; i++) unit_clear(&s_unit[i]);
    memset(s_unit, 0, sizeof s_unit);
    memset(s_cob, 0, sizeof s_cob);
    return 0;
}

/* THE FBI LOADER'S ENTRY, 0x42BF40(path, def) (`sub esp,0x518`), before the
   open that can fail (0x42BF66) or find no [UNITINFO] (0x42BF7C). */
#define VA_FBI_LOADER 0x0042BF40u
static const unsigned char FBI_LOADER_STOLEN[6] = { 0x81, 0xEC, 0x18, 0x05, 0x00, 0x00 };

static int __cdecl fbi_begin(void* esp)
{
    const char* def = (const char*)((void* const*)esp)[2];
    unsigned slot;
    DkUnit* r;
    if (!def_slot(def, &slot)) {
        static int once;
        if (!once) { once = 1; dlog("FBI loader: a def outside the def array's slots, skipped (logged once)"); }
        return 0;
    }
    r = &s_unit[slot];
    unit_clear(r);
    r->serial = ++s_serial;
    r->def = def;
    return 0;
}

/* THE COB CHECKSUM, 0x4B6BA0(buf, size) (`sub esp,0xC; push edi; mov edi,
   [esp+0x18]`): eleven callers, and only the one that returns into the COB
   loader (0x4B247C) is a script's block. */
#define VA_CHECKSUM     0x004B6BA0u
#define RA_COB_CHECKSUM 0x004B247Cu
static const unsigned char CHECKSUM_STOLEN[8] = { 0x83, 0xEC, 0x0C, 0x57, 0x8B, 0x7C, 0x24, 0x18 };

static int __cdecl cob_checksummed(void* esp)
{
    void* const* a = (void* const*)esp;
    int size = (int)(size_t)a[2];
    if ((unsigned)(size_t)a[0] == RA_COB_CHECKSUM && a[1] && size > 0)
        cob_put((const char*)a[1], (unsigned)size);
    return 0;
}

/* THE READ SITE. 0x42BF40(path, def) opens the FBI and finds its [UNITINFO]
   section; at 0x42BF8C..0x42BF93 it takes `ebp = def` and `ecx = the section`
   and at 0x42BF97 starts pushing the arguments of its first TDF read
   (UnitName, 0x42BFA7). Both registers are live there and nothing has been
   read from the file yet. The stolen instruction is `push 0x5119B8`, which
   carries no relative operand, so it runs unchanged from the stub.
   DISASSEMBLED; callers 0x42D722 (the level's load) and 0x42D269 (the
   one-type reload 0x42D1F0). */
#define VA_FBI_READ   0x0042BF97u
static const unsigned char FBI_STOLEN[5] = { 0x68, 0xB8, 0x19, 0x51, 0x00 };

/* The engine's TDF string reader, thiscall on the section. Found: strncpy of
   `len` bytes, `buf[len-1] = 0`, returns 1. Absent: an UNBOUNDED copy of
   `dflt`, returns 0 — so the default is always "". DISASSEMBLED
   0x4C48C0..0x4C4991. */
typedef int (__thiscall *PFN_TdfGetStr)(void* sec, char* buf, const char* key, int len,
                                         const char* dflt);
#define E_TdfGetStr ((PFN_TdfGetStr)0x004C48C0u)

/* TADR reads every unit key into a 1024-byte buffer; a value that fills ours
   may have been cut by the reader, and a list is taken whole or not at all */
#define DK_VALUE_CAP 1024
/* TADR compares a piece name cut at 63 characters, so a listed name longer
   than that can never match: the list that carries one is refused */
#define DK_NAME_CAP  63

/* PreviewPieces='s value split as TADR splits it — spaces, tabs, line ends,
   ',' and ';' separate; names fold to lower case — into `out`, back to back,
   each NUL-ended. Returns the name count (0 = no names), or -1 for a name
   longer than DK_NAME_CAP. `out` needs strlen(v) + 1 bytes. */
static int parse_pieces(const char* v, char* out, unsigned* outlen)
{
    unsigned o = 0, len = 0;
    int n = 0;
    for (;; v++) {
        char c = *v;
        if (!c || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';') {
            if (len) {
                if (len > DK_NAME_CAP) return -1;
                out[o++] = 0;
                n++;
                len = 0;
            }
            if (!c) break;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[o++] = c;
        len++;
    }
    *outlen = o;
    return n;
}

/* the name the log names a def by: its UnitName, read from the same section,
   because at this site the loader has not stored it yet */
static void fbi_name(void* sec, char* nm, int cap)
{
    nm[0] = 0;
    E_TdfGetStr(sec, nm, "UnitName", cap, "");
    if (!nm[0]) { strncpy(nm, "?", (size_t)cap); nm[cap - 1] = 0; }
}

/* A whole number from lo to 65535, written in decimal digits alone: no sign,
   no fraction, no suffix. */
static int parse_u16(const char* s, unsigned len, unsigned lo, unsigned* out)
{
    unsigned v = 0, i;
    if (!len || len > 5) return 0;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v < lo || v > 65535) return 0;
    *out = v;
    return 1;
}

static int vet_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* VeterancyThresholds='s value, split on white space as TADR splits it, taken
   whole or not at all: 1..DK_VET_MAX whole numbers from 1 to 65535, strictly
   increasing. Returns NULL with the list stored, or why it is refused. */
static const char* parse_thresholds(const char* v, DkUnit* r)
{
    unsigned short thr[DK_VET_MAX];
    unsigned n = 0, t;
    while (*v) {
        const char* s;
        if (vet_space(*v)) { v++; continue; }
        s = v;
        while (*v && !vet_space(*v)) v++;
        if (!parse_u16(s, (unsigned)(v - s), 1, &t)) return "a threshold is not a whole number from 1 to 65535";
        if (n == DK_VET_MAX) return "it lists more than 32 thresholds";
        if (n && t <= thr[n - 1]) return "the thresholds do not strictly increase";
        thr[n++] = (unsigned short)t;
    }
    if (!n) return "it names no threshold";
    memcpy(r->vthr, thr, n * sizeof thr[0]);
    r->vthr_n = (unsigned char)n;
    return NULL;
}

static void read_veterancy(DkUnit* r, void* sec)
{
    char v[DK_VALUE_CAP], nm[33];
    const char* why;
    unsigned rate;

    v[0] = 0;
    if (E_TdfGetStr(sec, v, "VeterancyThresholds", DK_VALUE_CAP, "")) {
        fbi_name(sec, nm, (int)sizeof nm);
        why = strlen(v) >= DK_VALUE_CAP - 1 ? "it fills the reader and may be cut"
                                            : parse_thresholds(v, r);
        if (why) dlog("%s: VeterancyThresholds= refused (%s); the type keeps stock's", nm, why);
        else     dlog("%s: VeterancyThresholds= %u level(s), the first at %u kill(s)", nm,
                      (unsigned)r->vthr_n, (unsigned)r->vthr[0]);
    }
    v[0] = 0;
    if (E_TdfGetStr(sec, v, "VeterancyAccuracyBuffRate", DK_VALUE_CAP, "")) {
        const char *s = v, *e, *t;
        fbi_name(sec, nm, (int)sizeof nm);
        while (vet_space(*s)) s++;
        for (e = s; *e && !vet_space(*e); e++) {}
        for (t = e; vet_space(*t); t++) {}
        if (*t || !parse_u16(s, (unsigned)(e - s), 0, &rate)) {
            dlog("%s: VeterancyAccuracyBuffRate= refused (not a whole number from 0 to 65535); "
                 "the type keeps stock's", nm);
        } else {
            r->vrate_on = 1;
            r->vrate = (unsigned short)rate;
            dlog("%s: VeterancyAccuracyBuffRate= %u%s", nm, rate, rate ? "" : " (no accuracy buff)");
        }
    }
}

static void __cdecl fbi_at_read(const unsigned int* regs)
{
    const char* def = (const char*)(size_t)regs[2];      /* ebp */
    void*       sec = (void*)(size_t)regs[6];            /* ecx */
    char        v[DK_VALUE_CAP];
    char        names[DK_VALUE_CAP + 1];
    char        nm[33];
    unsigned    slot, len = 0;
    DkUnit*     r;
    int         n;

    /* the entry observer reset this record for this call: the reader lands
       only with it */
    if (!def_slot(def, &slot)) return;
    r = &s_unit[slot];
    if (r->def != def) return;

    read_veterancy(r, sec);

    v[0] = 0;
    if (!E_TdfGetStr(sec, v, "PreviewPieces", DK_VALUE_CAP, "")) return;
    fbi_name(sec, nm, (int)sizeof nm);
    if (strlen(v) >= DK_VALUE_CAP - 1) {
        dlog("%s: PreviewPieces= fills the %d-byte reader and may be cut; ignored", nm, DK_VALUE_CAP);
        return;
    }
    n = parse_pieces(v, names, &len);
    if (n < 0) {
        dlog("%s: PreviewPieces= names a piece longer than %d characters, which no piece name "
             "can equal; ignored", nm, DK_NAME_CAP);
        return;
    }
    if (n == 0) { dlog("%s: PreviewPieces= names no piece; ignored", nm); return; }
    r->pp = (char*)malloc(len);
    if (!r->pp) { dlog("%s: PreviewPieces= could not be stored (out of memory); ignored", nm); return; }
    memcpy(r->pp, names, len);
    r->pp_n = (unsigned)n;
    dlog("%s: PreviewPieces= %d piece name(s)", nm, n);
}

static int fbi_install(void)
{
    unsigned char* s;
    unsigned char* p;
    if (!tagpu_detour_bytes_ok(VA_FBI_READ, FBI_STOLEN, sizeof FBI_STOLEN)) return 0;
    s = p = tagpu_detour_stub();
    if (!s) return 0;
    *p++ = 0x60;                                            /* pushad           */
    *p++ = 0x54;                                            /* push esp         */
    *p++ = 0xE8;                                            /* call             */
    tagpu_detour_rel(p, (unsigned int)(size_t)fbi_at_read); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                  /* add esp,4        */
    *p++ = 0x61;                                            /* popad            */
    memcpy(p, FBI_STOLEN, sizeof FBI_STOLEN); p += sizeof FBI_STOLEN;
    *p++ = 0xE9; tagpu_detour_rel(p, VA_FBI_READ + (unsigned)sizeof FBI_STOLEN); p += 4;
    if (!tagpu_detour_land(VA_FBI_READ, s, (int)sizeof FBI_STOLEN)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static int observe(unsigned va, const unsigned char* stolen, int n, tagpu_detour_before_fn fn)
{
    return tagpu_detour_bytes_ok(va, stolen, n) && tagpu_detour_observe(va, stolen, n, fn, NULL);
}

/* Each part lands only on the parts it rests on. The load's start empties
   both tables, so without it nothing else lands. Without the checksum
   observer no script has a length and no Create() mask is computed: a draw
   difference, logged. Without the loader's entry the reader does not land, so
   no record can outlive its load. The reader carries the veterancy keys, so
   the fail-closed table installs it first and refuses to install when it is
   not armed (tagpu_patches.c, fix_veterancy): a player whose reader failed
   would play stock veterancy for keyed types. Asked again, it answers what
   the first call did. */
static int s_unitsState;                    /* 0 not tried, 1 armed, -1 not */

int tagpu_datakeys_units_install(void)
{
    if (s_unitsState) return s_unitsState > 0;
    s_unitsState = -1;
    if (!observe(VA_UNIT_LOAD, UNIT_LOAD_STOLEN, (int)sizeof UNIT_LOAD_STOLEN, unit_load_begin)) {
        dlog("the unit keys NOT installed: the bytes at 0x42D2E0 are not the retail unit-data "
             "load's");
        return 0;
    }
    if (observe(VA_CHECKSUM, CHECKSUM_STOLEN, (int)sizeof CHECKSUM_STOLEN, cob_checksummed))
        dlog("COB lengths recorded at 0x4B6BA0's call from 0x4B2450 (the Create() mask)");
    else
        dlog("COB lengths NOT recorded: the bytes at 0x4B6BA0 are not the retail checksum's; "
             "no Create() mask is computed, every ghost shows every piece its list keeps");
    if (!observe(VA_FBI_LOADER, FBI_LOADER_STOLEN, (int)sizeof FBI_LOADER_STOLEN, fbi_begin) ||
        !fbi_install()) {
        dlog("the unit keys NOT installed: the bytes at 0x42BF40 or 0x42BF97 are not the retail "
             "loader's, or a write failed");
        return 0;
    }
    s_unitsState = 1;
    dlog("the unit keys read at 0x42BF40 and 0x42BF97 (PreviewPieces=, VeterancyThresholds=, "
         "VeterancyAccuracyBuffRate=), emptied at 0x42D2E0");
    return 1;
}

static void alert_install(void);
static void vet_panels_install(void);

void tagpu_datakeys_init(void)
{
    alert_install();
    vet_panels_install();
    tagpu_datakeys_units_install();
}

/* =========================================================================
   3. The build ghost's piece mask
   ========================================================================= */

/* THE COB AS 0x4B2450 LEAVES IT: the file read whole, its header's offsets
   turned into pointers in place (0x4B24A7..0x4B2527, DISASSEMBLED). The two
   name tables have every entry relocated too; the entry-point table does not,
   its entries being code offsets in dwords. Nothing in the loader checks a
   count or an offset, so every one of them is DATA here, and every read below
   is bounded by the block's recorded length (section 1): the header, each
   table whole, each name to its NUL. */
#define COB_VERSION   0x00
#define COB_NSCRIPTS  0x04
#define COB_NPIECES   0x08
#define COB_CODELEN   0x0C      /* dwords                                      */
#define COB_ENTRIES   0x18      /* int32[nscripts]: code offsets, dwords       */
#define COB_SNAMES    0x1C      /* char*[nscripts]                             */
#define COB_PNAMES    0x20      /* char*[npieces]                              */
#define COB_CODE      0x24      /* int32[codelen]                              */
#define COB_HEADER    0x2C      /* the last header word the loader reads, +4   */

/* the piece bits a script can carry, an order of magnitude above stock (the
   largest stock model has 36 pieces); a script past it gets no mask rather
   than part of one */
#define DK_MAX_COBPIECES 1024
#define DK_NAMECMP       256    /* the longest name a compare reads            */

/* the bytes from `p` to the end of the block at `cob`, 0 when `p` is outside
   it (a `p` below `cob` wraps past `size`) */
static unsigned cob_room(const char* cob, unsigned size, const void* p)
{
    size_t o = (size_t)p - (size_t)cob;
    return o < size ? (unsigned)(size - o) : 0u;
}

#define OP_SHOW 0x10005000u
#define OP_HIDE 0x10006000u

/* The opcodes whose length is certain, with their inline operand counts: the
   prologue walk stops at the first opcode not here. The same table as
   tools/ta3do's COB_PROLOGUE, which measured the stock hides with it. */
static const struct { unsigned op; unsigned char inl; } PROLOGUE[] = {
    { OP_SHOW, 1 }, { OP_HIDE, 1 },
    { 0x10007000u, 1 }, { 0x10008000u, 1 },                   /* CACHE, DONT_CACHE      */
    { 0x1000D000u, 1 }, { 0x1000E000u, 1 },                   /* SHADE, DONT_SHADE      */
    { 0x10001000u, 2 }, { 0x10002000u, 2 },                   /* MOVE, TURN             */
    { 0x1000B000u, 2 }, { 0x1000C000u, 2 },                   /* MOVE_NOW, TURN_NOW     */
    { 0x10021001u, 1 }, { 0x10021002u, 1 }, { 0x10021004u, 1 }, /* PUSH_CONSTANT/LOCAL/STATIC */
    { 0x10023002u, 1 }, { 0x10023004u, 1 },                   /* POP_LOCAL_VAR, POP_STATIC */
    { 0x10022000u, 0 }, { 0x10024000u, 0 },                   /* CREATE_LOCAL_VAR, POP_STACK */
    { 0x10031000u, 0 }, { 0x10032000u, 0 }, { 0x10033000u, 0 },
    { 0x10034000u, 0 }, { 0x10034001u, 0 },
};

static int op_inline(unsigned op)
{
    unsigned i;
    for (i = 0; i < sizeof PROLOGUE / sizeof PROLOGUE[0]; i++)
        if (PROLOGUE[i].op == op) return PROLOGUE[i].inl;
    return -1;
}

/* _stricmp's answer (0x4F8A70, the engine's piece and script matcher: A-Z
   fold) for a COB name `a`, which has `room` bytes before its block ends, and
   a model node's name `b`: 1 equal, 0 not. -1 when the two agree until `a`'s
   room or DK_NAMECMP runs out without ending, or `b` fails the range test:
   the caller then refuses the mask rather than guess what the engine's
   unbounded compare said. */
static int name_eq(const char* a, unsigned room, const char* b)
{
    unsigned i, lim = room < DK_NAMECMP ? room : DK_NAMECMP;
    if (!ptr_ok(b)) return -1;
    for (i = 0; i < lim; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return -1;
}

/* the same, against a lower-case literal of ours */
static int name_is(const char* a, unsigned room, const char* lit)
{
    unsigned i, lim = room < DK_NAMECMP ? room : DK_NAMECMP;
    for (i = 0; i < lim; i++) {
        int ca = (unsigned char)a[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (ca != (unsigned char)lit[i]) return 0;
        if (!ca) return 1;
    }
    return -1;
}

/* WHAT Create() HIDES, READ AND NOT RUN: from its entry point, through the
   opcodes of certain length, HIDE sets a COB piece's bit and SHOW clears it;
   the walk stops at the first opcode it cannot size. Conservative by
   construction — a hide behind a branch, a call or a sleep is not seen, and
   the ghost shows that piece, as it did before this mask existed.

   Returns the COB's piece count with `hid` filled (bits past it clear), 0 for
   a script with no Create(), -1 for a script the block's bounds refuse. */
static int create_hides(const char* cob, unsigned size, unsigned* hid)
{
    int ns, np, cl, i, ci = -1, pc;
    const int* entries;
    const char* const* snames;
    const int* code;
    if (size < COB_HEADER || *(const int*)(cob + COB_VERSION) != 4) return -1;
    ns = *(const int*)(cob + COB_NSCRIPTS);
    np = *(const int*)(cob + COB_NPIECES);
    cl = *(const int*)(cob + COB_CODELEN);
    if (ns <= 0 || np < 0 || np > DK_MAX_COBPIECES || cl <= 0) return -1;
    entries = *(const int* const*)(cob + COB_ENTRIES);
    snames  = *(const char* const* const*)(cob + COB_SNAMES);
    code    = *(const int* const*)(cob + COB_CODE);
    if (cob_room(cob, size, entries) / 4 < (unsigned)ns ||
        cob_room(cob, size, snames)  / 4 < (unsigned)ns ||
        cob_room(cob, size, code)    / 4 < (unsigned)cl) return -1;
    for (i = 0; i < ns; i++) {
        int e = name_is(snames[i], cob_room(cob, size, snames[i]), "create");
        if (e < 0) return -1;
        if (e) { ci = i; break; }
    }
    if (ci < 0) return 0;
    memset(hid, 0, (size_t)(DK_MAX_COBPIECES / 32) * sizeof *hid);
    pc = entries[ci];
    while (pc >= 0 && pc < cl) {
        unsigned op = (unsigned)code[pc];
        int inl = op_inline(op);
        if (inl < 0) break;
        if ((op == OP_HIDE || op == OP_SHOW) && pc + 1 < cl) {
            int k = code[pc + 1];
            if (k >= 0 && k < np) {
                if (op == OP_HIDE) hid[k >> 5] |= 1u << (k & 31);
                else               hid[k >> 5] &= ~(1u << (k & 31));
            }
        }
        pc += 1 + inl;
    }
    return np;
}

/* THE ENGINE'S PIECE ORDER FOR A NEW UNIT, reproduced so that COB piece k
   names the node the engine will hide. 0x45A950 builds the unit's
   primitives with 0x45AEC0 — a node, then its child's whole subtree, then
   its sibling's (the root's siblings included, as 0x45AE80 counts them) —
   and then, for each COB piece i below the primitive count, swaps into
   position i the FIRST primitive at an index >= i whose node name _stricmp
   matches COB piece i's (0x45A9E3..0x45AA70; the relink 0x45AF90 follows).
   COB piece i then drives primitive i whether or not a name matched.
   DISASSEMBLED.

   Only positions up to `upto` are settled: a later swap never moves a
   position below its own index. `pnames` holds `ncob` names, checked whole
   against the block by the caller. Returns the primitive count, -1 when the
   tree exceeds TAGPU_PBMAXPIECE or a compare cannot be answered. */
static int engine_prims(const char* root, const char* cob, unsigned size,
                        const char* const* pnames, int ncob, int upto, const char** prim)
{
    const char* stack[TAGPU_PBMAXPIECE + 2];
    int sp = 0, n = 0, i, j;
    stack[sp++] = root;
    while (sp > 0) {
        const char* nd = stack[--sp];
        const char* sib;
        const char* ch;
        if (n >= TAGPU_PBMAXPIECE) return -1;
        prim[n++] = nd;
        sib = *(const char* const*)(nd + N_SIB);
        ch  = *(const char* const*)(nd + N_CHILD);
        if (ptr_ok(sib)) stack[sp++] = sib;           /* popped after the child's subtree */
        if (ptr_ok(ch))  stack[sp++] = ch;
        if (sp > TAGPU_PBMAXPIECE) return -1;
    }
    for (i = 0; i < ncob && i < n && i <= upto; i++) {
        for (j = i; j < n; j++) {
            int e = name_eq(pnames[i], cob_room(cob, size, pnames[i]),
                            *(const char* const*)(prim[j] + N_NAME));
            if (e < 0) return -1;
            if (e) break;
        }
        if (j < n && j != i) { const char* t = prim[i]; prim[i] = prim[j]; prim[j] = t; }
    }
    return n;
}

/* the Create() mask, as bits over the ghost's walk: 1 with bits set, 0 when
   nothing is hidden, -1 when the script or the model is refused, -2 when the
   script's length was not recorded. `size` is cob_size(cob). */
static int create_mask(const char* root, const char* cob, unsigned size,
                       const char* const* walk, int np, uint32_t* bits)
{
    static unsigned    hid[DK_MAX_COBPIECES / 32];
    static const char* prim[TAGPU_PBMAXPIECE];
    const char*        hnode[TAGPU_PBMAXPIECE];
    const char* const* pnames;
    int ncob, nprim, k, g, nh = 0, upto = -1;

    if (!cob) return 0;                                /* no script: nothing hidden */
    if (!size) return -2;
    ncob = create_hides(cob, size, hid);
    if (ncob <= 0) return ncob;
    for (k = 0; k < ncob; k++) if (hid[k >> 5] >> (k & 31) & 1) upto = k;
    if (upto < 0) return 0;
    pnames = *(const char* const* const*)(cob + COB_PNAMES);
    if (cob_room(cob, size, pnames) / 4 < (unsigned)ncob) return -1;
    nprim = engine_prims(root, cob, size, pnames, ncob, upto, prim);
    if (nprim <= 0) return -1;
    for (k = 0; k <= upto && k < nprim; k++)
        if (hid[k >> 5] >> (k & 31) & 1) hnode[nh++] = prim[k];
    for (g = 0; g < np; g++)
        for (k = 0; k < nh; k++)
            if (walk[g] == hnode[k]) { bits[g >> 5] |= 1u << (g & 31); break; }
    return nh > 0;
}

/* is this node named in the list? TADR's rule: the node's name folded to
   lower case and cut at 63 characters, compared exactly; a node with no name
   is never listed */
static int listed(const char* name, const char* pp, unsigned n)
{
    char low[DK_NAME_CAP + 1];
    int i;
    unsigned k;
    if (!ptr_ok(name)) return 0;
    for (i = 0; i < DK_NAME_CAP && name[i]; i++) {
        char c = name[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    low[i] = 0;
    for (k = 0; k < n; k++, pp += strlen(pp) + 1)
        if (!strcmp(low, pp)) return 1;
    return 0;
}

/* The per-type cache: a mask is computed once for the level, and again only
   when its inputs move. The key is the frame packet's level generation (the
   template and the COB are freed at every level end), the record's serial
   (every entry to the FBI loader moves it, and a console reload enters it
   before it replaces the COB, which may come back at the same address), and
   the two pointers themselves. What it holds is VALUES — bits,
   a count, a pointer compared and never followed. Game thread only. */
typedef struct DkGhost {
    unsigned       gen, serial;
    const char*    root;
    const char*    cob;
    unsigned short np;
    unsigned char  valid, any;
    uint32_t       bits[8];
} DkGhost;

static DkGhost s_ghost[TAGPU_LIM_TYPES];

static void ghost_compute(const char* def, const char* root, const char* cob, unsigned cobsz,
                          const DkUnit* rec, DkGhost* g)
{
    static const char* walk[TAGPU_PBMAXPIECE];
    int np, g_i, nhid = 0, r;
    memset(g->bits, 0, sizeof g->bits);
    g->any = 0; g->np = 0;
    np = tagpu_model_walk(root, walk, TAGPU_PBMAXPIECE);
    if (np <= 0) {
        dlog("ghost mask: %.32s's model is refused by the walk (%s); every piece shows",
             def + UD_NAME, np < 0 ? "over 256 pieces" : "no root");
        return;
    }
    g->np = (unsigned short)np;
    if (rec && rec->pp) {
        unsigned found = 0, k;
        const char* s = rec->pp;
        for (g_i = 0; g_i < np; g_i++)
            if (!listed(*(const char* const*)(walk[g_i] + N_NAME), rec->pp, rec->pp_n)) {
                g->bits[g_i >> 5] |= 1u << (g_i & 31);
                nhid++;
            }
        for (k = 0; k < rec->pp_n; k++, s += strlen(s) + 1) {
            for (g_i = 0; g_i < np; g_i++)
                if (listed(*(const char* const*)(walk[g_i] + N_NAME), s, 1)) { found++; break; }
        }
        if (found) {
            g->any = nhid > 0;
            dlog("ghost mask: %.32s shows %d of %d pieces (PreviewPieces=, %u of its %u names in the model)",
                 def + UD_NAME, np - nhid, np, found, rec->pp_n);
            return;
        }
        memset(g->bits, 0, sizeof g->bits);
        nhid = 0;
        dlog("ghost mask: %.32s's PreviewPieces= names no piece of its model; ignored, Create()'s hides apply",
             def + UD_NAME);
    }
    r = create_mask(root, cob, cobsz, walk, np, g->bits);
    if (r < 0) {
        memset(g->bits, 0, sizeof g->bits);
        dlog(r == -2 ? "ghost mask: %.32s's script has no recorded length; every piece shows"
                     : "ghost mask: %.32s's script or model is refused by the bounds; every piece shows",
             def + UD_NAME);
        return;
    }
    if (r > 0) {
        for (g_i = 0; g_i < np; g_i++) nhid += (int)(g->bits[g_i >> 5] >> (g_i & 31) & 1);
        g->any = 1;
        dlog("ghost mask: %.32s hides %d of %d pieces (Create())", def + UD_NAME, nhid, np);
    }
}

int tagpu_datakeys_ghost_mask(unsigned type, TAGPU_PK_GHOSTMASK* out)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    const char* base;
    const char* mptrs;
    const char* def;
    const char* root;
    const char* cob;
    const DkUnit* rec = NULL;
    unsigned n, gen, serial = 0;
    DkGhost* g;

    if (!ptr_ok(ta) || !type || type >= TAGPU_LIM_TYPES) return 0;
    n = *(const unsigned*)(ta + OFF_UDEFCOUNT);
    if (type >= n) return 0;                          /* the bound every model id takes */
    base  = *(const char* const*)(ta + OFF_UNITDEFS);
    mptrs = *(const char* const*)(ta + OFF_MODELPTRS);
    if (!ptr_ok(base) || !ptr_ok(mptrs)) return 0;
    def  = base + (size_t)type * UDEF_STRIDE;
    root = *(const char* const*)(mptrs + (size_t)type * 4);
    if (!ptr_ok(root)) return 0;
    cob  = *(const char* const*)(def + UD_COB);
    if (s_unit[type].def == def) { rec = &s_unit[type]; serial = rec->serial; }

    g = &s_ghost[type];
    gen = tagpu_packet_pub_level_gen();
    if (!g->valid || g->gen != gen || g->serial != serial || g->root != root || g->cob != cob) {
        ghost_compute(def, root, cob, cob ? cob_size(cob) : 0u, rec, g);
        g->gen = gen; g->serial = serial; g->root = root; g->cob = cob; g->valid = 1;
    }
    if (!g->any) return 0;
    out->type    = (uint16_t)type;
    out->npieces = g->np;
    out->root    = (uint32_t)(size_t)root;
    memcpy(out->bits, g->bits, sizeof out->bits);
    return 1;
}

/* =========================================================================
   4. The weapon keys (C2)
   ========================================================================= */

/* ONE BYTE A WEAPON, indexed by the weapon's validated ID: the index
   tagpu_limits_weapon_index takes from a record's own address, -1 for
   anything that is not a whole record of this build's array (256 in the
   stock build, 4096 raised). So no value out of engine memory reaches the
   table unchecked, and a pointer that is not a weapon reads 0, which is
   stock: every hook below runs the engine's own instructions for a weapon
   whose byte is 0, and no retail weapon carries a key.

   WRITTEN on the LOADER thread inside the level's load: emptied at the
   weapon load's entry 0x42E310 (its one caller is 0x4918BB, in
   LoadGameData_Main 0x4917D0) and assigned per section at A'3's ID site
   0x42E468, after the ID and the name are accepted -- so a weapon the loader
   skips writes neither its record nor its byte, and a later section with the
   same ID replaces both. READ on the GAME thread in play, after that load:
   the ordering every game-thread read of the weapon array itself rests on.
   The render thread never reads it. */
static unsigned char s_wkey[TAGPU_LIM_WEAPONS];
static unsigned      s_wkeyed, s_wread;

typedef int (__thiscall *PFN_TdfGetInt)(void* sec, const char* key, int dflt);
#define E_TdfGetInt   ((PFN_TdfGetInt)0x004C46C0u)   /* atoi of the value, `ret 8` */
typedef int (__stdcall *PFN_GroundHeight)(const void* pos);
#define E_GroundHeight ((PFN_GroundHeight)0x00485070u) /* -1 off the map, `ret 4` */
typedef void (__stdcall *PFN_ClearTarget)(char* unit, int slot);
#define E_ClearTarget ((PFN_ClearTarget)0x0048A0F0u)

#define WF_WATER    0x10000u     /* weapon +0x111 bit 16, waterweapon            */
#define WF_TOAIR    0x20000u     /* bit 17, toairweapon                          */
#define DF_CANHOVER 0x1000u      /* UnitDef +0x241 bit 12                        */

unsigned tagpu_datakeys_wkey(const void* weapon)
{
    int i = tagpu_limits_weapon_index(weapon);
    return i < 0 ? 0u : s_wkey[i];
}

static void quiet_clear(void);

void __cdecl tagpu_datakeys_weapons_clear(void)
{
    if (s_wkeyed) dlog("weapon keys: emptied; the load before read %u weapon(s), %u with a key",
                       s_wread, s_wkeyed);
    memset(s_wkey, 0, sizeof s_wkey);
    s_wkeyed = s_wread = 0;
    quiet_clear();
}

/* The engine reads a boolean key as GetInt(key, 0) & 1 (the loader's rule for
   every flag of +0x111), so a key reads the same way here. */
static int tdf_flag(void* sec, const char* key)
{
    return E_TdfGetInt(sec, key, 0) & 1;
}

void tagpu_datakeys_weapon_read(int id, void* sec, const char* name)
{
    unsigned k = 0;
    if (id < 0 || id >= TAGPU_LIM_WEAPONS || !sec) return;
    s_wread++;
    if (tdf_flag(sec, "nottoair"))        k |= TAGPU_WK_NOTTOAIR;
    if (tdf_flag(sec, "nottounderwater")) k |= TAGPU_WK_NOTTOUNDERWATER;
    if (tdf_flag(sec, "surfacefire"))     k |= TAGPU_WK_SURFACEFIRE;
    if (tdf_flag(sec, "notoverwater"))    k |= TAGPU_WK_NOTOVERWATER;
    if (tdf_flag(sec, "notoverland"))     k |= TAGPU_WK_NOTOVERLAND;
    if (tdf_flag(sec, "nomapweaponalert")) k |= TAGPU_WK_NOMAPALERT;
    s_wkey[id] = (unsigned char)k;
    if (!k) return;
    s_wkeyed++;
    dlog("weapon keys: %.32s (ID %d):%s%s%s%s%s%s", name, id,
         k & TAGPU_WK_NOTTOAIR ? " nottoair" : "",
         k & TAGPU_WK_NOTTOUNDERWATER ? " nottounderwater" : "",
         k & TAGPU_WK_SURFACEFIRE ? " surfacefire" : "",
         k & TAGPU_WK_NOTOVERWATER ? " notoverwater" : "",
         k & TAGPU_WK_NOTOVERLAND ? " notoverland" : "",
         k & TAGPU_WK_NOMAPALERT ? " nomapweaponalert" : "");
    if ((k & TAGPU_WK_SURFACEFIRE) && !tdf_flag(sec, "waterweapon"))
        dlog("weapon keys: %.32s has surfacefire without waterweapon, which it needs; no effect", name);
    if ((k & TAGPU_WK_NOTOVERWATER) && (k & TAGPU_WK_NOTOVERLAND))
        dlog("weapon keys: %.32s has both notoverwater and notoverland; it fires only off the map", name);
}

/* first, second, fourth, eighth ... occurrence of an event, with its count:
   a log a measurement can read without a heartbeat, and bounded */
static void wk_event(unsigned* n, const char* what)
{
    unsigned c = ++*n;
    if ((c & (c - 1)) == 0) dlog("weapon keys: %s (%u so far)", what, c);
}
static unsigned s_evAir, s_evUnder, s_evSurface, s_evOrder, s_evSteer, s_evSuppress, s_evDrop;

static unsigned sea_level(void)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    return *(const unsigned char*)(ta + OFF_SEALEVEL);
}

static int wflags(const char* w) { return *(const int*)(w + 0x111); }
static int flying(const char* u) { return (*(const unsigned*)(u + 0x110) & 3) == 2; }

/* the top of a unit, as the engine sums it at 0x49ACEA..0x49ACF5 */
static int unit_top(const char* u)
{
    const char* def = *(const char* const*)(u + U_TYPE);
    return (int)*(const short*)(u + 0x70) + (int)*(const short*)(def + 0x170);
}

/* the water path's range test, 0x49AC47..0x49ACA2: the high words of dx*dx
   and dz*dz (the positions are 16.16), summed, against range squared */
static int in_range(const char* u, const char* t, const char* w)
{
    int dx = *(const int*)(t + U_XFIX) - *(const int*)(u + U_XFIX);
    int dz = *(const int*)(t + U_YFIX) - *(const int*)(u + U_YFIX);
    int r  = *(const int*)(w + 0xDC);
    return (int)(((long long)dx * dx) >> 32) + (int)(((long long)dz * dz) >> 32) <= r * r;
}

/* THE CAN-ENGAGE VERDICT, filtered. `v` is 0x49ABB0's answer (or the
   extra-weapons module's for a slot past 2); the caller is the one detour on
   0x49ABB0's entry, so every acquisition, retaliation, cursor and order that
   asks the engine asks this. surfacefire comes first and nottoair and
   nottounderwater after it, in one function: no order between the keys can
   loop, and a water weapon's rejection turns only into stock's own range
   test. */
int tagpu_datakeys_engage(char* u, char* t, const char* w, int v)
{
    unsigned k = tagpu_datakeys_wkey(w);
    int air;
    if (!k || !t) return v;
    air = flying(t);
    if (!v && (k & TAGPU_WK_SURFACEFIRE) && (wflags(w) & WF_WATER) && !air) {
        v = in_range(u, t, w);
        if (v) wk_event(&s_evSurface, "surfacefire engaged a surface target");
    }
    if (v && (k & TAGPU_WK_NOTTOAIR) && air) {
        v = 0;
        wk_event(&s_evAir, "nottoair refused a flying target");
    }
    if (v && (k & TAGPU_WK_NOTTOUNDERWATER) && unit_top(t) <= (int)sea_level()) {
        v = 0;
        wk_event(&s_evUnder, "nottounderwater refused a submerged target");
    }
    return v;
}

/* THE ORDER ACTION'S UNIT BRANCH, 0x43F0E0 from 0x43F1D4 to its exits: the
   action names weapon 0 (+0x10), and slot 1 (+0x2C, present when +0x3B bit 1
   is set) only in the submerged test. When neither carries a key this answers
   0 and the site runs stock's own instructions. Otherwise it takes stock's
   decisions in stock's order, with each key mirrored where stock tests its
   counterpart: nottoair beside toairweapon, nottounderwater in the submerged
   test, surfacefire at the hover's water refusal, for a target not flying.
   1 = refuse (0x4401DC), 2 = on to 0x43F27A, 3 = stock's no-action exit
   0x43F26C (a hover whose slot 1 is a water weapon). */
int __stdcall tagpu_datakeys_order(char* s, char* t)
{
    const char* w0 = *(const char* const*)(s + 0x10);
    const char* sdef = *(const char* const*)(s + U_TYPE);
    int present1 = (*(const unsigned char*)(s + 0x3B) & 2) != 0;
    const char* w1 = present1 ? *(const char* const*)(s + 0x2C) : NULL;
    unsigned k0 = tagpu_datakeys_wkey(w0), k1 = w1 ? tagpu_datakeys_wkey(w1) : 0u;
    int air, top, sea, f0;
    if (!(k0 & (TAGPU_WK_NOTTOAIR | TAGPU_WK_NOTTOUNDERWATER | TAGPU_WK_SURFACEFIRE)) &&
        !(k1 & TAGPU_WK_NOTTOUNDERWATER))
        return 0;
    wk_event(&s_evOrder, "an order onto a unit decided with a key");
    air = flying(t);
    f0 = wflags(w0);
    if (!air && (f0 & WF_TOAIR)) return 1;
    if (air && (k0 & TAGPU_WK_NOTTOAIR)) return 1;
    top = unit_top(t);
    sea = (int)sea_level();
    if (top < sea) {
        int ok0 = (f0 & WF_WATER) && !(k0 & TAGPU_WK_NOTTOUNDERWATER);
        int ok1 = w1 && (wflags(w1) & WF_WATER) && !(k1 & TAGPU_WK_NOTTOUNDERWATER);
        return ok0 || ok1 ? 2 : 1;
    }
    if (!(*(const unsigned*)(sdef + 0x241) & DF_CANHOVER)) return 2;
    if (f0 & WF_WATER) return (k0 & TAGPU_WK_SURFACEFIRE) && !air ? 2 : 1;
    if (w1 && (wflags(w1) & WF_WATER)) return 3;
    return 2;
}

/* THE GUIDANCE, 0x49B9EB: in its self-propelled flight a water weapon's
   projectile above the sea falls and does not steer; surfacefire's steers. */
int __stdcall tagpu_datakeys_steer(const char* w)
{
    if (!(tagpu_datakeys_wkey(w) & TAGPU_WK_SURFACEFIRE)) return 0;
    wk_event(&s_evSteer, "a surfacefire projectile steered above the sea");
    return 1;
}

/* THE FIRE GATE, after a slot's per-tick target read and its fire-function
   check (0x49E1FD in AutoAim, and the same step of the extra-weapons module's
   C loop): 0 = on as stock, 1 = the slot neither aims nor fires this tick
   (notoverwater / notoverland, by the ground under the firer; off the map,
   -1, it is not gated), 2 = its held target was a flying unit under nottoair
   and is dropped through the engine's own ClearTarget 0x48A0F0 -- what
   0x48A1E0 does to a dead one -- so acquisition picks again through the
   filtered verdict. THE INVARIANT: this runs after the target read, so a
   suppressed slot still drops a dead target every tick, as stock does.
   `slot` is the 28-byte slot; its target is a unit index while its spot is
   0x8000, bounded here by the engine's own unit array before it is read. */
int tagpu_datakeys_fire_gate(char* u, int idx, const char* w, const void* slot)
{
    unsigned k = tagpu_datakeys_wkey(w);
    if (!k) return 0;
    if (k & (TAGPU_WK_NOTOVERWATER | TAGPU_WK_NOTOVERLAND)) {
        int h = E_GroundHeight(u + U_XFIX);
        if (h != -1) {
            int water = h <= (int)sea_level();
            if (water ? (k & TAGPU_WK_NOTOVERWATER) : (k & TAGPU_WK_NOTOVERLAND)) {
                wk_event(&s_evSuppress, "a slot held its fire over the terrain its key names");
                return 1;
            }
        }
    }
    if (k & TAGPU_WK_NOTTOAIR) {
        const unsigned short* sl = (const unsigned short*)slot;
        const char* ta = *(const char* const*)TA_MAIN_PP;
        const char* first = *(const char* const*)(ta + OFF_UNIT_BEGIN);
        const char* last  = *(const char* const*)(ta + OFF_UNIT_END);
        unsigned tgt = sl[0];
        if (sl[1] == 0x8000 && tgt && ptr_ok(first) && last >= first &&
            tgt <= (unsigned)(last - first) / UNIT_STRIDE &&
            flying(first + (size_t)tgt * UNIT_STRIDE)) {
            E_ClearTarget(u, idx);
            wk_event(&s_evDrop, "nottoair dropped a held target that is flying");
            return 2;
        }
    }
    return 0;
}

/* =========================================================================
   5. nomapweaponalert: the silence of harmless weather (C2)
   =========================================================================
   A weather hit -- a projectile with no attacker whose weapon carries the key
   and has default damage 0 -- is applied exactly as stock applies it and
   says nothing about it: no
   "under attack" notification (its text and its sound), no blink of the hit
   unit's minimap dot, and no minimap dot for the projectile. DISPLAY ONLY:
   every sim write of the hit stays -- the recently-hit counter +0xFA, the kind
   +0xF5, the listeners' event, the remote owner's 0x0B. Each part below is
   skip-and-log and rests only on the sites it names.

   THE DECISION, harmless_weather, is about the hit record 0x489CE0 is
   applying (edi there: +3 the attacker's u16 id, +5 the amount, +8 the
   kind). The record names no weapon, and a weather stone is not the only
   attacker-less weapon hit: a unit's death explosion (0x49B000 zeroes the
   projectile's attacker) and the fire spreading from a burning feature
   (0x49A0C0) make them too. So the answer comes from where the weapon is
   known:
   - A record computed HERE: every weapon-kind hit comes from one call,
     0x489BB0 at 0x499E37 in the damage function 0x499CD0, whose projectile
     is in hand. weather_send frames that call with the projectile's own
     weapon's answer; 0x489BB0 builds the record on its own stack and applies
     it at 0x489C89, where local_apply pins the answer to that record's
     address. A record at that address is this hit; nothing else is.
   - A record RECEIVED as a 0x0B from a peer (the dispatcher's case,
     0x455412) carries no weapon, so it is harmless only when nothing was
     lost: no attacker, a weapon kind, amount 0, and the level's meteor weapon
     [0x512328] keyed with default damage 0. A received weather hit with a
     per-type damage alerts, and a received explosion or fire is silent only
     when it did no damage.
   Every weapon pointer is bounded as a record of the weapon array (the key
   store's index) before its default damage (+0xD4, a WORD: 0x499CE3,
   0x42EFA9, 0x42F326) is read. */

#define VA_METEOR_WEAPON 0x00512328u
#define WF_METEOR        0x20u       /* weapon +0x111 bit 5                 */
#define WF_NORADAR       0x40u       /* bit 6: its one reader is the dot, 0x467206 */
#define HIT_FA           0xF0u       /* what 0x467950 stores in +0xFA       */

static int weather_weapon(const char* w)
{
    return (tagpu_datakeys_wkey(w) & TAGPU_WK_NOMAPALERT) && *(const short*)(w + 0xD4) == 0;
}

/* GAME thread only, like every hit. s_armed is weather_send's answer from its
   set to the next local apply, which takes it; nothing else can run between
   them (0x489BB0 reaches 0x489C89 on every path, calling first only the
   arithmetic helper 0x4E43D0 and the veterancy and B7 answers at 0x489BF3,
   0x489BFA and 0x489C71, which read a unit and count events and neither make
   a hit nor touch s_armed), and weather_send zeroes it again on return. */
static int s_armed;
static const unsigned char* s_localRec;   /* the record 0x489C89 is applying, or NULL */
static int s_localWeather;                /* ... and its answer */
static int s_localOn;                     /* both local sites landed */

static int harmless_weather(const unsigned char* rec)
{
    unsigned char kind = rec[8];
    if (s_localOn && rec == s_localRec) return s_localWeather;
    if (*(const unsigned short*)(rec + 3) != 0 || (kind != 1 && kind != 2)) return 0;
    if (*(const unsigned short*)(rec + 5) != 0) return 0;
    return weather_weapon(*(const char* const*)VA_METEOR_WEAPON);
}

typedef int (__stdcall *PFN_HitSend)(char* attacker, char* victim, int amount, int kind, int angle);
#define E_HitSend ((PFN_HitSend)0x00489BB0u)
typedef int (__stdcall *PFN_Apply)(const unsigned char* rec);
#define E_Apply   ((PFN_Apply)0x00489CE0u)

/* 0x499E37's call of 0x489BB0(attacker, victim, amount, kind, angle), `ret
   0x14`, through a stub that adds the projectile the damage function holds
   ([esp+0x30] at the call; the engine reads its weapon, [proj], at 0x499E1E). */
static int __stdcall weather_send(const char* const* proj, char* attacker, char* victim,
                                  int amount, int kind, int angle)
{
    int r;
    s_armed = !attacker && weather_weapon(*proj);
    r = E_HitSend(attacker, victim, amount, kind, angle);
    s_armed = 0;
    return r;
}

/* 0x489C89's call of 0x489CE0(rec), `ret 4`: the local apply of the record
   0x489BB0 just built. Saving and restoring makes a nested local apply hand
   the outer record back its own answer. */
static int __stdcall local_apply(const unsigned char* rec)
{
    const unsigned char* prevRec = s_localRec;
    int prevWeather = s_localWeather, r;
    s_localRec = rec;
    s_localWeather = s_armed;
    s_armed = 0;
    r = E_Apply(rec);
    s_localRec = prevRec;
    s_localWeather = prevWeather;
    return r;
}

static unsigned s_evSilenced, s_evQuietHit, s_evBlink, s_evNoRadar;

/* --- the notification ------------------------------------------------------
   0x406F80(attacker, victim, amount), `ret 0xC`, has one caller, 0x489DA2, and
   its one "under attack" is 0x47F850(victim, 2, 0) at 0x4071D8. The call at
   0x489DA2 goes through hit_frame, which holds the record's answer for the
   whole of 0x406F80's dynamic extent; saving and restoring it makes a nested
   hit (0x4897B0's listeners, 0x48A060) hand the outer answer back by
   construction. The flag is written and read on the GAME thread only: the
   sim tick, and the dispatcher's 0x0B case in play. */
typedef int (__stdcall *PFN_Hit)(char* attacker, char* victim, int amount);
#define E_Hit    ((PFN_Hit)0x00406F80u)
typedef int (__stdcall *PFN_Notify)(char* unit, int kind, int arg);
#define E_Notify ((PFN_Notify)0x0047F850u)

static int s_hitQuiet;
static int s_alertOn;     /* both sites landed; set at attach, before the engine runs */

static int __stdcall hit_frame(const unsigned char* rec, char* attacker, char* victim, int amount)
{
    int prev = s_hitQuiet, r;
    s_hitQuiet = harmless_weather(rec);
    r = E_Hit(attacker, victim, amount);
    s_hitQuiet = prev;
    return r;
}

int __stdcall tagpu_datakeys_alert(char* unit, int kind, int arg)
{
    if (kind == 2 && s_alertOn && s_hitQuiet) {
        wk_event(&s_evSilenced, "nomapweaponalert silenced an under-attack notification");
        return 0;
    }
    return E_Notify(unit, kind, arg);
}

/* --- the blink ---------------------------------------------------------------
   The minimap rebuild 0x466DC0 (GAME thread, every sim tick) blinks a unit's
   dot while +0xFA is non-zero (0x466EB9..0x466ECA). A unit's +0xFA is written
   by the hit 0x467950 (0xF0; its one caller is 0x489D8E), the unit tick
   (0x48ADF0, down by one to 0), the create (0x485C12, 0) and the saved game's
   restore (0x4872CC) -- every write of a unit's +0xFA in the binary.

   s_quiet[slot] is E, the part of +0xFA owed to harmless hits only: the dot
   blinks while +0xFA > E. At each hit the loud remainder is fa - E, so a loud
   hit sets E = 0 and a harmless one E = 0xF0 - (fa - E): a real hit followed
   by weather keeps blinking exactly until its own 0xF0 ticks run out, because
   +0xFA and its loud part fall together. E = 0 is stock, and E means nothing
   while +0xFA is 0, so a slot's next unit needs no reset (the create zeroes
   +0xFA). The table is emptied at the level's weapon load 0x42E310, which the
   saved game's restore follows (it writes weapon fields, 0x487628), and a
   saved game carries each unit's E beside its +0xFA (below). Indexed by the
   unit's slot, bounded by the engine's own slot count (u16 main+0x14351) and
   by the table. */
#define DK_QUIET_SLOTS (10 * TAGPU_LIM_UNITS + 1)
static unsigned char s_quiet[DK_QUIET_SLOTS];

static void quiet_clear(void) { memset(s_quiet, 0, sizeof s_quiet); }

static int unit_slot(const char* u)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    const char* first;
    unsigned slots;
    size_t d;
    if (!ptr_ok(ta)) return -1;
    first = *(const char* const*)(ta + OFF_UNIT_BEGIN);
    slots = *(const unsigned short*)(ta + OFF_UNITSLOTS);
    if (!ptr_ok(first) || u < first) return -1;
    d = (size_t)(u - first);
    if (d % UNIT_STRIDE) return -1;
    d /= UNIT_STRIDE;
    return d < slots && d < DK_QUIET_SLOTS ? (int)d : -1;
}

typedef char* (__stdcall *PFN_MarkHit)(char* unit);
#define E_MarkHit ((PFN_MarkHit)0x00467950u)

static char* __stdcall hit_mark(const unsigned char* rec, char* victim)
{
    int s = unit_slot(victim);
    if (s >= 0) {
        unsigned fa = *(const unsigned char*)(victim + 0xFA), e = s_quiet[s];
        unsigned loud = fa > e ? fa - e : 0;
        if (loud > HIT_FA) loud = HIT_FA;
        if (harmless_weather(rec)) {
            s_quiet[s] = (unsigned char)(HIT_FA - loud);
            wk_event(&s_evQuietHit, "a harmless weather hit applied (nomapweaponalert)");
        } else {
            s_quiet[s] = 0;
        }
    }
    return E_MarkHit(victim);
}

static int __stdcall blink_quiet(const char* u)
{
    int s = unit_slot(u);
    unsigned fa;
    if (s < 0) return 0;
    fa = *(const unsigned char*)(u + 0xFA);
    if (!fa || fa > s_quiet[s]) return 0;
    wk_event(&s_evBlink, "a minimap dot held still after a harmless weather hit");
    return 1;
}

/* --- the blink across a save ---------------------------------------------------
   The unit saver 0x4876C0 (one caller, 0x432A01) builds each unit's 0xB8-byte
   record at [esp+0x18] and writes it whole (0x487A9E); the restore 0x487080
   reads a record back into its own [esp+0x18] (0x48711D) and writes +0xFA from
   record +0xB1 (0x4872CC). Record +0xB2 is a WORD that 0x48797B stores from a
   movzx of the byte +0x10E, and the restore reads only its low byte
   (0x4872D2); the only other reader, 0x486FD0, reads the id word +0x21. So
   record +0xB3 is 0 in every save stock writes and nothing reads it. E rides
   there: the saver stores (E << 8) | +0x10E and the restore gives the unit's
   slot the E beside its +0xFA, so a restored unit blinks exactly as it would
   have. A save without E carries 0, which is stock. The restore runs on the
   LOADER thread inside the level's load, after 0x42E310 emptied the table. */
static unsigned __stdcall save_word(const char* u, unsigned v)
{
    int s = unit_slot(u);
    return (s >= 0 ? (unsigned)s_quiet[s] << 8 : 0u) | (v & 0xFFu);
}

static void __stdcall restore_quiet(const char* u, unsigned e)
{
    int s = unit_slot(u);
    if (s >= 0) s_quiet[s] = (unsigned char)e;
}

/* --- the projectile's dot ------------------------------------------------------
   The loader's closing call 0x49E010(weapon) runs once a weapon's flags and
   default damage are final (both exits, 0x42F314 and 0x42F32E, come after
   the last writes of +0x111 and +0xD4), on the LOADER thread, after the key
   was read at 0x42E468. A weapon with the key, meteor=1 and default damage 0
   gets noradar there, exactly as `noradar=1` in its file would set it. */
#define VA_AIM_CHOICE 0x0049E010u
static const unsigned char AIM_CHOICE_STOLEN[10] = {
    0x8B, 0x4C, 0x24, 0x04,                         /* mov ecx,[esp+4]      */
    0x8B, 0x81, 0x11, 0x01, 0x00, 0x00 };           /* mov eax,[ecx+0x111]  */

static int __cdecl weapon_closed(void* esp)
{
    char* w = ((char**)esp)[1];
    unsigned f;
    if (!(tagpu_datakeys_wkey(w) & TAGPU_WK_NOMAPALERT)) return 0;
    f = (unsigned)wflags(w);
    if (!(f & WF_METEOR) || *(const short*)(w + 0xD4) != 0) {
        dlog("weapon keys: %.32s has nomapweaponalert but is not a meteor weapon with default "
             "damage 0; its projectile keeps its dot", w);
        return 0;
    }
    if (!(f & WF_NORADAR)) {
        *(unsigned*)(w + 0x111) = f | WF_NORADAR;
        wk_event(&s_evNoRadar, "a nomapweaponalert meteor weapon draws no minimap dot");
    }
    return 0;
}

/* --- install -------------------------------------------------------------------
   Three call sites become calls of ours, byte-matched against the retail call
   first; no branch in .text lands inside any of them (rel8/rel32 scan). The
   0x489CE0 two go through an 8-byte stub that adds the record: `pop eax; push
   edi; push eax; jmp fn`, so fn(rec, <the callee's own arguments>) returns
   with the callee's `ret` plus 4. Neither 0x489D93 nor 0x489DA7 reads eax,
   ecx or edx before writing it. The frame lands before the notification's
   site, and s_alertOn is set only once both have, so a failed write leaves
   stock's alert everywhere; the blink site lands only after the mark. */
static int site_is(unsigned va, const unsigned char* b, int n)
{
    return memcmp((const void*)(size_t)va, b, (size_t)n) == 0;
}

static unsigned char* rec_stub(void* fn)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x58;                                    /* pop eax: the return  */
    *p++ = 0x57;                                    /* push edi: the record */
    *p++ = 0x50;                                    /* push eax             */
    *p++ = 0xE9; tagpu_detour_rel(p, (unsigned)(size_t)fn);
    return s;
}

/* `call target` written over the 5-byte call at va. The rel32 is taken
   against va itself: tagpu_detour_rel would encode against this buffer. */
static int call_to(unsigned va, const void* target)
{
    unsigned char c[5];
    unsigned rel = (unsigned)(size_t)target - (va + 5);
    c[0] = 0xE8;
    memcpy(c + 1, &rel, 4);
    return tagpu_detour_write(va, c, 5);
}

#define VA_HIT_MARK    0x00489D8Eu
#define VA_HIT_CALL    0x00489DA2u
#define VA_ALERT_CALL  0x004071D8u
#define VA_BLINK       0x00466EB9u
#define VA_BLINK_BACK  0x00466EBFu
static const unsigned char HIT_MARK_WAS[5]   = { 0xE8, 0xBD, 0xDB, 0xFD, 0xFF };  /* call 0x467950 */
static const unsigned char HIT_CALL_WAS[5]   = { 0xE8, 0xD9, 0xD1, 0xF7, 0xFF };  /* call 0x406F80 */
static const unsigned char ALERT_CALL_WAS[5] = { 0xE8, 0x73, 0x86, 0x07, 0x00 };  /* call 0x47F850 */
static const unsigned char BLINK_WAS[6]      = { 0x8A, 0x83, 0xFA, 0x00, 0x00, 0x00 };

static int blink_install(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char *p = s, *j1, *j2;
    if (!s) return 0;
    memcpy(p, BLINK_WAS, sizeof BLINK_WAS); p += sizeof BLINK_WAS; /* mov al,[ebx+0xFA] */
    *p++ = 0x84; *p++ = 0xC0;                       /* test al,al           */
    *p++ = 0x74; j1 = p++;                          /* jz back              */
    *p++ = 0x51; *p++ = 0x52; *p++ = 0x50;          /* push ecx, edx, eax   */
    *p++ = 0x53;                                    /* push ebx: the unit   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)blink_quiet); p += 4;
    *p++ = 0x85; *p++ = 0xC0;                       /* test eax,eax         */
    *p++ = 0x58; *p++ = 0x5A; *p++ = 0x59;          /* pop eax, edx, ecx    */
    *p++ = 0x74; j2 = p++;                          /* jz back              */
    *p++ = 0x32; *p++ = 0xC0;                       /* xor al,al: no blink  */
    *j1 = (unsigned char)(p - (j1 + 1));
    *j2 = (unsigned char)(p - (j2 + 1));
    *p++ = 0xE9; tagpu_detour_rel(p, VA_BLINK_BACK);
    if (!tagpu_detour_land(VA_BLINK, s, (int)sizeof BLINK_WAS)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

#define VA_SAVE_WORD       0x0048797Bu
#define VA_SAVE_WORD_BACK  0x00487983u
#define VA_REST_FA         0x004872CCu
#define VA_REST_FA_BACK    0x004872D2u
static const unsigned char SAVE_WORD_WAS[8] = { 0x66, 0x89, 0x84, 0x24, 0xCA, 0x00, 0x00, 0x00 };
static const unsigned char REST_FA_WAS[6]   = { 0x88, 0x96, 0xFA, 0x00, 0x00, 0x00 };

/* Both sites are whole instructions whose next instruction neither reads the
   flags nor the registers the stubs clobber (eax is rewritten at 0x487983 and
   0x4872D2). The restore lands first: a restore without the saver reads the
   0 of every save, a saver without the restore writes a byte nothing reads. */
static int save_install(void)
{
    unsigned char *r, *s, *p;
    unsigned d = 0xCB + 12;                         /* record +0xB3, under three pushes */
    if (!site_is(VA_SAVE_WORD, SAVE_WORD_WAS, 8) || !site_is(VA_REST_FA, REST_FA_WAS, 6)) return 0;
    if (!(r = tagpu_detour_stub())) return 0;
    if (!(s = tagpu_detour_stub())) { VirtualFree(r, 0, MEM_RELEASE); return 0; }
    p = r;
    memcpy(p, REST_FA_WAS, sizeof REST_FA_WAS); p += sizeof REST_FA_WAS; /* mov [esi+0xFA],dl */
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;          /* push eax, ecx, edx   */
    *p++ = 0x0F; *p++ = 0xB6; *p++ = 0x84; *p++ = 0x24; memcpy(p, &d, 4); p += 4; /* movzx eax,byte [esp+d] */
    *p++ = 0x50;                                    /* push eax: E as saved */
    *p++ = 0x56;                                    /* push esi: the unit   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)restore_quiet); p += 4;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;          /* pop edx, ecx, eax    */
    *p++ = 0xE9; tagpu_detour_rel(p, VA_REST_FA_BACK);
    p = s;
    *p++ = 0x51; *p++ = 0x52;                       /* push ecx, edx        */
    *p++ = 0x50;                                    /* push eax: +0x10E     */
    *p++ = 0x55;                                    /* push ebp: the unit   */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)save_word); p += 4;
    *p++ = 0x5A; *p++ = 0x59;                       /* pop edx, ecx         */
    memcpy(p, SAVE_WORD_WAS, sizeof SAVE_WORD_WAS); p += sizeof SAVE_WORD_WAS; /* mov [esp+0xCA],ax */
    *p++ = 0xE9; tagpu_detour_rel(p, VA_SAVE_WORD_BACK);
    if (!tagpu_detour_land(VA_REST_FA, r, (int)sizeof REST_FA_WAS)) {
        VirtualFree(r, 0, MEM_RELEASE);
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    if (!tagpu_detour_land(VA_SAVE_WORD, s, (int)sizeof SAVE_WORD_WAS)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

#define VA_SEND_CALL   0x00499E37u
#define VA_APPLY_CALL  0x00489C89u
static const unsigned char SEND_CALL_WAS[5]  = { 0xE8, 0x74, 0xFD, 0xFE, 0xFF };  /* call 0x489BB0 */
static const unsigned char APPLY_CALL_WAS[5] = { 0xE8, 0x52, 0x00, 0x00, 0x00 };  /* call 0x489CE0 */

/* The send's frame lands first and the apply second: without the apply, the
   send's answer is taken by nothing and every record is judged as received
   (never silencing a loss); s_localOn is set only once both have. */
static int local_install(void)
{
    unsigned char *s, *p;
    if (!site_is(VA_SEND_CALL, SEND_CALL_WAS, 5) || !site_is(VA_APPLY_CALL, APPLY_CALL_WAS, 5)) return 0;
    if (!(s = tagpu_detour_stub())) return 0;
    p = s;
    *p++ = 0x58;                                    /* pop eax: the return  */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x30; /* push [esp+0x30]: the projectile */
    *p++ = 0x50;                                    /* push eax             */
    *p++ = 0xE9; tagpu_detour_rel(p, (unsigned)(size_t)weather_send);
    if (!call_to(VA_SEND_CALL, s)) {
        VirtualFree(s, 0, MEM_RELEASE);
        return 0;
    }
    if (!call_to(VA_APPLY_CALL, (const void*)local_apply)) return 0;
    s_localOn = 1;
    return 1;
}

static void alert_install(void)
{
    unsigned char *frame, *mark;
    if (local_install())
        dlog("nomapweaponalert: a hit computed here is judged by its own weapon (0x499E37, 0x489C89)");
    else
        dlog("nomapweaponalert: a hit computed here is NOT judged by its weapon: the bytes at "
             "0x499E37 or 0x489C89 are not the retail calls, or a write failed; every hit is "
             "judged as a received one (harmless only when it did no damage)");
    if (!site_is(VA_HIT_CALL, HIT_CALL_WAS, 5) || !site_is(VA_ALERT_CALL, ALERT_CALL_WAS, 5)) {
        dlog("nomapweaponalert: the under-attack silence NOT installed: the bytes at 0x489DA2 or "
             "0x4071D8 are not the retail calls; harmless weather alerts as stock");
    } else if (!(frame = rec_stub((void*)hit_frame)) || !call_to(VA_HIT_CALL, frame) ||
               !call_to(VA_ALERT_CALL, (const void*)tagpu_datakeys_alert)) {
        dlog("nomapweaponalert: the under-attack silence NOT installed: a write failed; "
             "harmless weather alerts as stock");
    } else {
        s_alertOn = 1;
        dlog("nomapweaponalert: the under-attack silence installed at 0x489DA2 and 0x4071D8");
    }
    if (!site_is(VA_HIT_MARK, HIT_MARK_WAS, 5) || !site_is(VA_BLINK, BLINK_WAS, 6)) {
        dlog("nomapweaponalert: the blink NOT held: the bytes at 0x489D8E or 0x466EB9 are not "
             "the retail ones; a harmless hit blinks the dot as stock");
    } else if (!(mark = rec_stub((void*)hit_mark)) || !call_to(VA_HIT_MARK, mark) ||
               !blink_install()) {
        dlog("nomapweaponalert: the blink NOT held: a write failed; a harmless hit blinks the dot "
             "as stock");
    } else {
        dlog("nomapweaponalert: the blink held at 0x489D8E and 0x466EB9");
        if (save_install())
            dlog("nomapweaponalert: a saved game carries the blink's hold (0x48797B, 0x4872CC)");
        else
            dlog("nomapweaponalert: a saved game does NOT carry the blink's hold: the bytes at "
                 "0x48797B or 0x4872CC are not the retail ones, or a write failed; a harmless "
                 "hit's blink that spans a save and its restore blinks for its remainder");
    }
    if (observe(VA_AIM_CHOICE, AIM_CHOICE_STOLEN, (int)sizeof AIM_CHOICE_STOLEN, weapon_closed))
        dlog("nomapweaponalert: the projectile's dot hidden through noradar at 0x49E010");
    else
        dlog("nomapweaponalert: the projectile's dot NOT hidden: the bytes at 0x49E010 are not "
             "the retail ones; a harmless meteor keeps its minimap dot");
}

/* =========================================================================
   6. Veterancy (C3)
   ========================================================================= */

/* A unit's kills are the u16 +0xB8, zero-extended by every engine reader;
   the one increment is the destructor's 0x4869CA, on every peer, from the
   death record (data-keys-evidence.md Part 3 §0). A type's LEVEL is the
   count of its thresholds at or below its kills, as TADR counts it. Each
   effect bounds the level where its arithmetic needs a bound, and each bound
   is stated at the effect. A type without the key never reaches these: its
   site runs stock's own instructions. */
#define U_KILLS         0xB8
#define DK_VET_TAKEN    25      /* (25 - L) * 4 % of a weapon hit: L = 25 is none */
#define DK_VET_RELOAD   16      /* the largest L with 100 - 6L > 0                */
#define DK_VET_CAPTURE  13107   /* stock's own ceiling, 65535 / 5                 */

static unsigned kills_of(const char* u) { return *(const unsigned short*)(u + U_KILLS); }

/* the record of the unit's type, or NULL: the unit's def pointer is DATA,
   bounded to a slot of the def array and taken only when the slot's row was
   written for that def */
static const DkUnit* vet_row(const char* u)
{
    const char* def;
    unsigned slot;
    const DkUnit* r;
    if (!ptr_ok(u)) return NULL;
    def = *(const char* const*)(u + U_TYPE);
    if (!def_slot(def, &slot)) return NULL;
    r = &s_unit[slot];
    return r->def == def ? r : NULL;
}

static unsigned vet_level(const DkUnit* r, unsigned k)
{
    unsigned lo = 0, hi = r->vthr_n;
    while (lo < hi) {
        unsigned m = (lo + hi) / 2;
        if (r->vthr[m] <= k) lo = m + 1; else hi = m;
    }
    return lo;
}

/* The capture's level has no top in TADR: past the last threshold it goes
   on by the last gap (by the one threshold, for a list of one); a unit
   reclaim's step, which TADR leaves stock, takes the same level. The gap is at
   least 1 and the threshold at least 1 by the parse, so neither divides by
   zero. The level is capped at stock's own ceiling, 13 107: the capture's
   base is at most 1800 (0x404396), so its product (0x4043EC..0x4043F7) stays
   at 236 106 000, below 2^31; the reclaim step's product is bounded by its answer. */
static unsigned vet_level_open(const DkUnit* r, unsigned k)
{
    unsigned n = r->vthr_n, top = r->vthr[n - 1], l;
    if (k <= top) return vet_level(r, k);
    l = n >= 2 ? n + (k - top) / (unsigned)(top - r->vthr[n - 2]) : k / r->vthr[0];
    return l > DK_VET_CAPTURE ? DK_VET_CAPTURE : l;
}

static unsigned stock_level(unsigned k) { return k / 5 > 5 ? 5 : k / 5; }

static void vet_event(unsigned* n, const char* what)
{
    unsigned c = ++*n;
    if ((c & (c - 1)) == 0) dlog("veterancy: %s (%u so far)", what, c);
}
static unsigned s_evTaken, s_evDealt, s_evReload, s_evLead, s_evAim, s_evCapture, s_evReclaim,
                s_evReclaimFit, s_evLabel;

static const DkUnit* vet_keyed(const char* u)
{
    const DkUnit* r = vet_row(u);
    return r && r->vthr_n ? r : NULL;
}

/* THE SITES' ANSWERS, each on the GAME thread in play: -1 runs stock's own
   instructions; any other value is the keyed type's. */

/* 0x489BFA, the damage a hit's victim takes: L for 0x489C16's (25 - L) * 4 %.
   A call of 30 000 or more never reaches it (the kill-outright exemption at
   0x489BF3). */
static unsigned taken_level(const DkUnit* r, unsigned k)
{
    unsigned l = vet_level(r, k);
    return l > DK_VET_TAKEN ? DK_VET_TAKEN : l;
}

int __stdcall tagpu_datakeys_vet_taken(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    if (!r) return -1;
    vet_event(&s_evTaken, "a keyed type's level set the damage it took");
    return (int)taken_level(r, kills_of(u));
}

/* 0x499DB5, the damage a shooter deals: L for 0x499DD1's (100 + 6L) %. L is
   at most DK_VET_MAX, so the product stays far inside an int; the HP word's
   saturation (0x489C71) bounds what the hit can do. */
int __stdcall tagpu_datakeys_vet_dealt(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    if (!r) return -1;
    vet_event(&s_evDealt, "a keyed type's level set the damage it dealt");
    return (int)vet_level(r, kills_of(u));
}

/* 0x49E468, a slot's reload: L for 0x49E48D's (100 - 6L) %. */
int __stdcall tagpu_datakeys_vet_reload(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    unsigned l;
    if (!r) return -1;
    l = vet_level(r, kills_of(u));
    vet_event(&s_evReload, "a keyed type's level set a reload");
    return (int)(l > DK_VET_RELOAD ? DK_VET_RELOAD : l);
}

/* the same level for the extra-weapons module's own reload (tagpu_weapons.c),
   stock's min(kills / 5, 5) for a type without the key */
int tagpu_datakeys_vet_reload_level(const char* u)
{
    int l = tagpu_datakeys_vet_reload(u);
    return l < 0 ? (int)stock_level(kills_of(u)) : l;
}

/* 0x48A324, target lead: on past the first threshold, as stock's is past 5 */
int __stdcall tagpu_datakeys_vet_lead(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    if (!r) return -1;
    vet_event(&s_evLead, "a keyed type's first threshold decided its target lead");
    return kills_of(u) > r->vthr[0] ? 1 : 0;
}

/* 0x49D6EA, the fire method's spread: the divisor kills / rate, which the
   engine applies when it exceeds 1 (0x49D702). Rate 0 is no buff. Stock's
   divisor is kills / 12, which TADR documents as its default rate; so the
   formula is the code's, and TADR's written "1 + kills / rate" is not. */
int __stdcall tagpu_datakeys_vet_accuracy(const char* u)
{
    const DkUnit* r = vet_row(u);
    if (!r || !r->vrate_on) return -1;
    vet_event(&s_evAim, "a keyed type's rate set its spread");
    return r->vrate ? (int)(kills_of(u) / r->vrate) : 0;
}

/* 0x4043D8, the capture's cost, set once as the capture starts and read of
   the TARGET: 10 + L tenths of it for 0x4043EC, stock's being 10 + kills / 5 */
int __stdcall tagpu_datakeys_vet_capture_cost(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    if (!r) return -1;
    vet_event(&s_evCapture, "a keyed target's level set a capture's cost");
    return 10 + (int)vet_level_open(r, kills_of(u));
}

/* 0x43869D, inside 0x438650, a unit reclaim's step: the HP the reclaimer
   takes from its target every 15 ticks (0x404981 deals it as a kind-5 hit;
   0x438650's callers are the reclaim order 0x40483D and the build order's
   reclaim 0x414C86), read of the RECLAIMER: L + 1 for 0x4386B9, stock's
   being (kills + 5) / 5.
   THE HOLD: the factor is held to the largest for which the workertime, the
   factor, the target's MaxHitPoints (def +0x1FA) and the ticks between steps
   multiply to below 2^32, the product stock's 0x4386B9..0x4386C3 take in 32
   bits (ARMCOM's 300 on a CORKROG wraps at 32). A product that does not fit
   at 1 is the engine's own at a recruit's factor, and the answer is 1.
   B10 (tagpu_patches.c, fix_reclaim_wrap) takes that product in 64 bits in
   both builds, so the hold no longer prevents a wrap: it caps a keyed
   veteran's step below its formula. Whether to release it is open for the
   owner (sim-fixes.md, B10).
   Called with the engine's x87 stack live (tagpu_patches.c, fix_veterancy): integer code only. */
int __stdcall tagpu_datakeys_vet_reclaim_step(const char* u, unsigned workertime,
                                              unsigned maxhp, unsigned ticks)
{
    const DkUnit* r = vet_keyed(u);
    unsigned f, fit;
    if (!r) return -1;
    f = 1 + vet_level_open(r, kills_of(u));
    if (workertime && maxhp && ticks) {
        fit = 0xFFFFFFFFu / workertime / maxhp / ticks;
        if (f > fit) {
            f = fit ? fit : 1;
            vet_event(&s_evReclaimFit, "a keyed reclaimer's step was held to what its product holds");
        }
    }
    vet_event(&s_evReclaim, "a keyed reclaimer's level set a unit reclaim's step");
    return (int)f;
}

/* the level a hit's victim would take the reduction of, keyed or stock: the
   kill-outright exemption counts the calls it changed with it */
unsigned tagpu_datakeys_vet_taken_level(const char* u)
{
    const DkUnit* r = vet_keyed(u);
    return r ? taken_level(r, kills_of(u)) : stock_level(kills_of(u));
}

/* --- the panels ----------------------------------------------------------------
   Both kill lines, the unit panel's 0x46AEE0 (at 0x46B306, the unit in esi)
   and the second panel's 0x467CB0 (at 0x467CCF, the unit in edi), print
   "N kills - Veteran" past 4 kills and "N kill(s)" otherwise. Here every unit
   at level 1 or more says "VetL", its own type's level or stock's for a type
   without the key, as TADR does; below level 1 the line is stock's short
   one. The singular "kill" stays stock's choice, which TADR drops. DISPLAY:
   a mismatch skips and logs. Both run on the game thread inside the panel's
   draw, and each site has its own buffer, used by the sprintf that follows
   the site before anything else runs. */
static char s_labelPanel[16], s_labelDev[16];

static const char* vet_label(const char* u, char* buf)
{
    const DkUnit* r = vet_keyed(u);
    unsigned k = kills_of(u), l = r ? vet_level(r, k) : stock_level(k);
    if (!l) return NULL;
    _snprintf(buf, 16, "Vet%u", l);
    buf[15] = 0;
    vet_event(&s_evLabel, "a panel named a veteran's level");
    return buf;
}

static const char* __stdcall label_panel(const char* u) { return vet_label(u, s_labelPanel); }
static const char* __stdcall label_dev(const char* u)   { return vet_label(u, s_labelDev); }

#define VA_PANEL_KILLS  0x0046B306u
#define VA_DEV_KILLS    0x00467CCFu
static const unsigned char PANEL_KILLS_WAS[7] = { 0x66, 0x8B, 0x8E, 0xB8, 0x00, 0x00, 0x00 }; /* mov cx,[esi+0xB8] */
static const unsigned char DEV_KILLS_WAS[7]   = { 0x66, 0x8B, 0x8F, 0xB8, 0x00, 0x00, 0x00 }; /* mov cx,[edi+0xB8] */

/* pushad; push <unit>; call label; mov [esp+0x18],eax (popad's ecx); popad;
   test ecx,ecx; jz short; cmp word [<unit>+0xB8],1; jne; <the singular>;
   mov eax,ecx; jmp <vet>; short: mov cx,[<unit>+0xB8]; jmp <short>. eax is
   the translated "kill" at both sites, kept by popad for the short line and
   for the singular; ecx is rewritten by both lines before it is read.
   unit_reg is 6 (esi) or 7 (edi); `single` stores eax where the site's own
   singular does. */
static unsigned char* label_stub(unsigned char unit_reg, const void* fn,
                                 const unsigned char* single, int nsingle,
                                 unsigned vet, unsigned shortline)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char *p = s, *jz, *jne;
    if (!s) return NULL;
    *p++ = 0x60;                                            /* pushad             */
    *p++ = (unsigned char)(0x50 + unit_reg);                /* push esi / edi     */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)fn); p += 4;
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x18;     /* mov [esp+0x18],eax */
    *p++ = 0x61;                                            /* popad              */
    *p++ = 0x85; *p++ = 0xC9;                               /* test ecx,ecx       */
    *p++ = 0x74; jz = p++;                                  /* jz short           */
    *p++ = 0x66; *p++ = 0x83; *p++ = (unsigned char)(0xB8 + unit_reg); /* cmp word [reg+0xB8],1 */
    *p++ = 0xB8; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x01;
    *p++ = 0x75; jne = p++;                                 /* jne plural         */
    memcpy(p, single, (size_t)nsingle); p += nsingle;
    *jne = (unsigned char)(p - (jne + 1));
    *p++ = 0x8B; *p++ = 0xC1;                               /* mov eax,ecx        */
    *p++ = 0xE9; tagpu_detour_rel(p, vet); p += 4;
    *jz = (unsigned char)(p - (jz + 1));
    *p++ = 0x66; *p++ = 0x8B; *p++ = (unsigned char)(0x88 + unit_reg); /* mov cx,[reg+0xB8] */
    *p++ = 0xB8; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0xE9; tagpu_detour_rel(p, shortline);
    return s;
}

static void vet_panels_install(void)
{
    static const unsigned char single_panel[4] = { 0x89, 0x44, 0x24, 0x10 }; /* mov [esp+0x10],eax (0x46B319) */
    static const unsigned char single_dev[2]   = { 0x8B, 0xF0 };             /* mov esi,eax (0x467CE2)       */
    unsigned char *a, *b;
    if (!site_is(VA_PANEL_KILLS, PANEL_KILLS_WAS, 7) || !site_is(VA_DEV_KILLS, DEV_KILLS_WAS, 7)) {
        dlog("veterancy: the panels NOT installed: the bytes at 0x46B306 or 0x467CCF are not the "
             "retail ones; a veteran's line reads stock's \"Veteran\"");
        return;
    }
    a = label_stub(6, (const void*)label_panel, single_panel, 4, 0x0046B331u, 0x0046B358u);
    b = label_stub(7, (const void*)label_dev, single_dev, 2, 0x00467CEEu, 0x00467D0Eu);
    /* both lines or neither: the first is put back when the second fails */
    if (!a || !b || !tagpu_detour_land(VA_PANEL_KILLS, a, 7)) {
        dlog("veterancy: the panels NOT installed (a stub or a write failed); a veteran's line "
             "reads stock's \"Veteran\"");
        return;
    }
    if (!tagpu_detour_land(VA_DEV_KILLS, b, 7)) {
        int back = tagpu_detour_write(VA_PANEL_KILLS, PANEL_KILLS_WAS, 7);
        dlog("veterancy: the panels NOT installed (the write at 0x467CCF failed; 0x46B306 %s); "
             "a veteran's line reads stock's \"Veteran\"",
             back ? "put back" : "COULD NOT be put back and names the level");
        return;
    }
    dlog("veterancy: both kill lines name the level, \"VetN\" (0x46B306, 0x467CCF)");
}
