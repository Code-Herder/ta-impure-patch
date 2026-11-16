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

/* The controlled player's mutual allies, humans and AIs, in a skirmish or a network game, in
   record order: at most `cap` into `out` (NULL counts them), and how many there are. Nothing for a
   watcher or a playback's viewer. A local AI's economy is its own record's, rates and share levels
   included; a remote ally's is the contribution channel's while it runs (tagpu_rec_mp_economy,
   `rates`), else its record's: the stored values and storages its 0x28 last reported -- an AI's
   from its host (tagpu_patches.c, ai_economy_send). Game thread. */
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
   1, or 0 for a watcher, a playback's viewer, or with no level in play. Game thread. */
int tagpu_share_stored(float out[2]);

/* Asks a give of `metal` and `energy` to the player of record `index`, run at the next draw
   (tagpu_sharetick_draw) in the same level under the give's bounds; a later ask replaces one not
   yet run. 0, and nothing asked, while that draw is not installed. Game thread. */
int tagpu_share_give(unsigned index, float metal, float energy);

/* The drawing board's audience (tagpu_board.c), for the controlled player of a level in play: a
   bit per record whose marks they see and whose marks their operations may change -- their own,
   their mutual allies' (tagpu_share_allies' rule, AIs included) and, for a watcher, every
   record's; a watcher's marks are seen by watchers alone. 0 with no controlled player. Game
   thread. */
unsigned tagpu_share_board_seen(void);
/* the same audience for record `c` (below the ten), whoever controls the level: a replay's Sight
   seat (tagpu_board.c). 0 for an inactive record. Game thread. */
unsigned tagpu_share_board_seen_by(unsigned c);

/* Record `index`'s name, NUL-terminated, into out[0..cap): 1, or 0 for no level in play or a
   record past the ten. Game thread. */
int tagpu_share_name(unsigned index, char* out, unsigned cap);

/* 1 while a press the tick took is held. */
int tagpu_sharetick_dragging(void);

/* The window lost the mouse: a drag ends with the level it has. */
void tagpu_sharetick_cancel(void);

#endif
