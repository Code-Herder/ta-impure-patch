#ifndef TAGPU_RECMP_NET_H
#define TAGPU_RECMP_NET_H

#include "tagpu_recmp.h"

#define RECMP_NET_PENDING 16u
#define RECMP_NET_ADDRESS 4096u

#pragma pack(push, 1)
typedef struct {
    uint16_t version, bytes;
    GUID match, session;
    uint32_t player;
} RecMpIdentity;
#pragma pack(pop)
_Static_assert(sizeof(RecMpIdentity) == 40, "version-1 private identity wire span");

typedef struct {
    GUID provider, match;
    unsigned host, address_bytes;
    unsigned char address[RECMP_NET_ADDRESS];
} RecMpNetConfig;

typedef struct RecMpNet RecMpNet;
typedef struct {
    unsigned pending, completed, completed_bytes, peers, membership;
    HRESULT failed;
} RecMpNetStats;

/* One worker owns the module, both COM objects, session, player and receive event.
   config is a copied primary-session address and match ID; it contains no engine
   pointer or primary-session interface. No call is made on TA's send queue. */
HRESULT tagpu_recmp_net_open(const RecMpNetConfig* config, RecMpNet** out);
const RecMpIdentity* tagpu_recmp_net_identity(const RecMpNet* net);
HANDLE tagpu_recmp_net_event(const RecMpNet* net);
HRESULT tagpu_recmp_net_send(RecMpNet* net, const void* packet, unsigned bytes);
HRESULT tagpu_recmp_net_receive(RecMpNet* net, DWORD* from, void* packet, unsigned* bytes);
void tagpu_recmp_net_stats(const RecMpNet* net, RecMpNetStats* out);
/* close refuses pending sends. abort destroys only the worker's own session and
   reports no delivery success; callers must already have marked the recording incomplete. */
HRESULT tagpu_recmp_net_close(RecMpNet* net);
void tagpu_recmp_net_abort(RecMpNet* net);

/* A binding is verified against the actual primary native player's public key,
   never learned from an unsigned private origin claim. Session and native private
   sender must both match. */
int tagpu_recmp_identity_read(const void* bytes, unsigned size, const GUID* match,
                              RecMpIdentity* out);
int tagpu_recmp_net_authorized(const RecMpNet* net, const RecMpIdentity* binding, DWORD from);

#endif
