#ifndef TAGPU_MODTAR_H
#define TAGPU_MODTAR_H
/* tagpu_modtar -- the reader of a mod's package tar, bounded to the file list the catalog declares
   for that package. A package is data from a server until this reader has accepted all of it: a
   member is written only when its path is safe and declared and its size is the declared one, and
   the whole is accepted only when each declared file has come exactly once with its declared hash and
   the archive's own end has been read.

   It takes the plain ustar that tools/modpack/modpack.py write_tar produces, regular files and
   nothing else: no links, no directories, no extended headers and no name in the prefix field, so a
   truncated header, a checksum that does not add up or a size that is not octal is a refusal, not a guess.

   Pure C99 and the C library: tests/modinsttest.py compiles it natively under the sanitizers. */
#include <stddef.h>
#include <stdint.h>
#include "tagpu_modpak.h"

typedef struct {
    const char* path;
    uint32_t    size;
    uint8_t     sha256[32];
} MODTAR_FILE;

/* Where the accepted bytes go. Each answers 1, or 0 with the reason the writer left in `why`. */
typedef struct {
    int  (*begin)(void* ctx, const char* path, uint32_t size);
    int  (*data)(void* ctx, const void* p, size_t n);
    int  (*end)(void* ctx);
    void* ctx;
} MODTAR_SINK;

typedef struct {
    MODTAR_SINK        sink;
    const MODTAR_FILE* files;
    int                nfiles;
    unsigned char*     seen;            /* the caller's, nfiles bytes */
    unsigned char      header[512];
    uint32_t           have;            /* bytes of `header` so far */
    uint32_t           left;            /* of the member being read */
    uint32_t           pad;             /* zeros to the block after it */
    int                current;
    int                zero_blocks;     /* the end is two of them; nothing but zeros may follow */
    int                in_member;
    MODSHA             sha;
    char               why[200];
} MODTAR;

/* 1 when a path can only name a file under the folder it is joined to: relative, '/' its only
   separator, no empty, "." or ".." component, no colon, no control character, and no component
   ending in a dot or a space, which Windows would drop. */
int  modtar_path_safe(const char* path);

void modtar_init(MODTAR* t, const MODTAR_SINK* sink, const MODTAR_FILE* files, int nfiles, unsigned char* seen);

/* The next `n` bytes, in any pieces. 0 with the reason in t->why, and nothing further is accepted. */
int  modtar_feed(MODTAR* t, const void* p, size_t n);

/* 1 when the archive ended where it says it does and every declared file came; 0 with t->why. */
int  modtar_finish(MODTAR* t);

#endif
