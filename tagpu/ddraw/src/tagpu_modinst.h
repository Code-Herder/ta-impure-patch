#ifndef TAGPU_MODINST_H
#define TAGPU_MODINST_H
/* tagpu_modinst -- downloading and installing a catalog mod on a background thread that touches only
   files: no engine memory, no render state, nothing but `mods\` and reads of the folder's own archives.

   Nothing unpacks from an unverified file: the download goes to mods\<id>.part, and its size and hash
   are checked against the catalog first. The package unpacks through the bounded tar reader into
   mods\<id>.new\, the recipe builds <id>.originals.hpi from the player's archives, and a marker
   written last says that folder is complete. It replaces the live folder at the next start, in
   tagpu_modinst_sweep, before any archive is mounted, so a running game never sees its content change.

   Design: research/notes/modules/mods.md, *Download and install*. */
#include <stddef.h>

typedef enum {
    MODINST_IDLE,
    MODINST_DOWNLOADING,
    MODINST_CHECKING,
    MODINST_UNPACKING,
    MODINST_RECIPE,
    MODINST_READY,              /* complete: it takes effect at the next start */
    MODINST_FAILED
} MODINST_PHASE;

typedef struct {
    MODINST_PHASE phase;
    unsigned      done, total;  /* the current phase's progress: its bytes, or the recipe's files; total 0 until it is known */
    char          why[200];     /* MODINST_FAILED's reason, for the player */
} MODINST_STATUS;

/* At process attach, before anything is mounted: finish what the last run left in the exe's mods\\ folder, for the catalog's
   ids alone. A folder with its marker replaces the live one, one without it and a .part are deleted, and a swap a crash
   interrupted is completed or undone, so the live folder is always the old version or the new. Does nothing without
   anything to settle, in an exe that is not the game, or while another process holds the install lock. */
void tagpu_modinst_sweep(const char* catalog, size_t n);
void tagpu_modinst_sweep_at_start(void);        /* with the DLL's own catalog */

/* 1 while another process holds the install lock. */
int  tagpu_modinst_locked(void);

/* Starts installing mod `id` from `catalog`. `base` is where a relative package URL resolves when it
   is not NULL, and otherwise the catalog's base_url. 1 when the thread is running; 0 with the reason in
   `why` when one is already running or the entry cannot be acted on. Call from one thread at a time. */
int  tagpu_modinst_start(const char* catalog, size_t n, const char* id, const char* base, char* why, size_t nwhy);

/* Where the install stands, without blocking. The thread that calls tagpu_modinst_start. */
void tagpu_modinst_status(MODINST_STATUS* out);

/* The version the folder mods\<id>\ holds into `out`, from its marker; 0 when it has none. */
int  tagpu_modinst_installed(const char* id, char* out, int cap);

/* The harness's way in until the Mods screen calls tagpu_modinst_start: in the game, the file tagpu_modinst.on naming
   "<id> <base url>" installs that mod at every start while it exists, from the DLL's own catalog. */
void tagpu_modinst_lever(void);

#endif
