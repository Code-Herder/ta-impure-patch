#ifndef TAGPU_RECMP_AUTH_H
#define TAGPU_RECMP_AUTH_H

#include "tagpu_recmp_net.h"

#define RECMP_PUBLIC_BYTES 276u
#define RECMP_SIGNATURE_BYTES 256u
#define RECMP_PRIMARY_BYTES 21u

#pragma pack(push, 1)
typedef struct {
    uint16_t version, bytes;
    unsigned char key[RECMP_PUBLIC_BYTES];
} RecMpPublic;
typedef struct {
    RecMpIdentity identity;
    unsigned char signature[RECMP_SIGNATURE_BYTES];
} RecMpSignedIdentity;
#pragma pack(pop)
_Static_assert(sizeof(RecMpPublic) == 280, "version-1 primary public identity span");
_Static_assert(sizeof(RecMpSignedIdentity) == 296, "version-1 signed private identity span");

typedef struct RecMpKey RecMpKey;

/* Created at native player creation, before a played match. The winning key and
   its provider live for the process. Concurrent creators publish only a fully
   built immutable key; losing candidates are their creator's own objects. */
const RecMpKey* tagpu_recmp_key(void);
void tagpu_recmp_public(const RecMpKey* key, RecMpPublic* out);
int tagpu_recmp_public_read(const void* bytes, unsigned size, RecMpPublic* out);

/* Signing and verification run on the private worker. A public key must come
   from GetPlayerData for the actual primary native ID, never a private claim.
   Origin, stream, match, session and private sender are covered by the signature. */
int tagpu_recmp_sign(const RecMpKey* key, DWORD origin, const GUID* stream,
                     const RecMpIdentity* identity, RecMpSignedIdentity* out);
int tagpu_recmp_verify(const RecMpPublic* key, DWORD origin, const GUID* stream,
                       const RecMpIdentity* local, DWORD from,
                       const void* bytes, unsigned size, RecMpSignedIdentity* out);

#endif
