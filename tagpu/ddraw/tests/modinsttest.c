/* modinsttest -- the mod installer run on its own, under Wine, by tools/modpack/installtest.py: the real module with its real
   WinHTTP and its real file handling, without a game around it. MODINST_STOP="<step>:<n>" ends the process at that step, as a
   crash or a killed game would, with nothing flushed and no handler run.

     modinsttest install CATALOG ID BASE      one install to its end: prints its result, exit 0 when ready
     modinsttest sweep CATALOG                what a start does before it mounts anything
     modinsttest hold PATH                    holds a file open with no sharing until it is killed
     modinsttest load-dll                     loads ddraw.dll from beside this exe, as a tool would: what its attach touches
     modinsttest installed ID                 the version the live folder's marker names
     modinsttest ufo OUT PATH...              an archive of files named as they are on the disk, for the writer's byte test */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_log.h"
#include "tagpu_modinst.h"
#include "tagpu_ufo.h"

void tagpu_log(const char* m) { printf("log: %s\n", m); fflush(stdout); }

void tagpu_logf(const char* fmt, ...)
{
    va_list ap;
    printf("log: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

void modinst_test_step(const char* step, unsigned n);
void modinst_test_step(const char* step, unsigned n)
{
    char want[64];
    size_t len = strlen(step);
    DWORD got = GetEnvironmentVariableA("MODINST_STOP", want, sizeof want);
    if (!got || got >= sizeof want || strncmp(want, step, len) || want[len] != ':') return;
    if (n >= (unsigned)strtoul(want + len + 1, NULL, 10)) TerminateProcess(GetCurrentProcess(), 9);
}

static char* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    char* b;
    long size;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char*)malloc((size_t)size + 1);
    if (b && fread(b, 1, (size_t)size, f) != (size_t)size) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)size;
    return b;
}

int main(int argc, char** argv)
{
    if (argc >= 5 && !strcmp(argv[1], "install")) {
        size_t n = 0;
        char* cat = read_file(argv[2], &n), why[200];
        MODINST_STATUS st;
        if (!cat) { printf("no catalog at %s\n", argv[2]); return 2; }
        if (!tagpu_modinst_start(cat, n, argv[3], argv[4][0] ? argv[4] : NULL, why, sizeof why)) {
            printf("refused: %s\n", why);
            return 1;
        }
        do {
            Sleep(5);
            tagpu_modinst_status(&st);
        } while (st.phase != MODINST_READY && st.phase != MODINST_FAILED);
        if (st.phase == MODINST_FAILED) { printf("failed: %s\n", st.why); return 1; }
        printf("ready\n");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "sweep")) {
        size_t n = 0;
        char* cat = read_file(argv[2], &n);
        if (!cat) { printf("no catalog at %s\n", argv[2]); return 2; }
        tagpu_modinst_sweep(cat, n);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "hold")) {
        HANDLE f = CreateFileA(argv[2], GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        printf(f == INVALID_HANDLE_VALUE ? "failed\n" : "held\n");
        fflush(stdout);
        Sleep(INFINITE);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "load-dll")) {
        HMODULE m = LoadLibraryA("ddraw.dll");
        printf(m ? "loaded\n" : "not loaded\n");
        fflush(stdout);
        ExitProcess(0);
    }
    if (argc >= 3 && !strcmp(argv[1], "installed")) {
        char v[64];
        if (!tagpu_modinst_installed(argv[2], v, sizeof v)) { printf("none\n"); return 1; }
        printf("%s\n", v);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "ufo")) {
        int nf = argc - 3, i;
        TAGPU_UFO_FILE* f = (TAGPU_UFO_FILE*)calloc((size_t)(nf ? nf : 1), sizeof *f);
        for (i = 0; i < nf; i++) {
            size_t size = 0;
            f[i].path = argv[3 + i];
            f[i].data = read_file(argv[3 + i], &size);
            f[i].size = (unsigned)size;
            if (!f[i].data) { printf("no file %s\n", argv[3 + i]); return 2; }
        }
        return tagpu_ufo_write(argv[2], f, nf) ? 0 : 1;
    }
    printf("usage: modinsttest install|sweep|installed|ufo\n");
    return 2;
}
