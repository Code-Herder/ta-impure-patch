#ifndef TAGPU_SHARETICK_H
#define TAGPU_SHARETICK_H
/* The player's share levels, set on the game's own resource bar
   (research/notes/modules/share.md). Everything here runs on the GAME thread: the draw from the
   post-GUI observer tagpu_menu.c owns, the rest from the window procedure (tagpu_shield.c). */

/* Draws a tick on each resource bar, and the levels under the bar while the pointer is on it;
   follows a drag. Inside DrawGameScreen, after its GUI draw and before the flip. */
void tagpu_sharetick_draw(void);

/* 1 when (gx, gy), in the engine's own coordinates, is on a bar's handle strip. */
int tagpu_sharetick_owns(int gx, int gy);

/* A left press or release at (gx, gy). 1 when it is ours: a press on a strip, and the release
   that ends that press. */
int tagpu_sharetick_click(int gx, int gy, int down);

/* The controlled player's mutual allies in a network game, in record order: at most `cap` into
   `out` (NULL counts them), and how many there are. Nothing outside a network game or for a
   watcher. An ally's economy is the contribution channel's while it runs (tagpu_rec_mp_economy,
   `rates`), else its record's: the stored values and storages its 0x28 last reported. Game
   thread. */
typedef struct {
    unsigned index;             /* the player record's, < 10                              */
    unsigned colour;            /* a palette index: tagpu_replay_logo_colour of its logo  */
    char     name[32];          /* the engine's, NUL-terminated                           */
    float    metal, metal_max, energy, energy_max;     /* stored, storage                  */
    unsigned units;             /* alive                                                  */
    int      out;               /* out of the game: none alive, one ever created, as the
                                   transfer's send test has it                            */
    int      rates;             /* the contribution channel's economy: the stored values and
                                   storages above are its too, and these are set          */
    float    metal_in, metal_out, energy_in, energy_out;
    float    share[2];          /* metal, energy share level; -1 while not sharing        */
} TAGPU_SHARE_ALLY;
int tagpu_share_allies(TAGPU_SHARE_ALLY* out, int cap);

/* The controlled player's stored metal and energy, 0 for a value that is not finite or negative:
   1, or 0 outside a network game, for a watcher, or with no level in play. Game thread. */
int tagpu_share_stored(float out[2]);

/* Asks a give of `metal` and `energy` to the player of record `index`, run at the next draw
   (tagpu_sharetick_draw) in the same level under the give's bounds; a later ask replaces one not
   yet run. 0, and nothing asked, while that draw is not installed. Game thread. */
int tagpu_share_give(unsigned index, float metal, float energy);

/* 1 while a press the tick took is held. */
int tagpu_sharetick_dragging(void);

/* The window lost the mouse: a drag ends with the level it has. */
void tagpu_sharetick_cancel(void);

#endif
