/* tagpu_datakeys.c — TADR section C's unit keys: the FBI reader and the build
   ghost's piece mask. The plan is research/notes/tadr-port/data-keys.md; the
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

/* ONE RECORD PER UnitDef SLOT, written by the FBI reader below every time the
   engine loads that slot's FBI, whether or not the file carries a key: a
   record that is always rewritten is always fresh. A reader asks for slot
   `type` and takes the row only when its `def` is the def it asked about, so
   a row left from an earlier game reads as "no keys" (the extra-weapons
   module's def_rec rule).

   WHO WRITES AND WHO READS, AND WHY THEY NEVER OVERLAP. The level's unit-data
   load 0x42D2E0 runs the FBI loader on the LOADER thread (LoadGameData_Main
   0x4917D0, called from the loader body 0x497180); the console's one-type
   reload 0x417490 -> 0x42D1F0 runs it on the GAME thread. The only reader is
   the game thread, inside an in-play packet fill. The loader thread writes
   between a level's teardown and its first in-play draw, which is the same
   ordering every game-thread read of the def array itself rests on (the
   in-play publish point, exe-reverse-engineering.md); the reload is on the
   reader's own thread. The render thread never reads a record. */
typedef struct DkUnit {
    const char* def;        /* the UnitDef this row was written for           */
    unsigned    serial;     /* a fresh number at every write: the mask cache's
                               key, so a reloaded FBI is re-read              */
    char*       pp;         /* PreviewPieces=, parsed: pp_n lower-case names,
                               each NUL-ended, back to back; NULL = absent    */
    unsigned    pp_n;
} DkUnit;

static DkUnit   s_unit[TAGPU_LIM_TYPES];
static unsigned s_serial;

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
   2. The FBI reader: 0x42BF97, inside the FBI loader 0x42BF40
   ========================================================================= */

/* THE SITE. 0x42BF40(path, def) opens the FBI and finds its [UNITINFO]
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

    if (!def_slot(def, &slot)) {
        static int once;
        if (!once) { once = 1; dlog("FBI reader: a def outside the def array's slots, skipped (logged once)"); }
        return;
    }
    r = &s_unit[slot];
    free(r->pp);
    r->pp = NULL; r->pp_n = 0;
    r->serial = ++s_serial;
    r->def = def;

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

/* A DRAW KEY, SO A MISMATCH SKIPS AND LOGS: without the reader no type carries
   PreviewPieces=, and every ghost takes its Create() mask, which needs no
   reader at all. */
void tagpu_datakeys_init(void)
{
    if (fbi_install()) dlog("FBI reader installed at 0x42BF97 (PreviewPieces=)");
    else dlog("FBI reader NOT installed: the bytes at 0x42BF97 are not the retail loader's; "
              "PreviewPieces= is not read and every ghost takes its Create() mask");
}

/* =========================================================================
   3. The build ghost's piece mask
   ========================================================================= */

/* THE COB AS 0x4B2450 LEAVES IT: the file read whole, its header's offsets
   turned into pointers in place (0x4B24A7..0x4B2527, DISASSEMBLED). The two
   name tables have every entry relocated too; the entry-point table does not,
   its entries being code offsets in dwords. Nothing in the loader checks a
   count or an offset, so every one of them is DATA here, bounded below. */
#define COB_VERSION   0x00
#define COB_NSCRIPTS  0x04
#define COB_NPIECES   0x08
#define COB_CODELEN   0x0C      /* dwords                                      */
#define COB_ENTRIES   0x18      /* int32[nscripts]: code offsets, dwords       */
#define COB_SNAMES    0x1C      /* char*[nscripts]                             */
#define COB_PNAMES    0x20      /* char*[npieces]                              */
#define COB_CODE      0x24      /* int32[codelen]                              */

/* data bounds, an order of magnitude above stock (the largest stock model has
   36 pieces): they stop a malformed script running away, and a script past
   one gets no mask rather than part of one */
#define DK_MAX_SCRIPTS   4096
#define DK_MAX_COBPIECES 1024
#define DK_MAX_CODE      (1 << 22)
#define DK_NAMECMP       256    /* the longest name a compare reads            */

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
   fold) for two engine strings: 1 equal, 0 not. -1 when a pointer fails the
   range test or both agree for DK_NAMECMP characters without ending: the
   caller then refuses the mask rather than guess what the engine's unbounded
   compare said. */
static int name_eq(const char* a, const char* b)
{
    int i;
    if (!ptr_ok(a) || !ptr_ok(b)) return -1;
    for (i = 0; i < DK_NAMECMP; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return -1;
}

/* the same, against a lower-case literal of ours */
static int name_is(const char* a, const char* lit)
{
    int i;
    if (!ptr_ok(a)) return -1;
    for (i = 0; i < DK_NAMECMP; i++) {
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
   a script with no Create(), -1 for a header the bounds refuse. */
static int create_hides(const char* cob, unsigned* hid)
{
    int ns, np, cl, i, ci = -1, pc;
    const int* entries;
    const char* const* snames;
    const int* code;
    if (*(const int*)(cob + COB_VERSION) != 4) return -1;
    ns = *(const int*)(cob + COB_NSCRIPTS);
    np = *(const int*)(cob + COB_NPIECES);
    cl = *(const int*)(cob + COB_CODELEN);
    if (ns <= 0 || ns > DK_MAX_SCRIPTS || np < 0 || np > DK_MAX_COBPIECES ||
        cl <= 0 || cl > DK_MAX_CODE) return -1;
    entries = *(const int* const*)(cob + COB_ENTRIES);
    snames  = *(const char* const* const*)(cob + COB_SNAMES);
    code    = *(const int* const*)(cob + COB_CODE);
    if (!ptr_ok(entries) || !ptr_ok(snames) || !ptr_ok(code)) return -1;
    for (i = 0; i < ns; i++) {
        int e = name_is(snames[i], "create");
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
   position below its own index. Returns the primitive count, -1 when the
   tree exceeds TAGPU_PBMAXPIECE or a compare cannot be answered. */
static int engine_prims(const char* root, const char* const* pnames, int ncob, int upto,
                        const char** prim)
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
            int e = name_eq(pnames[i], *(const char* const*)(prim[j] + N_NAME));
            if (e < 0) return -1;
            if (e) break;
        }
        if (j < n && j != i) { const char* t = prim[i]; prim[i] = prim[j]; prim[j] = t; }
    }
    return n;
}

/* the Create() mask, as bits over the ghost's walk: 1 with bits set, 0 when
   nothing is hidden, -1 when the script or the model is refused */
static int create_mask(const char* root, const char* cob, const char* const* walk, int np,
                       uint32_t* bits)
{
    static unsigned    hid[DK_MAX_COBPIECES / 32];
    static const char* prim[TAGPU_PBMAXPIECE];
    const char*        hnode[TAGPU_PBMAXPIECE];
    const char* const* pnames;
    int ncob, nprim, k, g, nh = 0, upto = -1;

    if (!ptr_ok(cob)) return 0;                        /* no script: nothing hidden */
    ncob = create_hides(cob, hid);
    if (ncob <= 0) return ncob;
    for (k = 0; k < ncob; k++) if (hid[k >> 5] >> (k & 31) & 1) upto = k;
    if (upto < 0) return 0;
    pnames = *(const char* const* const*)(cob + COB_PNAMES);
    if (!ptr_ok(pnames)) return -1;
    nprim = engine_prims(root, pnames, ncob, upto, prim);
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
   template and the COB are freed at every level end), the record's serial (a
   console reload re-reads the FBI, and its COB may come back at the same
   address), and the two pointers themselves. What it holds is VALUES — bits,
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

static void ghost_compute(const char* def, const char* root, const char* cob,
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
    r = create_mask(root, cob, walk, np, g->bits);
    if (r < 0) {
        memset(g->bits, 0, sizeof g->bits);
        dlog("ghost mask: %.32s's script or model is refused by the bounds; every piece shows",
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
        ghost_compute(def, root, cob, rec, g);
        g->gen = gen; g->serial = serial; g->root = root; g->cob = cob; g->valid = 1;
    }
    if (!g->any) return 0;
    out->type    = (uint16_t)type;
    out->npieces = g->np;
    out->root    = (uint32_t)(size_t)root;
    memcpy(out->bits, g->bits, sizeof out->bits);
    return 1;
}
