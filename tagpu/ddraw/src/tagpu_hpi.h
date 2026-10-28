#ifndef TAGPU_HPI_H
#define TAGPU_HPI_H
/* tagpu_hpi -- a reader of TA's HAPI archives (.hpi, .ufo, .ccx, .gp3), read-only, for the in-game
   wiki's 3D asset loader (tagpu_wikiasset.c). The Python twin is tools/ta3do's HpiArchive.

   THE FORMAT, little-endian. A 20-byte header: "HAPI", a version ("BANK" marks a saved game, which
   is not an asset archive), the directory's end, a header key and the directory's start. Every
   byte past the header is obfuscated when the key is not 0: with k = ~(key * 4 | key >> 6) & 0xFF,
   the byte b stored at file offset p reads (p ^ k ^ ~b) & 0xFF. The directory occupies
   [start, end) and every offset in it is a file offset. A directory is {u32 count, u32 list}; the
   list holds `count` 9-byte entries {u32 name, u32 data, u8 flag}, flag bit 0 a subdirectory
   (whose `data` is its own {count, list}) and otherwise a file, whose `data` is {u32 offset,
   u32 size, u8 compression}. A compressed file is ceil(size / 64 KiB) chunks behind a table of
   their lengths; each chunk is "SQSH", a version, a method (1 LZ77, 2 zlib), an encrypted flag, the
   compressed and decompressed sizes and a checksum, then the body.

   EVERY OFFSET IN AN ARCHIVE IS DATA UNTIL IT IS BOUNDED AGAINST THAT ARCHIVE. tagpu_hpi_open
   reads the header and the whole directory once and refuses the archive, naming why, unless the
   directory lies inside the file, every list and name lies inside the directory, a name is
   terminated within TAGPU_HPI_NAMEMAX bytes, the tree is at most TAGPU_HPI_DEPTH deep, and the walk
   visits no entry twice (which takes in a cycle and a list shared by two directories). After the
   open every entry index the accessors take is checked against the entry count. A file's data
   range is a FILE's property, checked when the file is read: a bad one fails that read, named, and
   leaves the archive and its precedence as the engine has them. A read checks its range against
   the file's size, each chunk against the archive and against TAGPU_HPI_CHUNKMAX, and each chunk's
   output against the size its own header and the file's size give it, after LZ77 or zlib.

   No engine memory, no Win32 and no state of its own: an archive is a handle its caller owns, read
   on one thread. tests/wikiassettest.c compiles this file natively. */
#include <stddef.h>
#include <stdint.h>

#define TAGPU_HPI_DEPTH     16              /* directory levels below the root                    */
#define TAGPU_HPI_NAMEMAX   256             /* a name with its terminator                         */
#define TAGPU_HPI_CHUNK     0x10000u        /* a compressed file's chunk, decompressed             */
#define TAGPU_HPI_CHUNKMAX  0x20000u        /* a chunk's stored length: LZ77's worst case on 64 KiB
                                               is 9/8 of it plus the 19-byte header               */
/* The most bytes one read returns. It bounds the range asked for, not the file: a read allocates
   only the file's chunk table (4 bytes per 64 KiB, 256 KiB for the largest uint32 size) and one
   chunk, so a file of any size may be read in ranges. Escalation's fx.GAF is 82 MB (measured
   2026-10-27), and the wiki reads its GAFs in pages. */
#define TAGPU_HPI_READMAX   (64u << 20)

/* Where an archive's bytes come from: a file handle in the DLL and the native test, memory in the
   fuzz pass. `read` answers 1 when all `n` bytes at `off` were read. */
typedef struct {
    void*    ctx;
    uint32_t size;
    int    (*read)(void* ctx, uint32_t off, void* dst, uint32_t n);
} TAGPU_HPISRC;

typedef struct TAGPU_HPI TAGPU_HPI;

/* The archive behind `src`, its header and directory read and checked; NULL with the reason in
   `why` when it is not one this reader takes. `src` is copied; its ctx must outlive the handle. */
TAGPU_HPI* tagpu_hpi_open(const TAGPU_HPISRC* src, char* why, size_t nwhy);
void       tagpu_hpi_close(TAGPU_HPI* a);

/* THE DIRECTORY, as entry indices: 0 is the root, a directory with no name. */
int         tagpu_hpi_count(const TAGPU_HPI* a);                  /* entries, the root included  */
int         tagpu_hpi_children(const TAGPU_HPI* a, int dir);      /* 0 for a file or a bad index */
int         tagpu_hpi_child(const TAGPU_HPI* a, int dir, int i);  /* in the list's order, or -1  */
const char* tagpu_hpi_name(const TAGPU_HPI* a, int e);            /* "" for a bad index          */
int         tagpu_hpi_is_dir(const TAGPU_HPI* a, int e);
uint32_t    tagpu_hpi_size(const TAGPU_HPI* a, int e);            /* a file's, decompressed      */

/* The entry `name` names in directory `dir`, compared as the engine's _stricmp compares (A-Z
   folded, nothing else), the list scanned from its LAST entry to its first as 0x4BB4E0 scans it,
   so of two equal names the later wins; -1 when none. */
int tagpu_hpi_lookup(const TAGPU_HPI* a, int dir, const char* name, size_t len);

/* The entry at `path`, split at '\' only as 0x4BB4E0 splits it, each component looked up as
   above and every component but the last a directory; -1 when none. */
int tagpu_hpi_find(const TAGPU_HPI* a, const char* path);

/* THE SHADOW MARK the engine's mount keeps in each entry's flag bit 1 (0x4BE320): an entry the
   enumerations skip because an earlier source holds its path. The caller computes it
   (tagpu_vfs.c); it lives here because it is a fact about one entry. */
int  tagpu_hpi_shadowed(const TAGPU_HPI* a, int e);
void tagpu_hpi_shadow(TAGPU_HPI* a, int e);

/* `n` bytes of file `e` from its byte `off`, decompressed, into `dst`; 0 with the reason in `why`
   when the range is past the file, `n` is past TAGPU_HPI_READMAX, or any chunk the range
   touches is not a chunk this reader takes. */
int tagpu_hpi_read(const TAGPU_HPI* a, int e, uint32_t off, uint32_t n, void* dst, char* why, size_t nwhy);

/* The engine's _stricmp (0x4F8A70 while no locale is set, which nothing in the exe sets): A-Z fold
   to a-z and the bytes compare unsigned. `n` bounds `b`, which need not be terminated. */
int tagpu_hpi_stricmp(const char* a, const char* b, size_t n);

#endif
