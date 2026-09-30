/* tagpu_puppet.c -- the puppet host's table (tagpu_puppet.h). */
#include <string.h>
#include "tagpu_puppet.h"
#include "tagpu_simreg.h"

typedef struct PuppetTable {
    volatile unsigned char on;              /* this level plays a recording                 */
    unsigned char seat[PUPPET_PLAYERS];     /* the record is a puppet seat, V included      */
    unsigned char local[PUPPET_PLAYERS];    /* getter 75's answer for its units             */
} PuppetTable;
static PuppetTable s_pup;
SIM_VAR(s_pup, SIM_CONTENT);

int tagpu_puppet_on(void) { return s_pup.on; }

int tagpu_puppet_seat(unsigned player)
{
    return s_pup.on && player < PUPPET_PLAYERS && s_pup.seat[player];
}

int tagpu_puppet_local(unsigned player)
{
    return player < PUPPET_PLAYERS ? s_pup.local[player] : 0;
}

const volatile unsigned char* tagpu_puppet_flag(void) { return &s_pup.on; }

void tagpu_puppet_begin(const unsigned char seat[PUPPET_PLAYERS],
                        const unsigned char local[PUPPET_PLAYERS])
{
    unsigned i;
    for (i = 0; i < PUPPET_PLAYERS; i++) {
        s_pup.seat[i] = seat[i] ? 1 : 0;
        s_pup.local[i] = local[i] ? 1 : 0;
    }
    s_pup.on = 1;
}

void tagpu_puppet_end(void)
{
    s_pup.on = 0;
    memset(s_pup.seat, 0, sizeof s_pup.seat);
    memset(s_pup.local, 0, sizeof s_pup.local);
}
