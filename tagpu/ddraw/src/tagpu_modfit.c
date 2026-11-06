#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modfit.h"
#include "tagpu_modcat.h"
#include "tagpu_modinst.h"
#include "tagpu_modload.h"
#include "tagpu_modpak.h"
#include "tagpu_profiles.h"
#include "tagpu_hpi.h"

#define FILE_MAX (64u << 20)    /* a file a mutator replaces; a larger one reads as differing */

typedef struct { TAGPU_HPI* hpi; HANDLE file; } ARC;

struct MODFIT {
    ARC* arc;
    int  n, cap;
};

static int arc_read(void* ctx, uint32_t off, void* dst, uint32_t n)
{
    HANDLE h = (HANDLE)ctx;
    LARGE_INTEGER at;
    DWORD got;
    at.QuadPart = off;
    return SetFilePointerEx(h, at, NULL, FILE_BEGIN) && ReadFile(h, dst, n, &got, NULL) && got == n;
}

/* an archive that cannot be read holds nothing here, and the mutator reads as not fitting what it replaces there */
static void add(MODFIT* f, const char* path)
{
    char why[160];
    TAGPU_HPISRC src;
    LARGE_INTEGER size;
    ARC* a;
    if (f->n == f->cap) {
        const int cap = f->cap ? f->cap * 2 : 64;
        ARC* grown = (ARC*)realloc(f->arc, (size_t)cap * sizeof *grown);
        if (!grown) return;
        f->arc = grown;
        f->cap = cap;
    }
    a = &f->arc[f->n];
    a->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (a->file == INVALID_HANDLE_VALUE) return;
    if (!GetFileSizeEx(a->file, &size) || size.QuadPart > 0x7FFFFFFF) { CloseHandle(a->file); return; }
    src.ctx = a->file;
    src.size = (uint32_t)size.QuadPart;
    src.read = arc_read;
    if (!(a->hpi = tagpu_hpi_open(&src, why, sizeof why))) { CloseHandle(a->file); return; }
    f->n++;
}

MODFIT* tagpu_modfit_open(const char* base, int (*original)(const char* name))
{
    static const char* const PATTERN[] = { "rev31.gp3", "*.ccx", "*.ufo", "*.hpi" };
    MODFIT* f = (MODFIT*)calloc(1, sizeof *f);
    const int folder = !strcmp(base, "none");
    size_t ncat = 0;
    const char* cat = tagpu_modinst_catalog(&ncat);
    unsigned k;
    if (!f) return NULL;
    if (!folder) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "mods\\%s\\%s.hpi", base, base);
        add(f, path);
        snprintf(path, sizeof path, "mods\\%s\\%s.originals.hpi", base, base);
        add(f, path);
    }
    for (k = 0; k < sizeof PATTERN / sizeof *PATTERN; k++) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(PATTERN[k], &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!folder) {
                /* mount_filtered's rule: the player's key over the mod's list, and the archive every mod needs stays */
                const int key = lstrcmpiA(fd.cFileName, tagpu_modload_essential_archive()) ? tagpu_profiles_hide_of(base, fd.cFileName) : 0;
                if (key > 0 || (key < 0 && cat && tagpu_modcat_hidden(cat, ncat, base, fd.cFileName))) continue;
                if (!original(fd.cFileName)) continue;
            }
            add(f, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return f;
}

void tagpu_modfit_close(MODFIT* f)
{
    int i;
    if (!f) return;
    for (i = 0; i < f->n; i++) {
        tagpu_hpi_close(f->arc[i].hpi);
        CloseHandle(f->arc[i].file);
    }
    free(f->arc);
    free(f);
}

int tagpu_modfit_hash(void* ctx, const char* path, uint8_t out[32])
{
    MODFIT* f = (MODFIT*)ctx;
    int i;
    for (i = 0; i < f->n; i++) {
        TAGPU_HPI* a = f->arc[i].hpi;
        const int e = tagpu_hpi_find(a, path);
        uint32_t size, off;
        unsigned char* data;
        MODSHA sha;
        char why[160];
        if (e < 0 || tagpu_hpi_is_dir(a, e)) continue;
        size = tagpu_hpi_size(a, e);
        if (size > FILE_MAX || !(data = (unsigned char*)malloc(size ? size : 1))) return 0;
        for (off = 0; off < size; ) {
            const uint32_t k = size - off > TAGPU_HPI_READMAX ? TAGPU_HPI_READMAX : size - off;
            if (!tagpu_hpi_read(a, e, off, k, data + off, why, sizeof why)) { free(data); return 0; }
            off += k;
        }
        modsha_init(&sha);
        modsha_add(&sha, data, size);
        modsha_end(&sha, out);
        free(data);
        return 1;
    }
    return 0;
}
