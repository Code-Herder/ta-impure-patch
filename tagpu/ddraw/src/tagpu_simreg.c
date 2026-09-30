/* tagpu_simreg.c -- the simulation-state registry: its walker and the wrappers the `sim` files
   take memory, TLS indices and levers through (inc/tagpu_simreg.h). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_simreg.h"
#include "tagpu_log.h"

/* The list's ends. The linker sorts `.rdata$simreg$*` by name, so every declaration (`$m`, one
   4-byte pointer each, 4-byte aligned) lies between these two with nothing else in between. */
static const SimDesc* const simreg_first __attribute__((used, section(".rdata$simreg$a"))) = NULL;
static const SimDesc* const simreg_last  __attribute__((used, section(".rdata$simreg$z"))) = NULL;

/* The walk leaves the object the marker is: an empty asm makes the pointer opaque, so the
   compiler cannot reason from "one pointer object" to "no second element". */
static const SimDesc* const* sim_opaque(const SimDesc* const* p)
{
    __asm__("" : "+r"(p));
    return p;
}

/* Set once the attach line is logged: install is over, and a lever or TLS index taken after
   it is logged by name. Written on DllMain's thread before any game thread exists. */
static volatile LONG s_closed;

const SimField tagpu_sim_addr[]  = { SIM_FIELD_AT(0, sizeof(void*), SIMF_ADDR, SIM_ID_NONE),
                                     SIM_FIELD_END };
const SimField tagpu_sim_local[] = { SIM_FIELD_AT(0, 4, SIMF_LOCAL, SIM_ID_NONE), SIM_FIELD_END };
const SimField tagpu_sim_owned[] = { SIM_FIELD_AT(0, sizeof(void*), SIMF_OWNED, SIM_ID_NONE),
                                     SIM_FIELD_END };

static void sim_fail(const SimDesc* d, const char* what)
{
    tagpu_logf("simreg: FAILED %s (declared at line %u) %s", d->name, d->line, what);
}

static int sim_owns(const SimDesc* d)
{
    const SimField* f;
    if (d->layout)
        for (f = d->layout; f->size; f++)
            if (f->kind == SIMF_OWNED) return 1;
    return 0;
}

void* tagpu_sim_alloc(const SimDesc* d, size_t bytes)
{
    if (d->flags & SIMD_CODE)
        return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (d->flags & SIMD_GUARD)
        return VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (d->flags & SIMD_HEAP)               /* the recorder asks which pages were written */
        return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (d->flags & SIMD_TLS)
        return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    sim_fail(d, "allocates, but is declared as no heap table, TLS slot, code or guard");
    return NULL;
}

void tagpu_sim_free(const SimDesc* d, void* p)
{
    if (!p) return;
    if (d->flags & (SIMD_CODE | SIMD_GUARD | SIMD_HEAP | SIMD_TLS))
        VirtualFree(p, 0, MEM_RELEASE);
    else
        sim_fail(d, "frees, but is declared as no heap table, TLS slot, code or guard");
}

void* tagpu_sim_alloc_owned(const SimDesc* d, size_t bytes)
{
    if (sim_owns(d)) return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    sim_fail(d, "allocates a block its rows own, but its layout names no owned field");
    return NULL;
}

void tagpu_sim_free_owned(const SimDesc* d, void* p)
{
    if (!p) return;
    if (sim_owns(d))
        HeapFree(GetProcessHeap(), 0, p);
    else
        sim_fail(d, "frees a block its rows own, but its layout names no owned field");
}

void* tagpu_sim_scratch(size_t bytes)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}

void tagpu_sim_scratch_free(void* p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

int tagpu_sim_tls(const SimDesc* d)
{
    DWORD* ix = (DWORD*)d->addr;
    if (!(d->flags & SIMD_TLS)) {
        sim_fail(d, "takes a TLS index, but is not declared SIM_TLS");
        return 0;
    }
    if (s_closed) sim_fail(d, "takes its TLS index after install");
    if (*ix == TLS_OUT_OF_INDEXES) *ix = TlsAlloc();
    return *ix != TLS_OUT_OF_INDEXES;
}

/* ---- levers --------------------------------------------------------------------------------- */

#define SIM_LEVERS     32
#define SIM_LEVER_TEXT 256      /* what the attach line and a recording keep of a lever's text */
#define SIM_LEVER_SHOW 32       /* what the attach line shows of it                             */

/* One entry per lever: a lever several installers consult is one setting of the game. `len`
   and `hash` cover every byte a reader read, so two texts that differ past SIM_LEVER_TEXT
   still differ here. */
static struct {
    const char* file;
    int         set;
    int         len;                    /* bytes of its text read; -1 when only its presence was */
    unsigned    hash;                   /* FNV-1a of those bytes                                  */
    char        text[SIM_LEVER_TEXT];   /* the first of them, NUL-ended                           */
} s_lever[SIM_LEVERS];
static unsigned s_levers;     /* distinct levers read while installing, including any past the list */

static unsigned lever_hash(const char* s, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void lever_keep(unsigned i, const char* text, int len, unsigned hash)
{
    int keep = len < SIM_LEVER_TEXT ? len : SIM_LEVER_TEXT - 1;
    memcpy(s_lever[i].text, text, (size_t)keep);
    s_lever[i].text[keep] = 0;
    s_lever[i].len = len;
    s_lever[i].hash = hash;
}

/* Two answers for one name mean the file came, went or changed while installing, so the
   installers disagree about which game this is. `text` is NULL for a presence read. */
static void lever_note(const char* file, int set, const char* text, int len)
{
    unsigned i, hash = text ? lever_hash(text, len) : 0;
    if (s_closed) {
        tagpu_logf("simreg: FAILED lever %s is read after install, so no recording lists it", file);
        return;
    }
    for (i = 0; i < s_levers && i < SIM_LEVERS; i++) {
        if (strcmp(s_lever[i].file, file)) continue;
        if (s_lever[i].set != set)
            tagpu_logf("simreg: FAILED lever %s read %s, then %s, while installing", file,
                       s_lever[i].set ? "present" : "absent", set ? "present" : "absent");
        else if (text && s_lever[i].len >= 0 && (s_lever[i].len != len || s_lever[i].hash != hash))
            tagpu_logf("simreg: FAILED lever %s read two texts while installing", file);
        else if (text && s_lever[i].len < 0)
            lever_keep(i, text, len, hash);
        return;
    }
    if (s_levers < SIM_LEVERS) {
        s_lever[s_levers].file = file;
        s_lever[s_levers].set = set;
        s_lever[s_levers].len = -1;
        if (text) lever_keep(s_levers, text, len, hash);
    } else {
        tagpu_logf("simreg: FAILED lever %s is past the list's %d entries", file, SIM_LEVERS);
    }
    s_levers++;
}

int tagpu_sim_lever(const char* file)
{
    int set = GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
    lever_note(file, set, NULL, 0);
    return set;
}

int tagpu_sim_lever_read(const char* file, char* buf, int cap)
{
    FILE* f = fopen(file, "rb");
    int n = -1;
    if (cap > 0) buf[0] = 0;
    if (f) {
        n = cap > 1 ? (int)fread(buf, 1, (size_t)cap - 1, f) : 0;
        fclose(f);
        if (cap > 0) buf[n] = 0;
    }
    lever_note(file, n >= 0, n >= 0 && cap > 0 ? buf : NULL, n);
    return n;
}

/* the start of a lever's text for the attach line: trailing blanks dropped, anything but
   printable ASCII shown as '?', "..." when more follows */
static void lever_show(char* out, size_t cap, unsigned i)
{
    const char* t = s_lever[i].text;
    size_t n = strlen(t), k, j = 0;
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\r' || t[n - 1] == '\n')) n--;
    for (k = 0; k < n && k < SIM_LEVER_SHOW && j + 1 < cap; k++)
        out[j++] = (t[k] >= 0x20 && t[k] < 0x7F && t[k] != '"') ? t[k] : '?';
    if ((k < n || s_lever[i].len > SIM_LEVER_TEXT - 1) && j + 3 < cap) {
        memcpy(out + j, "...", 3);
        j += 3;
    }
    out[j] = 0;
}

unsigned tagpu_simreg_levers(void)
{
    return s_closed ? (s_levers < SIM_LEVERS ? s_levers : SIM_LEVERS) : 0;
}

const char* tagpu_simreg_lever_at(unsigned i, int* set, const char** text, int* len)
{
    if (i >= tagpu_simreg_levers()) return NULL;
    *set = s_lever[i].set;
    *text = s_lever[i].len >= 0 ? s_lever[i].text : NULL;
    *len = s_lever[i].len;
    return s_lever[i].file;
}

/* ---- the walk ------------------------------------------------------------------------------ */

/* The declarations the attach walk accepted, in the list's order: a recording names a table by
   its index here, and its header maps each index to the declaration's name. Written once, by the
   attach walk on DllMain's thread, before any game thread exists. */
#define SIM_MAXDECL 1024
#define SIM_STR_(x) #x
#define SIM_STR(x)  SIM_STR_(x)
static const SimDesc* s_ok[SIM_MAXDECL];
static unsigned       s_nok;

unsigned tagpu_simreg_count(void)
{
    return s_closed ? s_nok : 0;
}

const SimDesc* tagpu_simreg_at(unsigned i)
{
    return i < tagpu_simreg_count() ? s_ok[i] : NULL;
}

/* NULL when the declaration's shape is one the recorder can walk, else what is wrong with it */
static const char* sim_shape(const SimDesc* d)
{
    const SimField* f;
    int count = 0, column = 0;
    if (d->cls >= SIM_CLASSES)                    return "has no class";
    if (d->key > SIM_ID_DPLAY)                    return "has an unknown key";
    if (!d->stride)                               return "has rows of 0 bytes";
    if (!d->nrows && !(d->rows && (d->flags & SIMD_HEAP)))
                                                  return "has no row count";
    if ((d->flags & (SIMD_LOCK | SIMD_CODE | SIMD_GUARD)) && d->cls != SIM_INSTALL)
                                                  return "is a lock, code or guard that is not INSTALL";
    if (!d->layout) return NULL;
    for (f = d->layout; f->size; f++) {
        unsigned span = f->off + f->size + (f->repeat > 1 ? (unsigned)(f->repeat - 1) * f->step : 0);
        if (span > d->stride)                     return "has a layout field past the end of its row";
        if (f->kind > SIMF_PINNED || f->id > SIM_ID_DPLAY)
                                                  return "has a layout field of an unknown kind";
        if (f->kind == SIMF_COUNT) count++;
        if (f->kind == SIMF_COLUMN) {
            if (!f->step)                         return "has a column with no element size";
            column++;
        }
    }
    if (column && count != 1)                     return "has columns without exactly one count";
    return NULL;
}

/* the bytes of the variable itself: a heap table's is its pointer, whatever its rows are */
static size_t sim_extent(const SimDesc* d)
{
    return (d->flags & SIMD_HEAP) ? sizeof(void*) : (size_t)d->stride * d->nrows;
}

/* The first well-shaped declaration before `at` that covers a byte `d` covers, or NULL. A
   variable declared twice is how the build check's count by name can miss an undeclared
   namesake, so no byte may have two declarations. */
static const SimDesc* sim_twin(const SimDesc* const* p, const SimDesc* const* at,
                               const SimDesc* d)
{
    size_t lo = (size_t)d->addr, hi = lo + sim_extent(d);
    for (; p < at; p++) {
        const SimDesc* e = *p;
        size_t elo, ehi;
        if (!e || sim_shape(e)) continue;
        elo = (size_t)e->addr;
        ehi = elo + sim_extent(e);
        if (lo < ehi && elo < hi) return e;
    }
    return NULL;
}

void tagpu_simreg_attach(void)
{
    static const char* const cname[SIM_CLASSES] = { "state", "content", "transient", "private",
                                                    "install" };
    const SimDesc* const* first = sim_opaque(&simreg_first) + 1;
    const SimDesc* const* end   = sim_opaque(&simreg_last);
    const SimDesc* const* p;
    unsigned n = 0, bad = 0, locks = 0, tls = 0, heap = 0, code = 0, engine = 0, laid = 0, i;
    unsigned cls[SIM_CLASSES] = { 0 };
    char classes[160], levers[1024], show[SIM_LEVER_SHOW + 4];
    int k = 0;

    for (p = first; p < end; p++) {
        const SimDesc* d = *p;
        const char* why = d ? sim_shape(d) : "is a NULL entry in the list";
        const SimDesc* twin = why ? NULL : sim_twin(first, p, d);
        n++;
        if (!d) {
            bad++;
            tagpu_logf("simreg: FAILED entry %u %s", n, why);
            continue;
        }
        if (why || twin) {
            bad++;
            if (why) sim_fail(d, why);
            else tagpu_logf("simreg: FAILED %s (declared at line %u) declares memory that %s "
                            "(line %u) declares", d->name, d->line, twin->name, twin->line);
            continue;
        }
        if (s_nok < SIM_MAXDECL) {
            s_ok[s_nok++] = d;
        } else {
            bad++;
            sim_fail(d, "is past the recorder's list of " SIM_STR(SIM_MAXDECL) " declarations");
            continue;
        }
        cls[d->cls]++;
        if (d->flags & SIMD_LOCK) locks++;
        if (d->flags & SIMD_TLS) tls++;
        if (d->flags & SIMD_HEAP) heap++;
        if (d->flags & (SIMD_CODE | SIMD_GUARD)) code++;
        if (d->flags & SIMD_ENGINE) engine++;
        if (d->layout) laid++;
    }
    for (i = 0; i < SIM_CLASSES; i++)
        k += snprintf(classes + k, sizeof classes - (size_t)k, "%s%u %s", i ? ", " : "", cls[i],
                      cname[i]);
    k = snprintf(levers, sizeof levers, "%u read", s_levers);
    for (i = 0; i < s_levers && i < SIM_LEVERS && k > 0 && (size_t)k < sizeof levers; i++) {
        if (!s_lever[i].set) continue;
        if (s_lever[i].len >= 0) {
            lever_show(show, sizeof show, i);
            k += snprintf(levers + k, sizeof levers - (size_t)k, ", %s=\"%s\"", s_lever[i].file,
                          show);
        } else {
            k += snprintf(levers + k, sizeof levers - (size_t)k, ", %s set", s_lever[i].file);
        }
    }
    InterlockedExchange(&s_closed, 1);
    tagpu_logf("simreg: %u declarations (%s; %u locks, %u TLS indices, %u heap tables, "
               "%u code or guard pointers, %u engine pools, %u layouts), %u malformed; levers: %s",
               n, classes, locks, tls, heap, code, engine, laid, bad, levers);
}
