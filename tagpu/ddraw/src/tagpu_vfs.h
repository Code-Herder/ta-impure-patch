#ifndef TAGPU_VFS_H
#define TAGPU_VFS_H
/* tagpu_vfs -- the game's files as the running engine finds them, for the in-game wiki's 3D asset
   loader (tagpu_wikiasset.c). Its Python twin is tools/wiki/tavfs.py; the rules are the engine's,
   DISASSEMBLED in research/notes/exe-reverse-engineering.md, *Where the engine looks for archives*,
   and in the 3D stage's plan (ingame-wiki-3d-stage-plan.md, section 5):

   - THE SOURCES are the loose files under the process's current directory, then the engine's own
     mount list in mount order: each archive's full path as InitTAHPIAry 0x41D4C0 mounted it, copied
     on the game thread (tagpu_replays_archive_paths). Nothing here names a mod, a pattern or an
     archive, so whatever a mod mounts first wins here as it wins in the game.
   - A PATH (0x4BB2E0) opens loose first, as the engine's fopen 0x4E4990 takes it relative to the
     current directory, then from the first archive whose directory holds it as a file, the path
     split at '\' and each component compared as _stricmp compares (tagpu_hpi_find).
   - A BUILT PATH (0x4290F0, and 0x435430 for maps) is "<dir>-<language>\<name>" first and
     "<dir>\<name>" when that opens nothing, each cut at its last '.' and given "." + ext.
   - AN ENUMERATION lists the loose files first (FindFirstFile's order, which the engine's
     _findfirst is), then each archive in mount order, each archive's directory in its list order
     (0x4BC640), and skips an archive's entry that the mount marked shadowed: one whose path an
     earlier source holds as a file, the earlier being the loose files or an archive before it
     (0x4BE320 / 0x4BE400, bit 1 of the entry's flag). So each path is listed once, from the source
     a path open takes it from. The recursive form (0x4BCB50) walks "<dir>\*", takes the files its
     filter matches and descends into each subdirectory where it meets it, within that one source.
     The shadow test is made when an entry is listed rather than at the mount; it gives the mount's
     answer for every file present then.
   - NAMES MATCH a pattern as 0x4BC370 matches: `*` any run, `?` any one character, letters
     compared upper-cased.

   THE HANDLES ARE READ-ONLY and shared for read and write, so the engine's own handles on the same
   archives are never refused; this module's are its caller's, on one thread. An archive this
   reader refuses (tagpu_hpi.h's bounds) is no source: the refusal is kept, named, for the report.
   No engine memory: the list and the directory arrive by value. */
#include <stddef.h>
#include <stdint.h>

#define TAGPU_VFS_PATHMAX 0x104         /* the engine's own path buffers (OpenHAPIFile 0x4BDD70) */
#define TAGPU_VFS_ARCMAX  256           /* archives taken from the mount list                    */

typedef struct TAGPU_VFS TAGPU_VFS;

/* A file found: where it opens from and how big it is. */
typedef struct {
    int      src;                       /* -1 a loose file, else the archive's index             */
    int      entry;                     /* the archive's entry (tagpu_hpi.h)                     */
    uint32_t size;
    char     path[TAGPU_VFS_PATHMAX];   /* the path as found, with '\' separators                */
} TAGPU_VFILE;

/* The sources: `cwd` the loose files' root, `arc` the mount list's full paths in mount order (a
   relative one is taken under `cwd`), `language` the engine's language value. NULL only when
   memory runs out; an archive that does not open is kept as a refusal. */
TAGPU_VFS*  tagpu_vfs_open(const char* cwd, const char (*arc)[TAGPU_VFS_PATHMAX], int narc, const char* language);
void        tagpu_vfs_close(TAGPU_VFS* v);

int         tagpu_vfs_narc(const TAGPU_VFS* v);
const char* tagpu_vfs_arc_name(const TAGPU_VFS* v, int i);      /* its file name, no folder     */
const char* tagpu_vfs_arc_refused(const TAGPU_VFS* v, int i);   /* why it is no source, or NULL */
const char* tagpu_vfs_source(const TAGPU_VFS* v, int src);      /* "loose" or the file name     */

/* `path` by the engine's precedence into `out`; 0 when no source holds it. */
int tagpu_vfs_find(TAGPU_VFS* v, const char* path, TAGPU_VFILE* out);

/* 0x4290F0's path for `dir`, `name` and `ext`, the language's folder first; 0 when neither opens. */
int tagpu_vfs_find_built(TAGPU_VFS* v, const char* dir, const char* name, const char* ext, TAGPU_VFILE* out);

/* `n` bytes of `f` from `off` into `dst`; 0 with the reason in `why`. */
int tagpu_vfs_read(TAGPU_VFS* v, const TAGPU_VFILE* f, uint32_t off, uint32_t n, void* dst, char* why, size_t nwhy);

/* The whole of `f`, malloc'd and NUL-terminated one past its size, which must be at most `max`;
   NULL with the reason in `why`. */
unsigned char* tagpu_vfs_load(TAGPU_VFS* v, const TAGPU_VFILE* f, uint32_t max, char* why, size_t nwhy);

/* Each file of the enumeration, in its order: the files of `dir` that `pattern` matches
   (0x4BCA30), or with `recursive` every file under `dir` it matches, subdirectories walked in
   place (0x4BCB50). `each` answers 0 to stop. */
typedef int (*TAGPU_VFSEACH)(void* ctx, const TAGPU_VFILE* f);
void tagpu_vfs_enum(TAGPU_VFS* v, const char* dir, const char* pattern, int recursive, TAGPU_VFSEACH each, void* ctx);

/* 0x4BC370's match of `name` against `pattern`. */
int tagpu_vfs_match(const char* name, const char* pattern);

#endif
