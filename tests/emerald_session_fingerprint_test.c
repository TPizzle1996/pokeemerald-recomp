/* R10-F: session content fingerprint tests.
 *
 * Pins the exact construction: deterministic across calls, sensitive to
 * provider content, provider identity and provider ORDER (ascending
 * precedence is part of the construction - TEST 6), insensitive to paths
 * (the construction has no path input at all), and strict about arguments.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "emerald/resources/emerald_resource_session.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/sha256.h"

static int sFailures = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            sFailures++;                                                \
        }                                                               \
    } while (0)

static void PrintDigestHex(const uint8_t digest[GEN3_PACK_SHA256_SIZE])
{
    size_t i;
    for (i = 0; i < GEN3_PACK_SHA256_SIZE; i++)
        printf("%02x", digest[i]);
}

/* The construction must be byte-exact across machines, compilers and
 * platforms: pinned vectors computed once from the specification. */
static bool DigestEqualsHex(const uint8_t digest[GEN3_PACK_SHA256_SIZE],
                            const char *hex)
{
    static const char nibbles[] = "0123456789abcdef";
    size_t i;

    if (strlen(hex) != GEN3_PACK_SHA256_SIZE * 2u)
        return false;
    for (i = 0; i < GEN3_PACK_SHA256_SIZE; i++)
    {
        char high = hex[i * 2u];
        char low = hex[i * 2u + 1u];
        const char *highPos = strchr(nibbles, high);
        const char *lowPos = strchr(nibbles, low);
        uint8_t expected;
        if (highPos == NULL || lowPos == NULL)
            return false;
        expected = (uint8_t)(((size_t)(highPos - nibbles) << 4)
                           | (size_t)(lowPos - nibbles));
        if (digest[i] != expected)
            return false;
    }
    return true;
}

/* Fixed synthetic digests: content-derived values stand in for the provider
 * logical digests; the construction must not care what produced them. */
static uint8_t sDigestA[GEN3_PACK_SHA256_SIZE];
static uint8_t sDigestB[GEN3_PACK_SHA256_SIZE];

static void FillDigests(void)
{
    size_t i;
    for (i = 0; i < GEN3_PACK_SHA256_SIZE; i++)
    {
        sDigestA[i] = (uint8_t)(i * 7u + 1u);
        sDigestB[i] = (uint8_t)(i * 13u + 5u);
    }
}

int main(void)
{
    struct EmeraldResourceFingerprintProvider providers[2];
    uint8_t single[GEN3_PACK_SHA256_SIZE];
    uint8_t singleAgain[GEN3_PACK_SHA256_SIZE];
    uint8_t two[GEN3_PACK_SHA256_SIZE];
    uint8_t twoReordered[GEN3_PACK_SHA256_SIZE];
    uint8_t twoChangedDigest[GEN3_PACK_SHA256_SIZE];
    uint8_t twoDifferentGameId[GEN3_PACK_SHA256_SIZE];
    struct EmeraldResourceSessionInfo info;

    FillDigests();

    /* Argument validation. */
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint(NULL, NULL, 0, single));
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint("bpee01", NULL, 0, NULL));
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint("bpee01", NULL, 1, single));
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint(
              "bpee01", providers, EMERALD_RESOURCE_SESSION_MAX_FINGERPRINT_PROVIDERS + 1u,
              single));
    providers[0].providerId = "emerald.rom-base.bpee01";
    providers[0].providerVersion = "v1";
    providers[0].kind = 2u; /* GEN3_PROVIDER_ROM_BASE */
    providers[0].precedence = 300u;
    providers[0].logicalContentDigest = sDigestA;
    CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpee01", providers, 1u, single));
    /* Strictly ascending precedence. */
    providers[0].providerId = NULL;
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint("bpee01", providers, 1u, single));
    providers[0].providerId = "emerald.rom-base.bpee01";
    providers[0].providerVersion = NULL;
    CHECK(!EmeraldResourceSession_ComputeContentFingerprint("bpee01", providers, 1u, single));
    providers[0].providerVersion = "v1";

    /* Determinism: identical input, identical digest. */
    CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpee01", providers, 1u, singleAgain));
    CHECK(memcmp(single, singleAgain, sizeof(single)) == 0);

    /* Two providers in ascending precedence. */
    providers[1].providerId = "emerald.mod.content";
    providers[1].providerVersion = "v7";
    providers[1].kind = 1u;
    providers[1].precedence = 500u;
    providers[1].logicalContentDigest = sDigestB;
    CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpee01", providers, 2u, two));

    /* TEST 6: provider ordering/precedence is meaningful - swapping the
     * records must change the digest. */
    {
        struct EmeraldResourceFingerprintProvider swapped[2];
        swapped[0] = providers[1];
        swapped[1] = providers[0];
        /* swapped is NOT ascending, so it is rejected outright. */
        CHECK(!EmeraldResourceSession_ComputeContentFingerprint("bpee01", swapped, 2u, twoReordered));
        /* The legitimate reorder: the mod provider takes the base slot. */
        swapped[0] = providers[1];
        swapped[1] = providers[0];
        swapped[0].precedence = 100u;
        swapped[1].precedence = 300u;
        CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpee01", swapped, 2u, twoReordered));
        CHECK(memcmp(two, twoReordered, sizeof(two)) != 0);
    }

    /* Changed provider content must not match. */
    {
        struct EmeraldResourceFingerprintProvider changed[2];
        changed[0] = providers[0];
        changed[1] = providers[1];
        changed[1].logicalContentDigest = sDigestA; /* content swapped */
        CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpee01", changed, 2u, twoChangedDigest));
        CHECK(memcmp(two, twoChangedDigest, sizeof(two)) != 0);
    }

    /* Game id is part of the construction. */
    CHECK(EmeraldResourceSession_ComputeContentFingerprint("bpge01", providers, 2u, twoDifferentGameId));
    CHECK(memcmp(two, twoDifferentGameId, sizeof(two)) != 0);

    /* Convenience form mirrors the single-provider construction. */
    memset(&info, 0, sizeof(info));
    snprintf(info.providerId, sizeof(info.providerId), "%s", "emerald.rom-base.bpee01");
    snprintf(info.providerVersion, sizeof(info.providerVersion), "%s", "v1");
    info.kind = 2u;
    info.precedence = 300u;
    snprintf(info.gameId, sizeof(info.gameId), "%s", "bpee01");
    memcpy(info.logicalContentDigest, sDigestA, sizeof(sDigestA));
    info.hasLogicalContentDigest = true;
    {
        uint8_t fromInfo[GEN3_PACK_SHA256_SIZE];
        CHECK(EmeraldResourceSession_ComputeBaseFingerprint(&info, fromInfo));
        CHECK(memcmp(single, fromInfo, sizeof(single)) == 0);
        CHECK(!EmeraldResourceSession_ComputeBaseFingerprint(NULL, fromInfo));
        CHECK(!EmeraldResourceSession_ComputeBaseFingerprint(&info, NULL));
    }

    /* Pinned vectors: byte-exact across machines. */
    CHECK(DigestEqualsHex(single,
        "ad90961852d7961dbc5059ced2b72dcf84cc67d942a860c168b0cb8ba667ddd8"));
    CHECK(DigestEqualsHex(two,
        "18ba86bbc6326ee7090048e87b5d8cb4a29aedec01a0216aee6827467d72b903"));

    printf("single-provider digest: ");
    PrintDigestHex(single);
    printf("\ntwo-provider digest: ");
    PrintDigestHex(two);
    printf("\n");

    if (sFailures != 0)
    {
        fprintf(stderr, "emerald_session_fingerprint_test: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("emerald_session_fingerprint_test: all checks passed\n");
    return 0;
}
