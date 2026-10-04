#ifndef TAGPU_RECMP_CHANNEL_H
#define TAGPU_RECMP_CHANNEL_H

#include "tagpu_recmp_auth.h"

#define RECMP_ACTORS 10u
typedef struct {
    DWORD origin;
    unsigned local;
    RecMpPublic public;
} RecMpActor;

/* IDs and public keys come from the actual primary native players. local agrees
   with native ownership and the game controller. The signer is process-lifetime
   memory; no engine pointer or primary interface is retained. */
struct RecMpConfig {
    RecMpNetConfig net;
    DWORD origin;
    unsigned count;
    RecMpActor actor[RECMP_ACTORS];
    const RecMpKey* signer;
};

#endif
