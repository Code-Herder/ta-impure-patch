/* modpakcheck.c -- tagpu_modpak.c's answers on real files, for modpack.py verify to compare with its
   own: `modpakcheck PACK EXE` prints the exe's file key, then each mod's image hash from it through
   its known XOR. */
#include <stdio.h>
#include <stdlib.h>
#include "tagpu_modpak.h"

static uint8_t* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    uint8_t* b;
    if (!f) exit(2);
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n + 1);
    if (!b || fread(b, 1, *n, f) != *n) exit(2);
    fclose(f);
    return b;
}

static void hex(const uint8_t* h)
{
    int i;
    for (i = 0; i < 32; i++) printf("%02x", h[i]);
}

int main(int argc, char** argv)
{
    size_t np, ne;
    uint8_t* pack, *exe, key[32], got[32];
    MODPAK p;
    const MODKNOWN* k;
    uint32_t i;
    if (argc != 3) return 2;
    pack = slurp(argv[1], &np);
    exe = slurp(argv[2], &ne);
    if (!tagpu_modpak_read(pack, np, &p) || !tagpu_modpak_file_key(exe, ne, 1, key)) { puts("refused"); return 1; }
    hex(key);
    puts("");
    k = tagpu_modpak_known(&p, 0, key);
    for (i = 0; i < p.nmods; i++) {
        MODIMG img;
        int ok = tagpu_modimg_from_exe(&img, exe, ne);
        if (ok && k) ok = tagpu_modimg_xor(&img, &p.xors[k->xor_]);
        if (ok) ok = tagpu_modimg_xor(&img, &p.xors[p.mods[i].xor_]);
        if (!ok) { printf("%s refused\n", p.mods[i].id); continue; }
        tagpu_modimg_hash(&img, p.mask, p.nmask, got);
        printf("%s ", p.mods[i].id);
        hex(got);
        puts("");
        tagpu_modimg_free(&img);
    }
    return 0;
}
