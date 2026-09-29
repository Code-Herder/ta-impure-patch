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
    if (d->flags & (SIMD_HEAP | SIMD_TLS))
        return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (sim_owns(d))
        return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    sim_fail(d, "allocates, but is declared as no heap table, TLS slot, code, guard or owner");
    return NULL;
}

void tagpu_sim_free(const SimDesc* d, void* p)
{
    if (!p) return;
    if (d->flags & (SIMD_CODE | SIMD_GUARD | SIMD_HEAP | SIMD_TLS))
        VirtualFree(p, 0, MEM_RELEASE);
    else if (sim_owns(d))
        HeapFree(GetProcessHeap(), 0, p);
    else
        sim_fail(d, "frees, but is declared as no heap table, TLS slot, code, guard or owner");
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

#define SIM_LEVERS 32
static struct { const char* file; int set; } s_lever[SIM_LEVERS];
static unsigned s_levers;     /* distinct levers read while installing, including any past the list */

/* One entry per lever: a lever several installers consult is one setting of the game. Two
   answers for one name mean the file came or went while installing, so the installers disagree
   about which game this is. */
static void lever_note(const char* file, int set)
{
    unsigned i;
    if (s_closed) {
        tagpu_logf("simreg: FAILED lever %s is read after install, so no recording lists it", file);
        return;
    }
    for (i = 0; i < s_levers && i < SIM_LEVERS; i++)
        if (!strcmp(s_lever[i].file, file)) {
            if (s_lever[i].set != set)
                tagpu_logf("simreg: FAILED lever %s read %s, then %s, while installing", file,
                           s_lever[i].set ? "set" : "unset", set ? "set" : "unset");
            return;
        }
    if (s_levers < SIM_LEVERS) {
        s_lever[s_levers].file = file;
        s_lever[s_levers].set = set;
    } else {
        tagpu_logf("simreg: FAILED lever %s is past the list's %d entries", file, SIM_LEVERS);
    }
    s_levers++;
}

int tagpu_sim_lever(const char* file)
{
    int set = GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES;
    lever_note(file, set);
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
    lever_note(file, n >= 0);
    return n;
}

/* ---- the walk ------------------------------------------------------------------------------ */

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

void tagpu_simreg_attach(void)
{
    static const char* const cname[SIM_CLASSES] = { "state", "content", "transient", "private",
                                                    "install" };
    const SimDesc* const* p   = sim_opaque(&simreg_first) + 1;
    const SimDesc* const* end = sim_opaque(&simreg_last);
    unsigned n = 0, bad = 0, locks = 0, tls = 0, heap = 0, code = 0, engine = 0, laid = 0, i;
    unsigned cls[SIM_CLASSES] = { 0 };
    char classes[160], levers[512];
    int k = 0;

    for (; p < end; p++) {
        const SimDesc* d = *p;
        const char* why = d ? sim_shape(d) : "is a NULL entry in the list";
        n++;
        if (why) {
            bad++;
            if (d) sim_fail(d, why);
            else tagpu_logf("simreg: FAILED entry %u %s", n, why);
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
    for (i = 0; i < s_levers && i < SIM_LEVERS && k > 0 && (size_t)k < sizeof levers; i++)
        if (s_lever[i].set)
            k += snprintf(levers + k, sizeof levers - (size_t)k, ", %s set", s_lever[i].file);
    InterlockedExchange(&s_closed, 1);
    tagpu_logf("simreg: %u declarations (%s; %u locks, %u TLS indices, %u heap tables, "
               "%u code or guard pointers, %u engine pools, %u layouts), %u malformed; levers: %s",
               n, classes, locks, tls, heap, code, engine, laid, bad, levers);
}
