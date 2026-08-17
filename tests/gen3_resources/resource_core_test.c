/*
 * Stage M0/M1 shared Gen3 resource core test (guardrails 1-20).
 *
 * Links the platform-neutral core (sha256.c + resource_id.c + resource_core.c
 * + resource_resolver.c) with NO Emerald defines, NO global.h / gba / SDL /
 * desktop / platform includes. See tests/gen3_resources/run.sh for the
 * isolation build and run_sanitize.sh for the ASan+UBSan build.
 *
 * Coverage by guardrail:
 *   1  canonical names hash EXACTLY their canonical bytes (fixed vectors prove
 *      the "gen3-resource-id-v1\0" NUL is included; no name rewriting);
 *   2  the internal SHA-256 matches the NIST standard vectors exactly;
 *   3  the resource key is a fixed 32-byte binary value (hex is diagnostic only);
 *   4  the injected key-derivation seam proves two different names mapped to the
 *      same key are rejected (production hashing untouched);
 *   5  catalog insertion order never changes keys, handles, or results;
 *   6  provider precedence is explicit metadata and wins over registration order;
 *      duplicate precedence is rejected;
 *   7  candidate construction is off to the side: valid A -> invalid B -> B
 *      rejected -> A remains active and bit-for-bit unchanged;
 *   8  ownership/lifetime: providers and catalogs own deep copies; snapshots are
 *      independent of candidates; payloads survive their source buffers;
 *   9  the generic resource view exposes only stable descriptive/runtime fields;
 *  10  type mismatch and schema mismatch are distinct errors and the catalog owns
 *      the contract (a provider cannot redefine it);
 *  11  an invalid OPTIONAL override records a rejection and falls through to the
 *      first valid lower provider;
 *  12  an invalid REQUIRED provider entry fails candidate construction;
 *  13  a missing required-for-base resource is reported separately from a
 *      malformed required provider entry;
 *  14  diagnostics are structured reason enums (tests never match English text);
 *  15  resolution traces are deterministic (exact golden record sequence, no
 *      pointers/timestamps/container order);
 *  16  no filesystem / manifest / ROM / SDL / Emerald integration (compile-time);
 *  17  no Emerald compatibility tables initialized (inventory doc only);
 *  18  build isolation (enforced by run.sh);
 *  19  sanitizer clean under ASan+UBSan (enforced by run_sanitize.sh);
 *  20  M0/M1 boundary: no M2 code exists; scope is documented in the report.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_provider.h"
#include "gen3/resources/resource_resolver.h"
#include "gen3/resources/resource_types.h"
#include "gen3/resources/resource_version.h"

/* Test-only seams; only reachable through the isolation include path. */
#include "resource_internal.h"
#include "gen3/resources/sha256.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *NameAlpha = "gen3:test/alpha";
static const char *NameBeta = "gen3:test/beta";
static const char *NamePal = "gen3:test/palette";
static const char *NameOptional = "gen3:test/optional";
static const char *NameBase = "gen3:base/needed";

static uint8_t Payload64[64];       /* valid TILE_GRAPHICS (64 % 32 == 0) */
static uint8_t PayloadPal[32];      /* valid PALETTE (32 % 2 == 0, <= 512) */
static uint8_t Payload10[10];       /* INVALID TILE_GRAPHICS (not % 32) */
static uint8_t Payload600[600];     /* INVALID PALETTE (> 512 bytes) */

static void InitPayloads(void)
{
    size_t i;
    for (i = 0; i < sizeof(Payload64); i++)
        Payload64[i] = (uint8_t)(i * 3u + 1u);
    for (i = 0; i < sizeof(PayloadPal); i++)
        PayloadPal[i] = (uint8_t)(i * 5u + 2u);
    memset(Payload10, 0xEE, sizeof(Payload10));
    memset(Payload600, 0xDD, sizeof(Payload600));
}

static void DiagInit(struct Gen3ResourceDiagnosticList *diag)
{
    Gen3ResourceDiagnostics_Init(diag);
}

static void DiagDestroy(struct Gen3ResourceDiagnosticList *diag)
{
    Gen3ResourceDiagnostics_Destroy(diag);
}

static bool DiagHas(const struct Gen3ResourceDiagnosticList *diag,
                    enum Gen3ResourceReason reason)
{
    size_t i;
    for (i = 0; i < diag->count; i++)
        if (diag->items[i].reason == reason)
            return true;
    return false;
}

static bool DiagHasWithSeverity(const struct Gen3ResourceDiagnosticList *diag,
                                enum Gen3ResourceReason reason,
                                enum Gen3ResourceDiagnosticSeverity severity,
                                const char *providerId)
{
    size_t i;
    for (i = 0; i < diag->count; i++)
    {
        const struct Gen3ResourceDiagnostic *d = &diag->items[i];
        if (d->reason == reason && d->severity == severity
         && (providerId == NULL || strcmp(d->providerId, providerId) == 0))
            return true;
    }
    return false;
}

static size_t DiagCountFor(const struct Gen3ResourceDiagnosticList *diag,
                           enum Gen3ResourceReason reason)
{
    size_t i;
    size_t count = 0;
    for (i = 0; i < diag->count; i++)
        if (diag->items[i].reason == reason)
            count++;
    return count;
}

/* A catalog with four resources; NameAlpha and NamePal are required-for-base. */
static struct Gen3ResourceCatalog *MakeCatalog(void)
{
    struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, NameAlpha,
                                   GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, true, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameBeta,
                                   GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NamePal,
                                   GEN3_RESOURCE_TYPE_PALETTE, 1, true, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameOptional,
                                   GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Finalize(catalog, NULL));
    return catalog;
}

/* A base-kind provider that validly supplies every MakeCatalog resource. */
static struct Gen3ResourceProvider *MakeBaseProvider(uint32_t precedence,
                                                     const char *id,
                                                     const char *version)
{
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceProvider *provider;
    meta.id = id;
    meta.version = version;
    meta.kind = GEN3_PROVIDER_ROM_BASE;
    meta.precedence = precedence;
    provider = Gen3ResourceProvider_Create(&meta);
    assert(provider != NULL);
    assert(Gen3ResourceProvider_Add(provider, NameAlpha,
                                    GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1,
                                    Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Add(provider, NameBeta,
                                    GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1,
                                    Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Add(provider, NamePal,
                                    GEN3_RESOURCE_TYPE_PALETTE, 1,
                                    PayloadPal, sizeof(PayloadPal), false, NULL));
    assert(Gen3ResourceProvider_Add(provider, NameOptional,
                                    GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1,
                                    Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Finalize(provider, NULL));
    return provider;
}

static struct Gen3ResourceSnapshot *BuildCandidateSnapshot(
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourceProvider *const *providers,
    size_t providerCount,
    struct Gen3ResourceDiagnosticList *diag)
{
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    size_t i;
    candidate = Gen3ResourceCandidate_Create(catalog, diag);
    assert(candidate != NULL);
    for (i = 0; i < providerCount; i++)
        assert(Gen3ResourceCandidate_AddProvider(candidate, providers[i], diag));
    assert(Gen3ResourceCandidate_Build(candidate, &snapshot, diag));
    Gen3ResourceCandidate_Destroy(candidate);
    return snapshot;
}

/* ------------------------------------------------------------------ */
/* 2. SHA-256 matches the NIST standard vectors exactly               */
/* ------------------------------------------------------------------ */
static void TestSha256StandardVectors(void)
{
    struct Gen3Sha256Context context;
    uint8_t digest[32];
    static const uint8_t empty[32] =
    {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
    };
    static const uint8_t abc[32] =
    {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };

    Gen3Sha256_Init(&context);
    Gen3Sha256_Final(&context, digest);
    assert(memcmp(digest, empty, 32) == 0);

    Gen3Sha256_Init(&context);
    Gen3Sha256_Update(&context, "abc", 3);
    Gen3Sha256_Final(&context, digest);
    assert(memcmp(digest, abc, 32) == 0);
    printf("2: SHA-256 standard vectors (empty, \"abc\") match exactly ok\n");
}

/* ------------------------------------------------------------------ */
/* 1. Fixed vectors prove the NUL between domain and name is included  */
/* ------------------------------------------------------------------ */
static void ExpectKeyHex(const char *name, const char *expectedHex)
{
    Gen3ResourceKey key;
    char hex[GEN3_RESOURCE_KEY_HEX_SIZE];
    Gen3ResourceId_DeriveKey(name, &key);
    Gen3ResourceId_FormatKeyHex(&key, hex);
    assert(strcmp(hex, expectedHex) == 0);
}

static void TestKeyDerivationFixedVectors(void)
{
    /* Externally computed (independent sha256sum) over the EXACT bytes
     * "gen3-resource-id-v1\0" + canonical-name-bytes. */
    ExpectKeyHex("gen3:test/alpha",
                 "d0a1dc59cae5153ec0b7f4095e4ca5b912b69b433e247b1156fc010033762058");
    ExpectKeyHex("gen3:graphics/hero/normal/back",
                 "64d05412317c3ff4fb15f8ca751c05167f46f2e68174310a0facc6690f9abcc2");
    ExpectKeyHex("mod:org.example.foo/sprites/icon",
                 "8779be6dfb15d95c6004c682756861e5d3e224455301a9e37c45f15eeccab46a");

    /* Prove the terminating NUL is genuinely part of the hash input: if the
     * NUL were dropped the digest of "gen3:test/alpha" would begin 1c0bbc78...
     * (independently computed). It does not. */
    {
        Gen3ResourceKey key;
        Gen3ResourceId_DeriveKey("gen3:test/alpha", &key);
        assert(key.bytes[0] != 0x1c);
    }
    printf("1: fixed key-derivation vectors prove the domain NUL is included ok\n");
}

/* ------------------------------------------------------------------ */
/* 1. Canonical names: validated, never normalized before hashing      */
/* ------------------------------------------------------------------ */
static void TestCanonicalNameValidation(void)
{
    char longName[300];

    assert(Gen3ResourceId_ValidateCanonicalName("gen3:test/alpha") == GEN3_RESOURCE_NAME_VALID);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a") == GEN3_RESOURCE_NAME_VALID);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a.b-c_d/0/9") == GEN3_RESOURCE_NAME_VALID);
    assert(Gen3ResourceId_ValidateCanonicalName("emerald:font/small/latin/glyphs") == GEN3_RESOURCE_NAME_VALID);
    assert(Gen3ResourceId_ValidateCanonicalName("mod:org.example.foo/sprites/icon") == GEN3_RESOURCE_NAME_VALID);

    assert(Gen3ResourceId_ValidateCanonicalName(NULL) == GEN3_RESOURCE_NAME_NULL);
    assert(Gen3ResourceId_ValidateCanonicalName("") == GEN3_RESOURCE_NAME_EMPTY);
    memset(longName, 'a', sizeof(longName));
    longName[256] = '\0';   /* 256 bytes: over the 255 limit */
    assert(Gen3ResourceId_ValidateCanonicalName(longName) == GEN3_RESOURCE_NAME_TOO_LONG);
    assert(Gen3ResourceId_ValidateCanonicalName("foo:bar") == GEN3_RESOURCE_NAME_INVALID_NAMESPACE);
    assert(Gen3ResourceId_ValidateCanonicalName("GEN3:test") == GEN3_RESOURCE_NAME_INVALID_NAMESPACE);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:TEST") == GEN3_RESOURCE_NAME_INVALID_CHARACTER);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:foo bar") == GEN3_RESOURCE_NAME_INVALID_CHARACTER);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:") == GEN3_RESOURCE_NAME_INVALID_NAMESPACE);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a/") == GEN3_RESOURCE_NAME_EMPTY_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:/a") == GEN3_RESOURCE_NAME_EMPTY_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a//b") == GEN3_RESOURCE_NAME_EMPTY_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:./a") == GEN3_RESOURCE_NAME_DOT_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:../a") == GEN3_RESOURCE_NAME_DOT_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a/../c") == GEN3_RESOURCE_NAME_DOT_SEGMENT);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a\\b") == GEN3_RESOURCE_NAME_INVALID_CHARACTER);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a:b") == GEN3_RESOURCE_NAME_INVALID_NAMESPACE);
    assert(Gen3ResourceId_ValidateCanonicalName("gen3:a\tb") == GEN3_RESOURCE_NAME_INVALID_CHARACTER);

    /* The accepted name hashes its EXACT bytes (the fixed vector pins the byte
     * sequence; nothing lowercases/trims/replaces). No normalization exists. */
    ExpectKeyHex("gen3:test/alpha",
                 "d0a1dc59cae5153ec0b7f4095e4ca5b912b69b433e247b1156fc010033762058");
    printf("1: canonical-name validation (no normalization; rejected cases) ok\n");
}

/* ------------------------------------------------------------------ */
/* 3. The key is a fixed 32-byte binary value; hex is diagnostic only  */
/* ------------------------------------------------------------------ */
static void TestKeyRepresentation(void)
{
    Gen3ResourceKey keyA;
    Gen3ResourceKey keyB;
    char hex[GEN3_RESOURCE_KEY_HEX_SIZE];
    size_t i;

    assert(GEN3_RESOURCE_KEY_SIZE == 32);
    Gen3ResourceId_DeriveKey(NameAlpha, &keyA);
    Gen3ResourceId_DeriveKey(NameBeta, &keyB);

    /* Same name -> identical bytes; different names -> different bytes. */
    assert(Gen3ResourceId_KeyEqual(&keyA, &keyA));
    assert(!Gen3ResourceId_KeyEqual(&keyA, &keyB));
    assert(!Gen3ResourceId_KeyEqual(NULL, &keyA));
    assert(!Gen3ResourceId_KeyEqual(&keyA, NULL));

    /* A full-width digest: at least two distinct byte values (never a
     * truncated / collapsed identity). */
    {
        int distinct = 0;
        for (i = 0; i < GEN3_RESOURCE_KEY_SIZE && distinct < 2; i++)
        {
            size_t j;
            bool seen = false;
            for (j = 0; j < i; j++)
            {
                if (keyA.bytes[j] == keyA.bytes[i])
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                distinct++;
        }
        assert(distinct >= 2);
    }

    /* Hex formatting is lossless diagnostic text only. */
    Gen3ResourceId_FormatKeyHex(&keyA, hex);
    assert(strlen(hex) == GEN3_RESOURCE_KEY_SIZE * 2u);
    for (i = 0; i < GEN3_RESOURCE_KEY_SIZE; i++)
    {
        unsigned hi = hex[i * 2u];
        unsigned lo = hex[i * 2u + 1u];
        assert((hi >= '0' && hi <= '9') || (hi >= 'a' && hi <= 'f'));
        assert((lo >= '0' && lo <= '9') || (lo >= 'a' && lo <= 'f'));
        assert(((hi <= '9' ? hi - '0' : hi - 'a' + 10u) << 4u
              | (lo <= '9' ? lo - '0' : lo - 'a' + 10u)) == keyA.bytes[i]);
    }
    printf("3: key is a fixed 32-byte binary value; hex is lossless diagnostics ok\n");
}

/* ------------------------------------------------------------------ */
/* 4. Injection seam: two different names, same key -> rejected        */
/* ------------------------------------------------------------------ */
static void TestDeriverAllSame(const char *name, Gen3ResourceKey *outKey, void *context)
{
    (void)name;
    (void)context;
    memset(outKey->bytes, 0xAA, GEN3_RESOURCE_KEY_SIZE);
}

static void TestDeriverGrouped(const char *name, Gen3ResourceKey *outKey, void *context)
{
    (void)context;
    memset(outKey->bytes,
           (strcmp(name, "gen3:a") == 0 || strcmp(name, "gen3:b") == 0) ? 0xAB : 0xCD,
           GEN3_RESOURCE_KEY_SIZE);
}

static void TestCollisionSeam(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceDiagnosticList diag;

    /* Two different canonical names derived to the SAME key are rejected. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    Gen3ResourceCatalog_SetKeyDeriverForTest(catalog, TestDeriverGrouped, NULL);
    DiagInit(&diag);
    assert(Gen3ResourceCatalog_Add(catalog, "gen3:a", GEN3_RESOURCE_TYPE_BINARY, 1, false, &diag));
    assert(Gen3ResourceCatalog_Add(catalog, "gen3:c", GEN3_RESOURCE_TYPE_BINARY, 1, false, &diag));
    assert(!Gen3ResourceCatalog_Add(catalog, "gen3:b", GEN3_RESOURCE_TYPE_BINARY, 1, false, &diag));
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_RESOURCE_KEY_COLLISION));
    assert(DiagCountFor(&diag, GEN3_RESOURCE_REASON_DUPLICATE_CATALOG_NAME) == 0);
    DiagDestroy(&diag);
    Gen3ResourceCatalog_Destroy(catalog);

    /* The seam is only installable BEFORE any entry exists (cannot weaken a
     * catalog already under construction): refused here, so "gen3:b" is hashed
     * with the production deriver and does NOT collide. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, "gen3:a", GEN3_RESOURCE_TYPE_BINARY, 1, false, NULL));
    Gen3ResourceCatalog_SetKeyDeriverForTest(catalog, TestDeriverAllSame, NULL);
    assert(Gen3ResourceCatalog_Add(catalog, "gen3:b", GEN3_RESOURCE_TYPE_BINARY, 1, false, NULL));

    /* Production hashing is untouched: with the standard deriver two different
     * names never collide. */
    {
        Gen3ResourceKey ka;
        Gen3ResourceKey kb;
        Gen3ResourceId_DeriveKey("gen3:a", &ka);
        Gen3ResourceId_DeriveKey("gen3:b", &kb);
        assert(!Gen3ResourceId_KeyEqual(&ka, &kb));
    }
    Gen3ResourceCatalog_Destroy(catalog);
    printf("4: collision seam rejects same-key different names; production hash untouched ok\n");
}

/* ------------------------------------------------------------------ */
/* 5. Catalog insertion order never changes keys/handles/results       */
/* ------------------------------------------------------------------ */
static void TestCatalogOrderIndependence(void)
{
    static const char *const names[] =
    {
        "gen3:zebra", "gen3:mango", "gen3:apple",
    };
    static const size_t orders[3][3] =
    {
        {0, 1, 2},   /* forward  */
        {2, 1, 0},   /* reverse  */
        {1, 2, 0},   /* shuffled */
    };
    Gen3ResourceHandle expected[3];
    size_t o;
    size_t r;

    /* Handles are assigned AFTER sorting names bytewise: apple < mango < zebra. */
    {
        struct Gen3ResourceCatalog *ref = Gen3ResourceCatalog_Create();
        size_t k;
        for (k = 0; k < 3; k++)
            assert(Gen3ResourceCatalog_Add(ref, names[k], GEN3_RESOURCE_TYPE_BINARY, 1, false, NULL));
        assert(Gen3ResourceCatalog_Finalize(ref, NULL));
        for (k = 0; k < 3; k++)
        {
            const struct Gen3ResourceContract *c =
                Gen3ResourceCatalog_Find(ref, names[k]);
            assert(c != NULL);
            expected[k] = c->handle;
        }
        assert(expected[0] == 2);   /* zebra -> handle 2 */
        assert(expected[1] == 1);   /* mango -> handle 1 */
        assert(expected[2] == 0);   /* apple -> handle 0 */
        Gen3ResourceCatalog_Destroy(ref);
    }

    for (r = 0; r < 3; r++)
    {
        struct Gen3ResourceCatalog *catalog = Gen3ResourceCatalog_Create();
        size_t k;
        for (o = 0; o < 3; o++)
            assert(Gen3ResourceCatalog_Add(catalog, names[orders[r][o]],
                                           GEN3_RESOURCE_TYPE_BINARY, 1, false, NULL));
        assert(Gen3ResourceCatalog_Finalize(catalog, NULL));
        assert(Gen3ResourceCatalog_Count(catalog) == 3);
        for (k = 0; k < 3; k++)
        {
            const struct Gen3ResourceContract *c = Gen3ResourceCatalog_Find(catalog, names[k]);
            Gen3ResourceKey key;
            assert(c != NULL);
            assert(c->handle == expected[k]);
            Gen3ResourceId_DeriveKey(names[k], &key);
            assert(Gen3ResourceId_KeyEqual(&c->key, &key));
        }
        Gen3ResourceCatalog_Destroy(catalog);
    }
    printf("5: catalog insertion order does not change keys, handles, or counts ok\n");
}

/* ------------------------------------------------------------------ */
/* 6. Provider precedence is explicit metadata (not registration order)*/
/* ------------------------------------------------------------------ */
static void TestProviderPrecedenceExplicit(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *mod = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[2];

    meta.id = "mod";
    meta.version = "2.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    mod = Gen3ResourceProvider_Create(&meta);
    assert(mod != NULL);
    assert(Gen3ResourceProvider_Add(mod, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    1, Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Finalize(mod, NULL));

    /* Same precedences, two registration orders -> SAME winner (mod, prec 20). */
    providers[0] = base;
    providers[1] = mod;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 2, &diag);
    DiagDestroy(&diag);
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(strcmp(view.winningProviderId, "mod") == 0);
    assert(view.winningProviderPrecedence == 20);
    assert(view.payloadSize == sizeof(Payload64));
    assert(memcmp(view.payload, Payload64, sizeof(Payload64)) == 0);
    Gen3ResourceSnapshot_Destroy(snapshot);

    providers[0] = mod;   /* registration order swapped */
    providers[1] = base;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 2, &diag);
    DiagDestroy(&diag);
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(strcmp(view.winningProviderId, "mod") == 0);
    assert(view.winningProviderPrecedence == 20);
    Gen3ResourceSnapshot_Destroy(snapshot);

    /* Duplicate precedence is rejected (never inferred or tie-broken silently). */
    {
        struct Gen3ResourceProvider *dup = MakeBaseProvider(10, "other-base", "1.0.0");
        struct Gen3ResourceProvider *dupPair[2];
        struct Gen3ResourceCandidate *candidate;
        struct Gen3ResourceSnapshot *snap = NULL;
        dupPair[0] = base;
        dupPair[1] = dup;
        DiagInit(&diag);
        candidate = Gen3ResourceCandidate_Create(catalog, &diag);
        assert(candidate != NULL);
        assert(Gen3ResourceCandidate_AddProvider(candidate, dupPair[0], &diag));
        assert(Gen3ResourceCandidate_AddProvider(candidate, dupPair[1], &diag));
        assert(!Gen3ResourceCandidate_Build(candidate, &snap, &diag));
        assert(snap == NULL);
        assert(DiagHas(&diag, GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_PRECEDENCE));
        DiagDestroy(&diag);
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceProvider_Destroy(dup);
    }

    Gen3ResourceProvider_Destroy(mod);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("6: explicit provider precedence wins; duplicate precedence rejected ok\n");
}

/* ------------------------------------------------------------------ */
/* 10. Type mismatch and schema mismatch are distinct; catalog owns the
 *     contract                                                        */
/* ------------------------------------------------------------------ */
static void TestTypeSchemaDistinct(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *badType = NULL;
    struct Gen3ResourceProvider *badSchema = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[3];

    meta.id = "bad-type";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    badType = Gen3ResourceProvider_Create(&meta);
    assert(badType != NULL);
    /* Declared type PALETTE against a TILE_GRAPHICS catalog contract. */
    assert(Gen3ResourceProvider_Add(badType, NameAlpha, GEN3_RESOURCE_TYPE_PALETTE,
                                    1, PayloadPal, sizeof(PayloadPal), false, NULL));
    assert(Gen3ResourceProvider_Finalize(badType, NULL));

    meta.id = "bad-schema";
    meta.precedence = 30;
    badSchema = Gen3ResourceProvider_Create(&meta);
    assert(badSchema != NULL);
    /* Declared schema 2 against a schema-1 catalog contract. */
    assert(Gen3ResourceProvider_Add(badSchema, NameBeta, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    2, Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Finalize(badSchema, NULL));

    /* base supplies the required-for-base resources; badType/badSchema are the
     * optional higher-precedence overrides being rejected. */
    providers[0] = base;
    providers[1] = badType;
    providers[2] = badSchema;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 3, &diag);
    /* Optional invalid entries do not fail the candidate; both reasons surface. */
    assert(snapshot != NULL);
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_TYPE_MISMATCH));
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_SCHEMA_MISMATCH));
    assert(DiagCountFor(&diag, GEN3_RESOURCE_REASON_TYPE_MISMATCH) == 1);
    assert(DiagCountFor(&diag, GEN3_RESOURCE_REASON_SCHEMA_MISMATCH) == 1);
    DiagDestroy(&diag);

    /* The bad entries fall through: the base provider (prec 10) resolves. */
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(strcmp(view.winningProviderId, "base") == 0);
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameBeta, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(strcmp(view.winningProviderId, "base") == 0);
    Gen3ResourceSnapshot_Destroy(snapshot);

    /* The catalog owns the contract: a provider CANNOT redefine it. */
    {
        const struct Gen3ResourceContract *contract =
            Gen3ResourceCatalog_Find(catalog, NameAlpha);
        assert(contract != NULL);
        assert(contract->type == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
        assert(contract->schema == 1);
    }

    Gen3ResourceProvider_Destroy(badSchema);
    Gen3ResourceProvider_Destroy(badType);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("10: type vs schema mismatch distinct; catalog owns the contract ok\n");
}

/* ------------------------------------------------------------------ */
/* 11. Optional override fallback: invalid higher entry records a
 *     rejection and the first valid lower provider wins               */
/* ------------------------------------------------------------------ */
static void TestOptionalOverrideFallback(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *mod = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[2];

    meta.id = "mod";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    mod = Gen3ResourceProvider_Create(&meta);
    assert(mod != NULL);
    /* Invalid (schema 2 vs contract 1) and OPTIONAL (requiredForProvider=false). */
    assert(Gen3ResourceProvider_Add(mod, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    2, Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Finalize(mod, NULL));

    providers[0] = base;
    providers[1] = mod;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 2, &diag);
    assert(snapshot != NULL);

    /* The invalid override is NOT silently ignored: a rejection diagnostic
     * (WARNING, SCHEMA_MISMATCH) for provider "mod" was recorded. */
    assert(DiagHasWithSeverity(&diag, GEN3_RESOURCE_REASON_SCHEMA_MISMATCH,
                               GEN3_DIAGNOSTIC_WARNING, "mod"));
    DiagDestroy(&diag);

    /* Resolution continues downward: the first valid lower provider (base) wins. */
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(strcmp(view.winningProviderId, "base") == 0);
    assert(view.winningProviderPrecedence == 10);
    Gen3ResourceSnapshot_Destroy(snapshot);

    Gen3ResourceProvider_Destroy(mod);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("11: invalid optional override recorded + falls through to lower provider ok\n");
}

/* ------------------------------------------------------------------ */
/* 12. A REQUIRED invalid provider entry fails candidate construction  */
/* ------------------------------------------------------------------ */
static void TestRequiredOverrideFailure(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *mod = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceProvider *providers[2];

    meta.id = "mod";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    mod = Gen3ResourceProvider_Create(&meta);
    assert(mod != NULL);
    /* Invalid and requiredForProvider=true: candidate construction MUST fail. */
    assert(Gen3ResourceProvider_Add(mod, NameAlpha, GEN3_RESOURCE_TYPE_PALETTE,
                                    1, PayloadPal, sizeof(PayloadPal), true, NULL));
    assert(Gen3ResourceProvider_Finalize(mod, NULL));

    providers[0] = base;
    providers[1] = mod;
    DiagInit(&diag);
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    assert(Gen3ResourceCandidate_AddProvider(candidate, providers[0], &diag));
    assert(Gen3ResourceCandidate_AddProvider(candidate, providers[1], &diag));
    assert(!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    assert(snapshot == NULL);

    /* The failure is the malformed required provider entry (ERROR, entry marked
     * required), NOT a missing-base reason. */
    assert(DiagHasWithSeverity(&diag, GEN3_RESOURCE_REASON_TYPE_MISMATCH,
                               GEN3_DIAGNOSTIC_ERROR, "mod"));
    assert(!DiagHas(&diag, GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE));
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidate);

    Gen3ResourceProvider_Destroy(mod);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("12: invalid REQUIRED provider entry fails the candidate ok\n");
}

/* ------------------------------------------------------------------ */
/* 13. Missing required-for-base is reported SEPARATELY from a bad
 *     required provider entry                                         */
/* ------------------------------------------------------------------ */
static void TestRequiredBaseResource(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProvider *synthetic = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[1];

    /* Catalog where only NameBase is required-for-base. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameBeta, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NamePal, GEN3_RESOURCE_TYPE_PALETTE, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameOptional, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameBase, GEN3_RESOURCE_TYPE_BINARY, 1, true, NULL));
    assert(Gen3ResourceCatalog_Finalize(catalog, NULL));

    /* A NON-base provider supplies NameBase -> STILL MISSING (base kind only
     * satisfies required-for-base). */
    meta.id = "synthetic";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    synthetic = Gen3ResourceProvider_Create(&meta);
    assert(synthetic != NULL);
    assert(Gen3ResourceProvider_Add(synthetic, NameBase, GEN3_RESOURCE_TYPE_BINARY,
                                    1, "BASE-DATA", 9, false, NULL));
    assert(Gen3ResourceProvider_Finalize(synthetic, NULL));
    providers[0] = synthetic;
    DiagInit(&diag);
    {
        struct Gen3ResourceCandidate *candidate = Gen3ResourceCandidate_Create(catalog, &diag);
        struct Gen3ResourceSnapshot *snap = NULL;
        assert(candidate != NULL);
        assert(Gen3ResourceCandidate_AddProvider(candidate, providers[0], &diag));
        assert(!Gen3ResourceCandidate_Build(candidate, &snap, &diag));
        assert(snap == NULL);
        assert(DiagHasWithSeverity(&diag, GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE,
                                   GEN3_DIAGNOSTIC_ERROR, NULL));
        DiagDestroy(&diag);
        Gen3ResourceCandidate_Destroy(candidate);
    }

    /* A ROM_BASE provider supplying it makes the candidate valid. */
    {
        struct Gen3ResourceProvider *rom = NULL;
        struct Gen3ResourceProviderMetadata m;
        m.id = "rom-base";
        m.version = "1.0.0";
        m.kind = GEN3_PROVIDER_ROM_BASE;
        m.precedence = 10;
        rom = Gen3ResourceProvider_Create(&m);
        assert(rom != NULL);
        assert(Gen3ResourceProvider_Add(rom, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                        1, Payload64, sizeof(Payload64), false, NULL));
        assert(Gen3ResourceProvider_Add(rom, NameBeta, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                        1, Payload64, sizeof(Payload64), false, NULL));
        assert(Gen3ResourceProvider_Add(rom, NamePal, GEN3_RESOURCE_TYPE_PALETTE,
                                        1, PayloadPal, sizeof(PayloadPal), false, NULL));
        assert(Gen3ResourceProvider_Add(rom, NameOptional, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                        1, Payload64, sizeof(Payload64), false, NULL));
        assert(Gen3ResourceProvider_Add(rom, NameBase, GEN3_RESOURCE_TYPE_BINARY,
                                        1, "BASE-DATA", 9, false, NULL));
        assert(Gen3ResourceProvider_Finalize(rom, NULL));
        providers[0] = rom;
        DiagInit(&diag);
        snapshot = BuildCandidateSnapshot(catalog, providers, 1, &diag);
        assert(snapshot != NULL);
        assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameBase, &handle) == GEN3_RESOURCE_OK);
        assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_BINARY, 1, &view) == GEN3_RESOURCE_OK);
        assert(view.payloadSize == 9);
        assert(memcmp(view.payload, "BASE-DATA", 9) == 0);
        Gen3ResourceSnapshot_Destroy(snapshot);
        DiagDestroy(&diag);
        Gen3ResourceProvider_Destroy(rom);
    }

    /* Both failure modes in ONE candidate are reported SEPARATELY: the missing
     * required base resource AND the malformed required provider entry. */
    {
        struct Gen3ResourceProvider *badRequired = NULL;
        struct Gen3ResourceProviderMetadata m;
        struct Gen3ResourceCandidate *candidate;
        struct Gen3ResourceSnapshot *snap = NULL;
        m.id = "bad-req";
        m.version = "1.0.0";
        m.kind = GEN3_PROVIDER_SYNTHETIC;
        m.precedence = 30;
        badRequired = Gen3ResourceProvider_Create(&m);
        assert(badRequired != NULL);
        assert(Gen3ResourceProvider_Add(badRequired, NameAlpha,
                                        GEN3_RESOURCE_TYPE_PALETTE, 1,
                                        PayloadPal, sizeof(PayloadPal), true, NULL));
        assert(Gen3ResourceProvider_Finalize(badRequired, NULL));
        providers[0] = badRequired;   /* no base-kind provider at all */
        DiagInit(&diag);
        candidate = Gen3ResourceCandidate_Create(catalog, &diag);
        assert(candidate != NULL);
        assert(Gen3ResourceCandidate_AddProvider(candidate, providers[0], &diag));
        assert(!Gen3ResourceCandidate_Build(candidate, &snap, &diag));
        assert(DiagHas(&diag, GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE));
        assert(DiagHas(&diag, GEN3_RESOURCE_REASON_TYPE_MISMATCH));
        DiagDestroy(&diag);
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceProvider_Destroy(badRequired);
    }

    Gen3ResourceProvider_Destroy(synthetic);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("13: required-for-base reported separately from malformed required entry ok\n");
}

/* ------------------------------------------------------------------ */
/* 7. Transactional publication: invalid B never disturbs active A     */
/* ------------------------------------------------------------------ */
static void TestTransactionalPublication(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *mod = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceRegistry registry;
    struct Gen3ResourceSnapshot *snapshotA;
    struct Gen3ResourceSnapshot *snapshotB = NULL;
    struct Gen3ResourceCandidate *candidateB;
    struct Gen3ResourceView viewBefore;
    struct Gen3ResourceView viewAfter;
    uint8_t payloadCopy[64];
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[2];

    providers[0] = base;
    DiagInit(&diag);
    snapshotA = BuildCandidateSnapshot(catalog, providers, 1, &diag);
    DiagDestroy(&diag);
    Gen3ResourceRegistry_Init(&registry);
    Gen3ResourceRegistry_Publish(&registry, snapshotA);

    /* Record A's resolved result bit-for-bit. */
    assert(Gen3ResourceSnapshot_FindHandle(snapshotA, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshotA, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &viewBefore) == GEN3_RESOURCE_OK);
    memcpy(payloadCopy, viewBefore.payload, viewBefore.payloadSize);

    /* Build an INVALID candidate B (required invalid mod entry). */
    meta.id = "mod";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    mod = Gen3ResourceProvider_Create(&meta);
    assert(mod != NULL);
    assert(Gen3ResourceProvider_Add(mod, NameAlpha, GEN3_RESOURCE_TYPE_PALETTE,
                                    1, PayloadPal, sizeof(PayloadPal), true, NULL));
    assert(Gen3ResourceProvider_Finalize(mod, NULL));
    providers[0] = base;
    providers[1] = mod;
    DiagInit(&diag);
    candidateB = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidateB != NULL);
    assert(Gen3ResourceCandidate_AddProvider(candidateB, providers[0], &diag));
    assert(Gen3ResourceCandidate_AddProvider(candidateB, providers[1], &diag));
    assert(!Gen3ResourceCandidate_Build(candidateB, &snapshotB, &diag));
    assert(snapshotB == NULL);
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidateB);

    /* B was rejected: A remains the active snapshot, same object, unchanged. */
    assert(Gen3ResourceRegistry_Active(&registry) == snapshotA);
    assert(Gen3ResourceSnapshot_Count(snapshotA) == 4);
    assert(Gen3ResourceSnapshot_FindHandle(snapshotA, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshotA, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &viewAfter) == GEN3_RESOURCE_OK);
    assert(viewAfter.payloadSize == viewBefore.payloadSize);
    assert(memcmp(viewAfter.payload, payloadCopy, viewBefore.payloadSize) == 0);
    assert(strcmp(viewAfter.winningProviderId, viewBefore.winningProviderId) == 0);

    Gen3ResourceRegistry_Destroy(&registry);
    Gen3ResourceProvider_Destroy(mod);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("7: rejected candidate leaves published snapshot A bit-for-bit unchanged ok\n");
}

/* ------------------------------------------------------------------ */
/* 8. Ownership/lifetime: deep copies, snapshots independent of sources*/
/* ------------------------------------------------------------------ */
static void TestOwnershipLifetime(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProvider *provider;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceView view;
    Gen3ResourceHandle handle;
    uint8_t stackPayload[64];
    size_t i;

    /* Provider metadata is copied at Create: the caller's struct may be a
     * stack value that leaves scope. */
    {
        struct Gen3ResourceProviderMetadata local;
        local.id = "stack-id";
        local.version = "9.9.9";
        local.kind = GEN3_PROVIDER_SYNTHETIC;
        local.precedence = 5;
        provider = Gen3ResourceProvider_Create(&local);
    }
    assert(provider != NULL);
    assert(strcmp(provider->id, "stack-id") == 0);
    assert(provider->precedence == 5);

    /* Payload is copied at Add: mutating the source buffer cannot affect it. */
    for (i = 0; i < sizeof(stackPayload); i++)
        stackPayload[i] = (uint8_t)(0x40 + i);
    assert(Gen3ResourceProvider_Add(provider, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    1, stackPayload, sizeof(stackPayload), false, NULL));
    memset(stackPayload, 0x00, sizeof(stackPayload));
    assert(Gen3ResourceProvider_Finalize(provider, NULL));
    {
        size_t entryIndex;
        const struct Gen3ProviderEntryOwned *entry =
            Gen3ResourceProvider_FindEntry(provider, NameAlpha, &entryIndex);
        assert(entry != NULL);
        assert(entry->payload[0] == 0x40);
    }

    /* Catalogs copy canonical names (literals here, but owned either way). */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, "gen3:owned/name", GEN3_RESOURCE_TYPE_BINARY, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Finalize(catalog, NULL));

    /* Snapshots own COMPLETE copies: destroying the candidate leaves the
     * published snapshot fully usable. */
    DiagInit(&diag);
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    assert(Gen3ResourceCandidate_AddProvider(candidate, provider, &diag));
    assert(Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidate);
    Gen3ResourceProvider_Destroy(provider);   /* candidate/provider already copied */

    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_OK);
    assert(view.payloadSize == sizeof(stackPayload));
    assert(view.payload[0] == 0x40);
    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("8: deep-copy ownership; snapshot survives source destruction ok\n");
}

/* ------------------------------------------------------------------ */
/* 9. The generic resource view exposes stable descriptive fields      */
/* ------------------------------------------------------------------ */
static void TestResourceView(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceView view;
    struct Gen3ResourceView view2;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[1];
    const struct Gen3ResourceContract *contract;

    providers[0] = base;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 1, &diag);
    DiagDestroy(&diag);
    contract = Gen3ResourceCatalog_Find(catalog, NamePal);
    assert(contract != NULL);
    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NamePal, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_PALETTE, 1, &view) == GEN3_RESOURCE_OK);

    assert(view.handle == contract->handle);
    assert(Gen3ResourceId_KeyEqual(&view.key, &contract->key));
    assert(strcmp(view.canonicalName, NamePal) == 0);
    assert(view.type == GEN3_RESOURCE_TYPE_PALETTE);
    assert(view.schema == 1);
    assert(view.payloadSize == sizeof(PayloadPal));
    assert(memcmp(view.payload, PayloadPal, sizeof(PayloadPal)) == 0);
    assert(strcmp(view.winningProviderId, "base") == 0);
    assert(strcmp(view.winningProviderVersion, "1.0.0") == 0);
    assert(view.winningProviderPrecedence == 10);

    /* The view BORROWS from the snapshot (no per-call copy): two resolves give
     * the same stable payload address, valid until the snapshot is destroyed. */
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_PALETTE, 1, &view2) == GEN3_RESOURCE_OK);
    assert(view2.payload == view.payload);
    assert(view2.payloadSize == view.payloadSize);

    /* Wrong expected type / schema / handle are distinct results. */
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, &view) == GEN3_RESOURCE_WRONG_TYPE);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, handle, GEN3_RESOURCE_TYPE_PALETTE, 2, &view) == GEN3_RESOURCE_WRONG_SCHEMA);
    assert(Gen3ResourceSnapshot_Resolve(snapshot, (Gen3ResourceHandle)9999, GEN3_RESOURCE_TYPE_PALETTE, 1, &view) == GEN3_RESOURCE_INVALID_HANDLE);

    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("9: resource view exposes stable descriptive/runtime fields only ok\n");
}

/* ------------------------------------------------------------------ */
/* Typed accessors + payload validity gates                            */
/* ------------------------------------------------------------------ */
static void TestTypedAccessorsAndPayloads(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "base", "1.0.0");
    struct Gen3ResourceProvider *badTile = NULL;
    struct Gen3ResourceProvider *badPal = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3TileGraphicsView tg;
    struct Gen3PaletteView pal;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[1];

    providers[0] = base;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 1, &diag);
    DiagDestroy(&diag);

    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_GetTileGraphics(snapshot, handle, 1, &tg) == GEN3_RESOURCE_OK);
    assert(tg.size == sizeof(Payload64));
    assert(memcmp(tg.bytes, Payload64, sizeof(Payload64)) == 0);

    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NamePal, &handle) == GEN3_RESOURCE_OK);
    assert(Gen3ResourceSnapshot_GetPalette(snapshot, handle, 1, &pal) == GEN3_RESOURCE_OK);
    assert(pal.size == sizeof(PayloadPal));
    assert(pal.colorCount == sizeof(PayloadPal) / 2u);

    /* Wrong-typed accessor: requesting tile graphics for a palette contract. */
    assert(Gen3ResourceSnapshot_GetTileGraphics(snapshot, handle, 1, &tg) == GEN3_RESOURCE_WRONG_TYPE);
    /* Wrong schema: same type but a schema the contract does not declare. */
    {
        Gen3ResourceHandle tileHandle;
        assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &tileHandle) == GEN3_RESOURCE_OK);
        assert(Gen3ResourceSnapshot_GetTileGraphics(snapshot, tileHandle, 2, &tg) == GEN3_RESOURCE_WRONG_SCHEMA);
    }
    Gen3ResourceSnapshot_Destroy(snapshot);

    /* Invalid payload sizes are rejected at Build (INVALID_PAYLOAD). */
    meta.id = "bad-tile";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    badTile = Gen3ResourceProvider_Create(&meta);
    assert(badTile != NULL);
    assert(Gen3ResourceProvider_Add(badTile, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    1, Payload10, sizeof(Payload10), false, NULL));
    assert(Gen3ResourceProvider_Finalize(badTile, NULL));

    meta.id = "bad-pal";
    meta.precedence = 30;
    badPal = Gen3ResourceProvider_Create(&meta);
    assert(badPal != NULL);
    assert(Gen3ResourceProvider_Add(badPal, NamePal, GEN3_RESOURCE_TYPE_PALETTE,
                                    1, Payload600, sizeof(Payload600), false, NULL));
    assert(Gen3ResourceProvider_Finalize(badPal, NULL));

    {
        struct Gen3ResourceCandidate *candidate;
        struct Gen3ResourceSnapshot *snap = NULL;
        struct Gen3ResourceProvider *all[3];
        all[0] = base;
        all[1] = badTile;
        all[2] = badPal;
        DiagInit(&diag);
        candidate = Gen3ResourceCandidate_Create(catalog, &diag);
        assert(candidate != NULL);
        assert(Gen3ResourceCandidate_AddProvider(candidate, all[0], &diag));
        assert(Gen3ResourceCandidate_AddProvider(candidate, all[1], &diag));
        assert(Gen3ResourceCandidate_AddProvider(candidate, all[2], &diag));
        assert(Gen3ResourceCandidate_Build(candidate, &snap, &diag));
        assert(snap != NULL);
        assert(DiagCountFor(&diag, GEN3_RESOURCE_REASON_INVALID_PAYLOAD) == 2);
        DiagDestroy(&diag);
        Gen3ResourceCandidate_Destroy(candidate);
        Gen3ResourceSnapshot_Destroy(snap);
    }

    Gen3ResourceProvider_Destroy(badPal);
    Gen3ResourceProvider_Destroy(badTile);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("typed accessors + payload validity gates (tile %%32 / palette <=512) ok\n");
}

/* ------------------------------------------------------------------ */
/* 15. Resolution traces are deterministic (exact golden sequence)     */
/* ------------------------------------------------------------------ */
static void TestTraceDeterminism(void)
{
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *base = MakeBaseProvider(10, "zz-base", "1.0.0");
    struct Gen3ResourceProvider *mid = MakeBaseProvider(20, "aa-mid", "2.0.0");
    struct Gen3ResourceProvider *top = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceSnapshot *snapshot;
    struct Gen3ResourceTrace trace;
    Gen3ResourceHandle handle;
    struct Gen3ResourceProvider *providers[3];

    /* top: highest precedence but INVALID (optional schema mismatch). */
    meta.id = "mm-top";
    meta.version = "3.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 30;
    top = Gen3ResourceProvider_Create(&meta);
    assert(top != NULL);
    assert(Gen3ResourceProvider_Add(top, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                    2, Payload64, sizeof(Payload64), false, NULL));
    assert(Gen3ResourceProvider_Finalize(top, NULL));

    providers[0] = base;
    providers[1] = mid;
    providers[2] = top;
    DiagInit(&diag);
    snapshot = BuildCandidateSnapshot(catalog, providers, 3, &diag);
    DiagDestroy(&diag);
    assert(snapshot != NULL);

    assert(Gen3ResourceSnapshot_FindHandle(snapshot, NameAlpha, &handle) == GEN3_RESOURCE_OK);
    Gen3ResourceTrace_Init(&trace);
    assert(Gen3ResourceSnapshot_Trace(snapshot, handle, &trace));

    /* Golden: descending precedence, no pointers/timestamps in the records. */
    assert(trace.count == 3);
    assert(strcmp(trace.items[0].providerId, "mm-top") == 0);
    assert(trace.items[0].precedence == 30);
    assert(trace.items[0].disposition == GEN3_TRACE_REJECTED);
    assert(trace.items[0].reason == GEN3_RESOURCE_REASON_SCHEMA_MISMATCH);
    assert(strcmp(trace.items[1].providerId, "aa-mid") == 0);
    assert(trace.items[1].precedence == 20);
    assert(trace.items[1].disposition == GEN3_TRACE_WINNER);
    assert(trace.items[1].reason == GEN3_RESOURCE_REASON_NONE);
    assert(strcmp(trace.items[2].providerId, "zz-base") == 0);
    assert(trace.items[2].precedence == 10);
    assert(trace.items[2].disposition == GEN3_TRACE_FALLBACK);
    assert(trace.items[2].reason == GEN3_RESOURCE_REASON_NONE);
    Gen3ResourceTrace_Destroy(&trace);

    /* Re-running the trace yields the identical sequence. */
    Gen3ResourceTrace_Init(&trace);
    assert(Gen3ResourceSnapshot_Trace(snapshot, handle, &trace));
    assert(trace.count == 3);
    assert(strcmp(trace.items[0].providerId, "mm-top") == 0);
    assert(strcmp(trace.items[1].providerId, "aa-mid") == 0);
    assert(strcmp(trace.items[2].providerId, "zz-base") == 0);
    Gen3ResourceTrace_Destroy(&trace);

    Gen3ResourceSnapshot_Destroy(snapshot);
    Gen3ResourceProvider_Destroy(top);
    Gen3ResourceProvider_Destroy(mid);
    Gen3ResourceProvider_Destroy(base);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("15: deterministic trace (descending precedence, golden sequence) ok\n");
}

/* ------------------------------------------------------------------ */
/* 14. Diagnostics are structured reason enums; strings are derived    */
/* ------------------------------------------------------------------ */
static void TestDiagnosticsStructured(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceProvider *bad = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceDiagnosticList direct;
    enum Gen3ResourceReason reason;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;
    struct Gen3ResourceProvider *providers[1];

    /* Every reason enum has a non-empty human description, but tests assert the
     * ENUM, never the string. */
    for (reason = GEN3_RESOURCE_REASON_NONE;
         reason <= GEN3_RESOURCE_REASON_RESOURCE_NOT_RESOLVED;
         reason++)
    {
        const char *description = Gen3ResourceReason_Describe(reason);
        assert(description != NULL);
        assert(description[0] != '\0');
        assert(strcmp(description, "unknown resource diagnostic") != 0);
    }

    /* A forced entry failure carries the full structured payload. Catalog has
     * no required-for-base resources so the only diagnostic is the entry. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS, 1, false, NULL));
    assert(Gen3ResourceCatalog_Finalize(catalog, NULL));

    meta.id = "bad";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 20;
    bad = Gen3ResourceProvider_Create(&meta);
    assert(bad != NULL);
    assert(Gen3ResourceProvider_Add(bad, NameAlpha, GEN3_RESOURCE_TYPE_PALETTE,
                                    1, PayloadPal, sizeof(PayloadPal), true, NULL));
    assert(Gen3ResourceProvider_Finalize(bad, NULL));

    providers[0] = bad;
    DiagInit(&diag);
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    assert(Gen3ResourceCandidate_AddProvider(candidate, providers[0], &diag));
    assert(!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    assert(diag.count == 1);
    assert(diag.items[0].severity == GEN3_DIAGNOSTIC_ERROR);
    assert(diag.items[0].reason == GEN3_RESOURCE_REASON_TYPE_MISMATCH);
    assert(strcmp(diag.items[0].resourceName, NameAlpha) == 0);
    assert(strcmp(diag.items[0].providerId, "bad") == 0);
    assert(diag.items[0].expectedType == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
    assert(diag.items[0].actualType == GEN3_RESOURCE_TYPE_PALETTE);
    assert(diag.items[0].expectedSchema == 1);
    assert(diag.items[0].actualSchema == 1);
    assert(diag.items[0].providerEntryRequired == true);
    assert(diag.items[0].catalogRequiredForBase == false);
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidate);

    /* The list owns its records: destroying the caller's source names is safe. */
    Gen3ResourceDiagnostics_Init(&direct);
    assert(Gen3ResourceDiagnostics_Append(&direct, GEN3_DIAGNOSTIC_WARNING,
        GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE, "gen3:ephemeral", "prov",
        GEN3_RESOURCE_TYPE_INVALID, GEN3_RESOURCE_TYPE_INVALID, 0, 0, false, false));
    assert(direct.count == 1);
    assert(direct.items[0].reason == GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE);
    assert(strcmp(direct.items[0].resourceName, "gen3:ephemeral") == 0);
    Gen3ResourceDiagnostics_Destroy(&direct);

    Gen3ResourceProvider_Destroy(bad);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("14: structured reason enums; strings derived only for humans ok\n");
}

/* ------------------------------------------------------------------ */
/* Provider/candidate boundary checks                                  */
/* ------------------------------------------------------------------ */
static void TestProviderAndCandidateBoundaries(void)
{
    struct Gen3ResourceCatalog *unfinalized = Gen3ResourceCatalog_Create();
    struct Gen3ResourceCatalog *catalog = MakeCatalog();
    struct Gen3ResourceProvider *unfinalizedProvider = NULL;
    struct Gen3ResourceProviderMetadata meta;
    struct Gen3ResourceDiagnosticList diag;
    struct Gen3ResourceCandidate *candidate;
    struct Gen3ResourceSnapshot *snapshot = NULL;

    assert(unfinalized != NULL);
    DiagInit(&diag);
    assert(Gen3ResourceCandidate_Create(unfinalized, &diag) == NULL);
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_CATALOG_NOT_FINALIZED));
    DiagDestroy(&diag);
    Gen3ResourceCatalog_Destroy(unfinalized);

    meta.id = "unfinalized-provider";
    meta.version = "1.0.0";
    meta.kind = GEN3_PROVIDER_SYNTHETIC;
    meta.precedence = 10;
    unfinalizedProvider = Gen3ResourceProvider_Create(&meta);
    assert(unfinalizedProvider != NULL);
    /* Not finalized -> cannot join a candidate. */
    DiagInit(&diag);
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    assert(!Gen3ResourceCandidate_AddProvider(candidate, unfinalizedProvider, &diag));
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_PROVIDER_NOT_FINALIZED));
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidate);

    /* Provider with duplicate entries cannot finalize. */
    {
        struct Gen3ResourceProvider *dup = NULL;
        meta.id = "dup-provider";
        dup = Gen3ResourceProvider_Create(&meta);
        assert(dup != NULL);
        assert(Gen3ResourceProvider_Add(dup, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                        1, Payload64, sizeof(Payload64), false, NULL));
        assert(Gen3ResourceProvider_Add(dup, NameAlpha, GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
                                        1, Payload64, sizeof(Payload64), false, NULL));
        DiagInit(&diag);
        assert(!Gen3ResourceProvider_Finalize(dup, &diag));
        assert(DiagHas(&diag, GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_ENTRY));
        DiagDestroy(&diag);
        Gen3ResourceProvider_Destroy(dup);
    }

    /* Required-for-base with NO provider at all -> MISSING_REQUIRED_BASE. */
    DiagInit(&diag);
    candidate = Gen3ResourceCandidate_Create(catalog, &diag);
    assert(candidate != NULL);
    assert(!Gen3ResourceCandidate_Build(candidate, &snapshot, &diag));
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE));
    assert(snapshot == NULL);
    DiagDestroy(&diag);
    Gen3ResourceCandidate_Destroy(candidate);

    Gen3ResourceProvider_Destroy(unfinalizedProvider);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("provider/candidate boundary checks (finalize, duplicates, no-base) ok\n");
}

/* ------------------------------------------------------------------ */
/* R12-A: audio instrument-bank type vocabulary (append-only)          */
/* ------------------------------------------------------------------ */
static void TestR12AudioTypeVocabulary(void)
{
    struct Gen3ResourceCatalog *catalog;
    struct Gen3ResourceDiagnosticList diag;
    const struct Gen3ResourceContract *contract;

    /* The new type names exactly; every pre-existing audio-adjacent type is
     * untouched (the enum is append-only: nothing renumbered). */
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_INSTRUMENT_BANK),
                  "instrument-bank") == 0);
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_AUDIO_SAMPLE),
                  "audio-sample") == 0);
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE),
                  "music-sequence") == 0);
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_SOUND_EFFECT),
                  "sound-effect") == 0);
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_CRY), "cry") == 0);
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_BINARY), "binary") == 0);
    /* Out of range (COUNT and beyond) is "invalid": never a crash, never an
     * alias to a real type. */
    assert(strcmp(Gen3ResourceType_Name(GEN3_RESOURCE_TYPE_COUNT), "invalid") == 0);
    assert(strcmp(Gen3ResourceType_Name((enum Gen3ResourceType)9999), "invalid") == 0);

    /* The catalog accepts instrument-bank contracts: schema 1 (voicegroups,
     * cry tables) and schema 2 (keysplit runs) per the approved R12 §2. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    assert(Gen3ResourceCatalog_Add(catalog, "emerald:audio/voicegroup/rs-drumset",
                                   GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 1, false, NULL));
    assert(Gen3ResourceCatalog_Add(catalog, "emerald:audio/keysplit/piano",
                                   GEN3_RESOURCE_TYPE_INSTRUMENT_BANK, 2, false, NULL));
    assert(Gen3ResourceCatalog_Finalize(catalog, NULL));
    contract = Gen3ResourceCatalog_Find(catalog, "emerald:audio/voicegroup/rs-drumset");
    assert(contract != NULL);
    assert(contract->type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
    assert(contract->schema == 1);
    contract = Gen3ResourceCatalog_Find(catalog, "emerald:audio/keysplit/piano");
    assert(contract != NULL);
    assert(contract->type == GEN3_RESOURCE_TYPE_INSTRUMENT_BANK);
    assert(contract->schema == 2);
    Gen3ResourceCatalog_Destroy(catalog);

    /* COUNT is not a storable type: catalog insertion is refused with the
     * structured INVALID_ARGUMENT diagnostic, never accepted silently. */
    catalog = Gen3ResourceCatalog_Create();
    assert(catalog != NULL);
    DiagInit(&diag);
    assert(!Gen3ResourceCatalog_Add(catalog, "emerald:audio/song/one",
                                    GEN3_RESOURCE_TYPE_COUNT, 1, false, &diag));
    assert(DiagHas(&diag, GEN3_RESOURCE_REASON_INVALID_ARGUMENT));
    DiagDestroy(&diag);
    Gen3ResourceCatalog_Destroy(catalog);
    printf("R12-A: instrument-bank vocabulary (names, catalog, COUNT rejection) ok\n");
}

/* ------------------------------------------------------------------ */
/* Version constants                                                   */
/* ------------------------------------------------------------------ */
static void TestVersionConstants(void)
{
    assert(RESOURCE_API_VERSION_MAJOR == 1);
    assert(RESOURCE_API_VERSION_MINOR == 0);
    assert(RESOURCE_API_VERSION_PATCH == 0);
    assert(MOD_MANIFEST_VERSION == 1);
    assert(MOD_PACK_VERSION == 1);
    assert(RESOURCE_PACK_FORMAT_VERSION == 1);
    printf("version constants (API 1.0.0; manifest/pack 1) ok\n");
}

int main(void)
{
    InitPayloads();
    TestSha256StandardVectors();
    TestKeyDerivationFixedVectors();
    TestCanonicalNameValidation();
    TestKeyRepresentation();
    TestCollisionSeam();
    TestCatalogOrderIndependence();
    TestProviderPrecedenceExplicit();
    TestTypeSchemaDistinct();
    TestOptionalOverrideFallback();
    TestRequiredOverrideFailure();
    TestRequiredBaseResource();
    TestTransactionalPublication();
    TestOwnershipLifetime();
    TestResourceView();
    TestTypedAccessorsAndPayloads();
    TestTraceDeterminism();
    TestDiagnosticsStructured();
    TestProviderAndCandidateBoundaries();
    TestR12AudioTypeVocabulary();
    TestVersionConstants();
    printf("gen3 resource core test passed\n");
    return 0;
}
