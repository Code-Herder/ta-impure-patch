#ifndef TAGPU_MODPAK_H
#define TAGPU_MODPAK_H
/* tagpu_modpak -- the exe patch pack (mods/patches.bin, format in tools/modpack/modpack.py) and what
   it is checked against: SHA-256, a file's key, an image's hash. Every offset of the pack and the exe
   is checked against its length, so a damaged one is refused rather than read past.

   Pure C99 and the C library: tools/modpack/modpack.py verify compiles it natively and compares its
   answers with its own on every known exe. */
#include <stddef.h>
#include <stdint.h>

#define MODPAK_MAXSEC 8

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    uint32_t nbuf;
} MODSHA;

void modsha_init(MODSHA* s);
void modsha_add(MODSHA* s, const void* data, size_t n);
void modsha_end(MODSHA* s, uint8_t out[32]);

typedef struct { const uint8_t* at; uint32_t nruns; } MODXOR;    /* its runs: u32 va | u16 len | bytes */
typedef struct { char id[33]; const uint8_t* hash; uint32_t xor_; uint8_t loader; } MODPATCH;
typedef struct { uint8_t kind; const uint8_t* key; int32_t xor_; } MODKNOWN;     /* kind 0 exe, 1 loader */
typedef struct { uint32_t va, len; } MODRANGE;

typedef struct {
    const uint8_t* stock_key;
    MODXOR*   xors;   uint32_t nxors;
    MODPATCH* mods;   uint32_t nmods;
    MODKNOWN* known;  uint32_t nknown;
    MODRANGE* mask;   uint32_t nmask;   /* sorted, disjoint: what no image hash covers */
    const uint8_t* originals; uint32_t noriginals;   /* 32 B each: the original archives' SHA-256 */
} MODPAK;

/* The pack in `blob`, which must outlive it. 1, or 0 with `out` holding nothing to free. */
int  tagpu_modpak_read(const uint8_t* blob, size_t n, MODPAK* out);
void tagpu_modpak_free(MODPAK* p);
const MODPATCH* tagpu_modpak_mod(const MODPAK* p, const char* id);
const MODKNOWN* tagpu_modpak_known(const MODPAK* p, int kind, const uint8_t key[32]);
int  tagpu_modpak_is_original(const MODPAK* p, const uint8_t sha[32]);

/* A known file's identity. An exe's ignores the PE checksum and Characteristics, which the 4 GB copy
   changes; 0 for an exe too short to have them. */
int  tagpu_modpak_file_key(const uint8_t* data, size_t n, int is_exe, uint8_t out[32]);

/* An image: the exe's sections in VA order, .rsrc left out, each a copy of its raw bytes. */
typedef struct { uint32_t va, len; uint8_t* bytes; } MODSEC;
typedef struct { MODSEC sec[MODPAK_MAXSEC]; int nsec; } MODIMG;

/* From the file's bytes; 0 when its headers do not fit it. */
int  tagpu_modimg_from_exe(MODIMG* img, const uint8_t* exe, size_t n);
void tagpu_modimg_free(MODIMG* img);
/* The XOR on the image; 0 when a run lies outside every section, the image then unusable. */
int  tagpu_modimg_xor(MODIMG* img, const MODXOR* x);
/* SHA-256 of the sections, the mask's bytes as zero. The mask is sorted, as the pack's is. */
void tagpu_modimg_hash(const MODIMG* img, const MODRANGE* mask, uint32_t nmask, uint8_t out[32]);

#endif
