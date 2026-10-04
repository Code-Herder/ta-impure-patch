/* Exercise the primary-owned public key and signed private identity without TA. */
#include "tagpu_recmp_auth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static HANDLE release;
static const RecMpKey* keys[4];

static DWORD WINAPI creator(void* arg)
{
    unsigned at = (unsigned)(uintptr_t)arg;
    CHECK(WaitForSingleObject(release,5000) == WAIT_OBJECT_0);
    keys[at] = tagpu_recmp_key();
    CHECK(keys[at]);
    return 0;
}

int main(void)
{
    HANDLE threads[4];
    RecMpPublic public, parsed, before, wrong_key;
    RecMpIdentity identity = {1,sizeof identity,{.Data1=1},{.Data1=2},11};
    RecMpIdentity local = identity;
    RecMpSignedIdentity signed_identity, out, bad, previous;
    GUID stream = {.Data1=3}, wrong_stream = {.Data1=4};
    unsigned char* pages;
    DWORD old, i;
    local.player = 22;
    release = CreateEvent(NULL,TRUE,FALSE,NULL);
    CHECK(release);
    for (i=0;i<4;i++) {
        threads[i] = CreateThread(NULL,0,creator,(void*)(uintptr_t)i,0,NULL);
        CHECK(threads[i]);
    }
    CHECK(SetEvent(release));
    CHECK(WaitForMultipleObjects(4,threads,TRUE,15000) == WAIT_OBJECT_0);
    for (i=0;i<4;i++) { CHECK(keys[i] == keys[0]); CloseHandle(threads[i]); }
    CloseHandle(release);
    tagpu_recmp_public(keys[0],&public);
    CHECK(tagpu_recmp_public_read(&public,sizeof public,&parsed));
    CHECK(!memcmp(&public,&parsed,sizeof public));
    pages = VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    memcpy(pages+4096-sizeof public,&public,sizeof public);
    CHECK(tagpu_recmp_public_read(pages+4096-sizeof public,sizeof public,&parsed));
    before = parsed;
    CHECK(!tagpu_recmp_public_read(pages+4096,sizeof public-1,&parsed));
    CHECK(!tagpu_recmp_public_read(pages+4096-sizeof public,sizeof public+1,&parsed));
    wrong_key = public; wrong_key.version++;
    CHECK(!tagpu_recmp_public_read(&wrong_key,sizeof wrong_key,&parsed));
    wrong_key = public; wrong_key.key[12] ^= 1;
    CHECK(!tagpu_recmp_public_read(&wrong_key,sizeof wrong_key,&parsed));
    CHECK(!memcmp(&before,&parsed,sizeof before));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    CHECK(tagpu_recmp_sign(keys[0],51,&stream,&identity,&signed_identity));
    CHECK(tagpu_recmp_verify(&public,51,&stream,&local,11,
                             &signed_identity,sizeof signed_identity,&out));
    previous = out;
    CHECK(!tagpu_recmp_verify(&public,52,&stream,&local,11,
                              &signed_identity,sizeof signed_identity,&out));
    CHECK(!tagpu_recmp_verify(&public,51,&wrong_stream,&local,11,
                              &signed_identity,sizeof signed_identity,&out));
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,12,
                              &signed_identity,sizeof signed_identity,&out));
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&identity,11,
                              &signed_identity,sizeof signed_identity,&out));
    bad = signed_identity; bad.identity.player = 12;
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,12,&bad,sizeof bad,&out));
    bad = signed_identity; bad.identity.match.Data1++;
    local.match = bad.identity.match;
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,11,&bad,sizeof bad,&out));
    local = identity; local.player = 22;
    bad = signed_identity; bad.identity.session.Data1++;
    local.session = bad.identity.session;
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,11,&bad,sizeof bad,&out));
    local = identity; local.player = 22;
    bad = signed_identity; bad.signature[100] ^= 1;
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,11,&bad,sizeof bad,&out));
    wrong_key = public; wrong_key.key[64] ^= 1;
    CHECK(!tagpu_recmp_verify(&wrong_key,51,&stream,&local,11,
                              &signed_identity,sizeof signed_identity,&out));
    CHECK(!memcmp(&previous,&out,sizeof out));
    pages = VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    memcpy(pages+4096-sizeof signed_identity,&signed_identity,sizeof signed_identity);
    CHECK(tagpu_recmp_verify(&public,51,&stream,&local,11,
                             pages+4096-sizeof signed_identity,sizeof signed_identity,&out));
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,11,
                              pages+4096,sizeof signed_identity-1,&out));
    CHECK(!tagpu_recmp_verify(&public,51,&stream,&local,11,
                              pages+4096-sizeof signed_identity,sizeof signed_identity+1,&out));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    puts("PASS: concurrent immutable key publication, bounded public/signature spans, signed origin/stream/match/session/private sender, wrong key rejected");
    return 0;
}
