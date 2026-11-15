#ifndef TAGPU_PROFILES_H
#define TAGPU_PROFILES_H
#include "tagpu_mod.h"

/* A mod's play options in force: the two resource options, and which wheel engages the mega map --
   `wheel` whether the player may choose it, `wheel_default` 1 for the plain wheel, which a mod that
   does not allow the choice holds the player to. */
typedef struct {
    int manual, quick, radius, maximum, quick_default;
    int wheel, wheel_default;
} TagpuResourcePolicy;

/* The player's changes to a mod's play options: the keys of profiles\<id>.ini. The wheel's is
   spelled mega_map_wheel=wheel|ctrl+wheel and held as 1|0. */
enum { TAGPU_CHOICE_RADIUS, TAGPU_CHOICE_QUICK, TAGPU_CHOICE_WHEEL, TAGPU_CHOICES };

/* Attach moves the old layout's files and keys into this one. All other calls belong to the game
   thread. A main-menu selection releases the previous match's fixed policy. */
void tagpu_profiles_attach(void);
void tagpu_profiles_select(int mod, const char* version);
void tagpu_profiles_menu_tick(void);
void tagpu_profiles_begin_match(void);
const TagpuResourcePolicy* tagpu_profiles_policy(void);
int tagpu_profiles_mod(void);
/* The file name of the mod selected: names[] below, and what a catalog id is. */
const char* tagpu_profiles_name(void);
int tagpu_profiles_radius(void);
int tagpu_profiles_quick(void);
void tagpu_profiles_set_radius(int value);
void tagpu_profiles_set_quick(int value);
/* 1 when the plain wheel engages the mega map and Ctrl+wheel zooms, 0 for the other way round. The
   one call here any thread may make: tagpu_zoom.c asks it on the message thread at every notch.
   Ctrl+wheel under tagpu_defaults.off, where neither the mod nor the player has a say. */
int  tagpu_profiles_wheel(void);
void tagpu_profiles_set_wheel(int value);
void tagpu_profiles_status(char* out, unsigned cap);
/* The player's raw value, -1 for "the mod's default", kept above a cap or under a lock; and a value
   put back, by the menu's Undo and Restore, -1 removing the key. */
int tagpu_profiles_choice(int which);
void tagpu_profiles_restore_choice(int which, int value);
/* Any mod's file, for the Mods screen at the main menu, where a write inside a click holds up no game.
   The player's value of a choice, or -1. */
int tagpu_profiles_choice_of(const char* id, int which);
/* A base mod's file holds which of its mutators are on ("none" the folder's own game): mutator.<id>, 1 on, 0 off, -1 no
   key, without case. A mutator is off with no key. Setting -1 removes the key: 1, or 0 with the reason in the log. */
int tagpu_profiles_mutator_of(const char* base, const char* mutator);
int tagpu_profiles_set_mutator_of(const char* base, const char* mutator, int value);

/* The file's write: the render thread each frame, the Mods screen's Confirm, and the exit once no other thread is inside. A
   click only records its value (tagpu_menu_present). */
void tagpu_profiles_flush(void);
/* The same past the wait after a failed write, which stops a retry every frame: a player's click may try at once. */
void tagpu_profiles_flush_now(void);
/* 1 when nothing waits to be written: the flush above held. */
int  tagpu_profiles_saved(void);
void tagpu_profiles_final(void);

#endif
