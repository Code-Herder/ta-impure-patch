#ifndef TAGPU_SIMREG_H
#define TAGPU_SIMREG_H
/* tagpu_simreg -- the simulation-state registry (research/notes/tadr-port/demo-recorder-completeness.md).

   A source file marked `sim` in thread-split.allow declares every writable static, heap table
   and TLS slot it has, beside its definition, with one of the macros below. Each declaration is
   a SimDesc in the linker section `.rdata$simreg$m`, which the linker sorts between this
   module's `$a` and `$z` markers, so the recorder walks one list of all simulation state and no
   central file names any of it.

   The rule is enforced at build time by tools/sim-state-check.sh, on the objects being linked:
   a writable static of a `sim` object with no declaration, a declaration of something that is
   not a writable static, a `sim` object that imports a listed allocator, TLS, page or
   process-memory call or GetProcAddress, and a lever file named outside tagpu_sim_lever each
   fail the build by name; the script's header lists what those rules cannot see. So memory,
   TLS slots, our own page changes and levers come through here or through tagpu_detour.h (code
   memory and engine code writes). The walk at attach refuses two declarations of one byte.

   THE CLASSES. When unsure, SIM_STATE: a wrong STATE costs recording bytes, a wrong other class
   costs a replay that silently differs.
     SIM_STATE      the simulation reads it and it changes during a game: recorded, restored,
                    verified. Display state that follows from simulation events is STATE too.
     SIM_CONTENT    derived only from game data when it loads, constant until that data loads
                    again: rebuilt, not recorded, hashed to catch a mismatch.
     SIM_TRANSIENT  carries nothing from one tick to the next: at every tick boundary each byte
                    is back at its value from load (an empty queue, a free hold, a cleared flag)
                    or is written before anything reads it (a scratch buffer). The harness resets
                    it to its value from load at a snapshot, as a fresh process holds it, and the
                    round trip must still match.
     SIM_PRIVATE    diagnostics the simulation never reads: counters, log throttles, oracles.
                    A module keeps them in one struct per feature, so every use says so.
     SIM_INSTALL    written while Impure installs -- in DllMain, and in the start-up checks at
                    the first DirectDraw call -- and never during a game: trampolines, code
                    pages, site tables, TLS indices, levers. Hashed after install; a change
                    in a game fails the harness by name. Locks are INSTALL and left out of
                    that hash (SIM_LOCK).

   THE KEY says what a row index means, so a takeover can move rows with their units. A field
   whose VALUE is an address or an identity is named in the row's layout (SimField): a table
   stores IDs, or its layout says which bytes are not plain values. */

#include <stddef.h>

typedef enum SimClass { SIM_STATE, SIM_CONTENT, SIM_TRANSIENT, SIM_PRIVATE, SIM_INSTALL,
                        SIM_CLASSES } SimClass;

/* what a row index, or an identity field's value, names */
typedef enum SimId {
    SIM_ID_NONE,          /* one row, or rows with no identity             */
    SIM_ID_UNIT_SLOT,     /* the unit array slot                           */
    SIM_ID_UNIT_TYPE,     /* the unit type's def index                     */
    SIM_ID_PLAYER,        /* the player index                              */
    SIM_ID_WEAPON,        /* the weapon ID                                 */
    SIM_ID_DPLAY          /* a DirectPlay player ID: the session's, not the recording's */
} SimId;

typedef enum SimFieldKind {
    SIMF_ID,        /* a value naming `id`: remapped with it, never compared across a remap   */
    SIMF_ADDR,      /* the address of what `id` names (a def, a weapon record): the recording
                       holds the target's ID, one more than its row (tagpu_recfmt.h), where a
                       SIMF_ID holds the engine's index; SIM_ID_NONE = other engine or process
                       memory, never a value                                                 */
    SIMF_OWNED,     /* the address of a block this row owns (SIM_ALLOC_OWNED of its table)    */
    SIMF_LOCAL,     /* meaningful only in this process: a serial, a clock, a generation       */
    SIMF_COUNT,     /* how many elements each SIMF_COLUMN of this row holds                   */
    SIMF_COLUMN,    /* the address of an array of elements `step` bytes apart, one per `id`,
                       COUNT of them: a struct-of-arrays table                                */
    SIMF_TARGET,    /* the engine's weapon target, a u16 and the u16 after it: a unit slot when
                       the second is 0x8000, else a ground X and Z                            */
    SIMF_PINNED,    /* the engine holds this field's ADDRESS (a running script's record points
                       at it), so the row is written in place and never moved while it may   */
    SIMF_OPAQUE     /* a plain value whose bits are arbitrary (a random generator's state, a
                       hash): recorded and compared as any plain value, and passed over by
                       the address scan, which cannot tell such a word from an address       */
} SimFieldKind;

typedef struct SimField {
    unsigned short off;     /* in the row                                        */
    unsigned short size;    /* bytes; 0 ends the layout                          */
    unsigned short repeat;  /* the field occurs `repeat` times, `step` bytes     */
    unsigned short step;    /*   apart; 0 = once                                 */
    unsigned char  kind;    /* SimFieldKind                                      */
    unsigned char  id;      /* SimId                                             */
} SimField;

#define SIMD_HEAP   0x01    /* addr is the address of the pointer to the rows    */
#define SIMD_TLS    0x02    /* addr is a TLS index; the class is the slot's content */
#define SIMD_LOCK   0x04    /* a lock: INSTALL, left out of the install hash     */
#define SIMD_CODE   0x08    /* a pointer to code memory SIM_ALLOC makes executable */
#define SIMD_GUARD  0x10    /* a pointer to a reserved region no access may touch */
#define SIMD_ENGINE 0x20    /* an engine pool this DLL holds, in the engine's own record layout:
                               its bytes are described with the engine's records, not here   */

typedef struct SimDesc {
    const char*     name;   /* "src/tagpu_datakeys.c:s_unit": every failure names it   */
    unsigned        line;   /* the declaration's                                      */
    void*           addr;   /* the variable (for SIMD_HEAP, the pointer)              */
    unsigned        stride; /* bytes a row                                            */
    unsigned        nrows;  /* rows the table holds; 0 for a heap table that `rows` counts */
    const unsigned* rows;   /* how many of them are live, a prefix; NULL = all        */
    unsigned char   cls;    /* SimClass                                               */
    unsigned char   key;    /* SimId of a row index                                   */
    unsigned short  flags;  /* SIMD_*                                                 */
    const SimField* layout; /* the fields that are not plain values, or NULL          */
} SimDesc;

/* layout entries: a member of the row type; each element of an array member; a member of each
   of `n` records of `type` that make up a row; `size` bytes at `off`; the end. A layout is an
   array of these, ended by SIM_FIELD_END. */
#define SIM_FIELD(type, member, kind_, id_) \
    { offsetof(type, member), sizeof(((type*)0)->member), 0, 0, (kind_), (id_) }
#define SIM_FIELD_ARRAY(type, member, kind_, id_)                                              \
    { offsetof(type, member), sizeof(((type*)0)->member[0]),                                   \
      sizeof(((type*)0)->member) / sizeof(((type*)0)->member[0]), sizeof(((type*)0)->member[0]), \
      (kind_), (id_) }
#define SIM_FIELD_EACH(type, member, n, kind_, id_) \
    { offsetof(type, member), sizeof(((type*)0)->member), (n), sizeof(type), (kind_), (id_) }
#define SIM_FIELD_AT(off, size, kind_, id_) { (off), (size), 0, 0, (kind_), (id_) }
/* a pointer member that is a column of a struct-of-arrays table */
#define SIM_COLUMN(type, member, id_)                                                          \
    { offsetof(type, member), sizeof(((type*)0)->member), 0, sizeof(*((type*)0)->member),      \
      SIMF_COLUMN, (id_) }
#define SIM_FIELD_END { 0, 0, 0, 0, 0, 0 }

/* layouts of a row that is one value: an address of other memory, a process-local value, a
   block the row owns */
extern const SimField tagpu_sim_addr[];
extern const SimField tagpu_sim_local[];
extern const SimField tagpu_sim_owned[];

#define SIM__IS_ARRAY(v) (!__builtin_types_compatible_p(__typeof__(v), __typeof__(&(v)[0])))

#define SIM__ENTRY(var, ...)                                                                   \
    static const SimDesc simdir_##var = { .name = __FILE__ ":" #var, .line = __LINE__,         \
                                          .addr = (void*)&(var), __VA_ARGS__ };                \
    static const SimDesc* const simreg_##var                                                   \
        __attribute__((used, section(".rdata$simreg$m"))) = &simdir_##var

/* A scalar, a struct or a buffer: one row. */
#define SIM_VAR(var, cls_) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = (cls_))

/* A scalar or struct whose layout names fields that are not plain values. */
#define SIM_VAR_L(var, cls_, layout_) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = (cls_), .layout = (layout_))

/* A static array of rows; `key_` is what a row index means. */
#define SIM_ROWS(var, cls_, key_, layout_)                                                     \
    _Static_assert(SIM__IS_ARRAY(var), #var " is not an array");                               \
    SIM__ENTRY(var, .stride = sizeof((var)[0]), .nrows = sizeof(var) / sizeof((var)[0]),        \
               .cls = (cls_), .key = (key_), .layout = (layout_))

/* A static array whose first `count` rows are live. */
#define SIM_ROWS_LIVE(var, cls_, key_, count, layout_)                                         \
    _Static_assert(SIM__IS_ARRAY(var), #var " is not an array");                               \
    _Static_assert(__builtin_types_compatible_p(__typeof__(count), unsigned),                  \
                   #count " is not unsigned");                                                 \
    SIM__ENTRY(var, .stride = sizeof((var)[0]), .nrows = sizeof(var) / sizeof((var)[0]),        \
               .rows = &(count), .cls = (cls_), .key = (key_), .layout = (layout_))

/* A pointer to `count` rows of `stride_` bytes that SIM_ALLOC made. */
#define SIM_HEAP(var, cls_, key_, stride_, count, layout_)                                     \
    _Static_assert(!SIM__IS_ARRAY(var), #var " is not a pointer");                             \
    _Static_assert(__builtin_types_compatible_p(__typeof__(count), unsigned),                  \
                   #count " is not unsigned");                                                 \
    SIM__ENTRY(var, .stride = (stride_), .rows = &(count), .cls = (cls_), .key = (key_),        \
               .flags = SIMD_HEAP, .layout = (layout_))

/* A pointer to `n_` rows of `*var` that SIM_ALLOC made. */
#define SIM_BLOCK(var, cls_, key_, n_, layout_)                                                \
    _Static_assert(!SIM__IS_ARRAY(var), #var " is not a pointer");                             \
    SIM__ENTRY(var, .stride = sizeof(*(var)), .nrows = (n_), .cls = (cls_), .key = (key_),      \
               .flags = SIMD_HEAP, .layout = (layout_))

/* An engine pool this DLL holds (the raised limits move some out of the engine): one row, the
   whole variable, in the engine's record layout. */
#define SIM_ENGINE(var, cls_) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = (cls_), .flags = SIMD_ENGINE)

/* A TLS index (a DWORD that starts at TLS_OUT_OF_INDEXES); `cls_` is the slot's content. */
#define SIM_TLS(var, cls_) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = (cls_), .flags = SIMD_TLS)

/* A CRITICAL_SECTION or other lock. */
#define SIM_LOCK(var) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = SIM_INSTALL, .flags = SIMD_LOCK)

/* A pointer to executable memory SIM_ALLOC makes: stubs and trampolines an installer builds. */
#define SIM_CODE(var) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = SIM_INSTALL, .flags = SIMD_CODE)

/* A pointer to a reserved region SIM_ALLOC maps with no access, so that a use faults. */
#define SIM_GUARD(var) \
    SIM__ENTRY(var, .stride = sizeof(var), .nrows = 1, .cls = SIM_INSTALL, .flags = SIMD_GUARD)

/* Memory a declaration's variable points at: a heap table's rows (SIM_HEAP, SIM_BLOCK), a TLS
   slot's buffer (SIM_TLS), code (SIM_CODE), a guard (SIM_GUARD). Zeroed from VirtualAlloc, a heap
   table's pages write-watched (MEM_WRITE_WATCH: the demo recorder asks which were written, and
   images only those), code executable and a guard only reserved; NULL on failure, or for any
   other declaration (logged by name). */
void* tagpu_sim_alloc(const SimDesc* d, size_t bytes);
void  tagpu_sim_free(const SimDesc* d, void* p);
#define SIM_ALLOC(var, bytes) tagpu_sim_alloc(&simdir_##var, (bytes))
#define SIM_FREE(var, p)      tagpu_sim_free(&simdir_##var, (p))

/* A block one of a declaration's rows owns (its layout names an SIMF_OWNED field): zeroed from
   the process heap. A separate call, so that a heap table whose rows own blocks takes each from
   the right place; NULL on failure, or when the layout names no owned field (logged by name). */
void* tagpu_sim_alloc_owned(const SimDesc* d, size_t bytes);
void  tagpu_sim_free_owned(const SimDesc* d, void* p);
#define SIM_ALLOC_OWNED(var, bytes) tagpu_sim_alloc_owned(&simdir_##var, (bytes))
#define SIM_FREE_OWNED(var, p)      tagpu_sim_free_owned(&simdir_##var, (p))

/* Zeroed scratch that the call taking it frees before it returns: it holds nothing a tick
   boundary can see, so it needs no declaration. */
void* tagpu_sim_scratch(size_t bytes);
void  tagpu_sim_scratch_free(void* p);

/* Allocates a SIM_TLS index once; 1 when the index is valid. */
int tagpu_sim_tls(const SimDesc* d);
#define SIM_TLS_ALLOC(var) tagpu_sim_tls(&simdir_##var)

/* A lever: a file in the game's folder whose presence, or text, changes what the simulation
   runs, read while Impure installs. 1 when it exists. The registry keeps one entry per lever:
   whether it exists and, when its text is read, the text; its attach line counts the levers and
   names each one set, with the start of its text. A lever read after install, or read two ways
   while installing (present and absent, or two texts), is logged by name. */
int tagpu_sim_lever(const char* file);
/* A lever with a value: up to cap-1 bytes of it, NUL-ended; the count, or -1 when absent. */
int tagpu_sim_lever_read(const char* file, char* buf, int cap);
/* A file that tooling reads or writes (a trigger, a dump), never a lever: the lever rule of
   tools/sim-state-check.sh accepts a `tagpu_` file name only inside one of these three. */
#define SIM_TOOL_FILE(name) name

/* Walks the registry once every simulation module has installed (DllMain): checks each
   declaration's shape, closes the lever list and logs one summary line. */
void tagpu_simreg_attach(void);

/* The recorder's view, valid once the attach walk has run (0 and NULL before it): the
   declarations it accepted, in the list's order -- the index is a recording's name for a table,
   its header maps each index to the declaration's name -- and the levers read while installing,
   each with its file name, whether it was present and the start of its text (NULL when only its
   presence was read). */
unsigned       tagpu_simreg_count(void);
const SimDesc* tagpu_simreg_at(unsigned i);
unsigned       tagpu_simreg_levers(void);
const char*    tagpu_simreg_lever_at(unsigned i, int* set, const char** text, int* len);

/* A declaration's layout as one number, for a reader to tell whether a recorded table's fields
   are the ones its own declaration of the same name describes (tagpu_recfmt.h, RT_TABLE): the
   CRC-32 (crc32.c) of its form -- u8 2 when a count variable says how many rows are live
   (SIM_ROWS_LIVE, SIM_HEAP), else 1; u8 0; u16 its flags -- then of each layout field to the end
   marker, as u16 off, size, repeat, step, u8 kind, u8 id, little-endian. 0 for a declaration with
   no layout, whose rows are plain values. */
unsigned       tagpu_simreg_layout_hash(const SimDesc* d);

/* 1 for a declaration a recording carries and a seek writes back: SIM_STATE, less the engine pools
   the raised limits moved into the DLL (their record layouts are not yet written anywhere), TLS
   slots (per thread) and the INSTALL-only kinds (a lock, code, a guard). The recorder and the
   replay read this one test, so a table the recorder never writes can never be paired, whatever a
   recording's header names. */
int            tagpu_simreg_recorded(const SimDesc* d);

/* The static SIM_TRANSIENT declarations (not SIMD_TLS, SIMD_HEAP or SIMD_ENGINE), stride x nrows
   bytes each, saved at a tick boundary and put back for the replay's round trip, on the game
   thread. The reset returns the bytes that differed and names up to 8 declarations in `names`
   ("<name>: <k> bytes"): at a tick boundary every byte of one is at its value from load or written
   before it is read (SIM_TRANSIENT above), so the reset changes nothing the simulation reads unless
   a declaration is misclassified, which the round trip's continuation dump names. */
void           tagpu_simreg_transient_save(void);
unsigned       tagpu_simreg_transient_reset(char* names, unsigned cap);

#endif
