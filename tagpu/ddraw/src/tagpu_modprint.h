#ifndef TAGPU_MODPRINT_H
#define TAGPU_MODPRINT_H
/* tagpu_modprint -- what this process plays with, as two hashes other Impure players can compare (research/notes/modules/mods.md,
   *The fingerprint*).

   ONE PRINT SERVES THE PROCESS. It is taken once InitTAHPIAry mounts nothing more, when a level's own call would mount nothing either, and a mutator
   turned on or off takes a restart. The exe's half names the image the mod's table makes, so a managed player and a drop-in player of one mod agree. The content's
   half walks the files under every folder the mounted archives have, once each from the source it opens from, after the mount's removals: that is how a mutator
   and what it hides reach it without being looked at.

   Files only: no engine memory on its thread. */
#include <stddef.h>
#include <stdint.h>

#define MODPRINT_HASH  16
#define MODPRINT_LABEL 120
#define MODPRINT_TAIL  (8 + 2 * MODPRINT_HASH + MODPRINT_LABEL)

typedef struct {
    uint8_t exe[MODPRINT_HASH], content[MODPRINT_HASH];
    char    label[MODPRINT_LABEL];      /* "ProTA 4.8 + Tall commander 1.0": printable ASCII, terminated */
} MODPRINT;

/* Whether a file of the game counts, its path with '\' separators in any case. Left out: what cannot change a simulation (.gaf, .pcx, .bmp, .wav,
   .fnt, and a .gui no unit's build menu names); maps\, which stock compares itself (0x4373A0 against PlayerInfo +0xA9, 0x4481BE); and camps\, as a
   network game runs no campaign, whose loose examples a retail folder holds and a managed mod's closed loose step does not see. `is_unit` answers whether a name, no folder and no extension, is a unit's FBI. An unknown type counts. */
int  tagpu_modprint_counts(const char* path, int (*is_unit)(void* ctx, const char* name, size_t len), void* ctx);

/* The print as the player data carries it, and back. A value in a peer's data is DATA: 0 for anything that is not a print, and the label comes
   back terminated and printable whatever the bytes were. */
void tagpu_modprint_tail_put(const MODPRINT* p, unsigned char out[MODPRINT_TAIL]);
int  tagpu_modprint_tail_read(const unsigned char* in, size_t n, MODPRINT* out);

/* GAME THREAD, in the shell: takes the mounts to the point a level finds them (tagpu_replays_mount_all), then starts the walk on its own thread, once. */
void tagpu_modprint_start(void);
/* Whether anything has asked for the print yet: before that, this process has not been near a network game. */
int  tagpu_modprint_started(void);
/* GAME THREAD: the print, waiting for the walk and starting it first if nothing has. An ordering: nothing here times out. NULL only if memory ran
   out, and then this player sends none and is compared with nobody. */
const MODPRINT* tagpu_modprint_wait(void);

#endif
