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
int tagpu_profiles_radius(void);
int tagpu_profiles_quick(void);
void tagpu_profiles_set_radius(int value);
void tagpu_profiles_set_quick(int value);
void tagpu_profiles_status(char* out, unsigned cap);
/* The player's raw value, -1 for "the mod's default", kept above a cap or under a lock; and the
   same value put back, by the menu's Undo. */
int tagpu_profiles_choice(int which);
void tagpu_profiles_restore_choice(int which, int value);

#endif
