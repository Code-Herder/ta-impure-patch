#include "tagpu_recmp_auth.h"
#include <wincrypt.h>
#include <string.h>

struct RecMpKey {
    HCRYPTPROV provider;
    HCRYPTKEY key;
    RecMpPublic public;
};

static PVOID volatile s_key;

static int public_valid(const RecMpPublic* public)
{
    PUBLICKEYSTRUC header;
    RSAPUBKEY rsa;
    memcpy(&header, public->key, sizeof header);
    memcpy(&rsa, public->key + sizeof header, sizeof rsa);
    return public->version == RECMP_VERSION && public->bytes == sizeof *public &&
        header.bType == PUBLICKEYBLOB && header.bVersion == CUR_BLOB_VERSION &&
        !header.reserved && header.aiKeyAlg == CALG_RSA_SIGN &&
        rsa.magic == 0x31415352u && rsa.bitlen == 2048 && rsa.pubexp == 65537;
}

int tagpu_recmp_public_read(const void* bytes, unsigned size, RecMpPublic* out)
{
    RecMpPublic public;
    if (!bytes || !out || size != sizeof public) return 0;
    memcpy(&public, bytes, sizeof public);
    if (!public_valid(&public)) return 0;
    *out = public;
    return 1;
}

static void key_destroy(RecMpKey* key)
{
    if (key->key) CryptDestroyKey(key->key);
    if (key->provider) CryptReleaseContext(key->provider, 0);
    VirtualFree(key, 0, MEM_RELEASE);
}

const RecMpKey* tagpu_recmp_key(void)
{
    RecMpKey* key = InterlockedCompareExchangePointer(&s_key, NULL, NULL);
    RecMpKey* winner;
    DWORD bytes = RECMP_PUBLIC_BYTES;
    if (key) return key;
    key = VirtualAlloc(NULL, sizeof *key, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!key) return NULL;
    /* VERIFYCONTEXT creates only temporary key material. No user key container
       is opened or persisted, and SILENT refuses provider UI. */
    if (!CryptAcquireContextA(&key->provider, NULL, MS_ENH_RSA_AES_PROV_A,
                              PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT) ||
        !CryptGenKey(key->provider, AT_SIGNATURE, (2048u << 16) | CRYPT_EXPORTABLE, &key->key) ||
        !CryptExportKey(key->key, 0, PUBLICKEYBLOB, 0, key->public.key, &bytes) ||
        bytes != RECMP_PUBLIC_BYTES) goto failed;
    key->public.version = RECMP_VERSION;
    key->public.bytes = sizeof key->public;
    if (!public_valid(&key->public)) goto failed;
    winner = InterlockedCompareExchangePointer(&s_key, key, NULL);
    if (winner) { key_destroy(key); return winner; }
    return key;
failed:
    key_destroy(key);
    return NULL;
}

void tagpu_recmp_public(const RecMpKey* key, RecMpPublic* out) { *out = key->public; }

static int stream_present(const GUID* stream)
{
    static const GUID zero = {0};
    return stream && memcmp(stream, &zero, sizeof zero);
}

static int binding_hash(HCRYPTPROV provider, DWORD origin, const GUID* stream,
                         const RecMpIdentity* identity, HCRYPTHASH* hash)
{
    static const unsigned char domain[] = "IMPURE-PRIVATE-BINDING-1";
    unsigned char binding[sizeof origin + sizeof *stream + sizeof *identity];
    memcpy(binding, &origin, sizeof origin);
    memcpy(binding + sizeof origin, stream, sizeof *stream);
    memcpy(binding + sizeof origin + sizeof *stream, identity, sizeof *identity);
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, hash)) return 0;
    return CryptHashData(*hash, domain, sizeof domain, 0) &&
           CryptHashData(*hash, binding, sizeof binding, 0);
}

int tagpu_recmp_sign(const RecMpKey* key, DWORD origin, const GUID* stream,
                     const RecMpIdentity* identity, RecMpSignedIdentity* out)
{
    RecMpSignedIdentity signed_identity = {0};
    HCRYPTHASH hash = 0;
    DWORD bytes = sizeof signed_identity.signature;
    int result = 0;
    if (!key || !origin || !stream_present(stream) || !identity || !out ||
        !tagpu_recmp_identity_read(identity, sizeof *identity, &identity->match,
                                   &signed_identity.identity)) return 0;
    if (binding_hash(key->provider, origin, stream, identity, &hash) &&
        CryptSignHashA(hash, AT_SIGNATURE, NULL, 0, signed_identity.signature, &bytes) &&
        bytes == sizeof signed_identity.signature) {
        *out = signed_identity;
        result = 1;
    }
    if (hash) CryptDestroyHash(hash);
    return result;
}

int tagpu_recmp_verify(const RecMpPublic* key, DWORD origin, const GUID* stream,
                       const RecMpIdentity* local, DWORD from,
                       const void* bytes, unsigned size, RecMpSignedIdentity* out)
{
    RecMpSignedIdentity signed_identity;
    RecMpIdentity checked;
    HCRYPTPROV provider = 0;
    HCRYPTKEY public_key = 0;
    HCRYPTHASH hash = 0;
    int result = 0;
    if (!key || !public_valid(key) || !origin || !stream_present(stream) || !local ||
        !from || from == local->player || !bytes || size != sizeof signed_identity || !out) return 0;
    memcpy(&signed_identity, bytes, sizeof signed_identity);
    if (!tagpu_recmp_identity_read(&signed_identity.identity, sizeof signed_identity.identity,
                                   &local->match, &checked) || checked.player != from ||
        memcmp(&checked.session, &local->session, sizeof checked.session)) return 0;
    if (CryptAcquireContextA(&provider, NULL, MS_ENH_RSA_AES_PROV_A,
                             PROV_RSA_AES, CRYPT_VERIFYCONTEXT | CRYPT_SILENT) &&
        CryptImportKey(provider, key->key, sizeof key->key, 0, 0, &public_key) &&
        binding_hash(provider, origin, stream, &signed_identity.identity, &hash) &&
        CryptVerifySignatureA(hash, signed_identity.signature, sizeof signed_identity.signature,
                              public_key, NULL, 0)) {
        *out = signed_identity;
        result = 1;
    }
    if (hash) CryptDestroyHash(hash);
    if (public_key) CryptDestroyKey(public_key);
    if (provider) CryptReleaseContext(provider, 0);
    return result;
}
