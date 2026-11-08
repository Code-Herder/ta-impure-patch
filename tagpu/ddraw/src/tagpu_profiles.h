#ifndef TAGPU_PROFILES_H
#define TAGPU_PROFILES_H
#include "tagpu_mod.h"

typedef struct {
    int manual, quick, radius, maximum, quick_default;
} TagpuResourcePolicy;

/* The player's changes to a mod's play options: the keys of profiles\<id>.ini. */
enum { TAGPU_CHOICE_RADIUS, TAGPU_CHOICE_QUICK, TAGPU_CHOICES };

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
void tagpu_profiles_status(char* out, unsigned cap);
/* The player's raw value, -1 for "the mod's default", kept above a cap or under a lock; and a value
   put back, by the menu's Undo and Restore, -1 removing the key. */
int tagpu_profiles_choice(int which);
void tagpu_profiles_restore_choice(int which, int value);
/* Any mod's file, for the Mods screen at the main menu, where a write inside a click holds up no game.
   The player's value of a choice, or -1; hide.<archive>'s, 1 hidden, 0 mounted, -1 no key, without case. */
int tagpu_profiles_choice_of(const char* id, int which);
int tagpu_profiles_hide_of(const char* id, const char* archive);
/* Sets a key of that file, -1 removing it: 1, or 0 with the reason in the log. */
int tagpu_profiles_set_hide_of(const char* id, const char* archive, int value);
/* A base mod's file holds its mutators' switches too ("none" the folder's own game): mutator.<id> and file.<path>, each
   1 on, 0 off, -1 no key, without case. A mutator is off and a file on with no key. */
int tagpu_profiles_mutator_of(const char* base, const char* mutator);
int tagpu_profiles_file_of(const char* base, const char* path);
int tagpu_profiles_set_mutator_of(const char* base, const char* mutator, int value);
int tagpu_profiles_set_file_of(const char* base, const char* path, int value);
/* `fn` with the path of each file.<path> key in that file. */
void tagpu_profiles_each_file(const char* base, void (*fn)(void* ctx, const char* path), void* ctx);

/* The file's write: the render thread each frame, the Mods screen's Confirm, and the exit once no other thread is inside. A
   click only records its value (tagpu_menu_present). */
void tagpu_profiles_flush(void);
/* The same past the wait after a failed write, which stops a retry every frame: a player's click may try at once. */
void tagpu_profiles_flush_now(void);
/* 1 when nothing waits to be written: the flush above held. */
int  tagpu_profiles_saved(void);
void tagpu_profiles_final(void);

#endif
