/* Stage R5: deterministic literal-only GBA LZ77 encoder + compatibility image
 * unit tests (R5 §19-§24), updated for the R7B family image API and the R8
 * RAW-entry encoding.
 *
 * This is the platform-neutral suite: it exercises the reusable gen3 codec
 * (resource_lz) and the Emerald compatibility image builder
 * (emerald_resource_compat) in isolation - no global.h, no sprite tables, no
 * native target. The GBA LZ77 decode side is a faithful host-side port of the
 * real Emerald decompressor (src/platform/bios.c LZ77UnCompWram); the
 * AUTHORITATIVE real-decompressor round-trip and the real native load path are
 * covered by emerald_trainer_native_compat_test.c against the real bios.c.
 *
 * Coverage:
 *   - §3/§19  codec contract: deterministic literal-only encoding, exact
 *             header/group structure, padding, buffer/overflow guards.
 *   - §3      decompress(encoded) == payload for boundary sizes via a faithful
 *             port of the GBA decoder (cross-checked against the real one in
 *             the native suite).
 *   - R7B §8  family image build fails closed unless every entry's payload
 *             size matches the type-derived (2048 sheet / 32 palette) or
 *             per-entry override; diagnostics carry the first mismatch.
 *   - §2/§21 image getters: streams + canonical copies, single allocation,
 *             survive destruction of the source payloads (independence).
 *   - R8      RAW entries (back sheets): the stream IS the canonical payload
 *             verbatim; expectedSize overrides serve non-2048 sheets.
 *   - §13/§24 transactional construction: failure leaves *outImage NULL and
 *             nothing allocated.
 *
 * No user ROM, no .lz files, no frontend includes (guardrail 18).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen3/resources/resource_lz.h"
#include "emerald/resources/emerald_resource_compat.h"

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(label, cond)                                                   \
    do                                                                       \
    {                                                                        \
        gChecks++;                                                           \
        if (!(cond))                                                         \
        {                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (label));         \
            gFailures++;                                                     \
        }                                                                    \
    } while (0)

/* Faithful host port of the real GBA LZ77 decoder (bios.c LZ77UnCompWram).
 * Byte-sourced, terminates on the decoded-size counter; the final literal-only
 * group's padding bytes are consumed but never written. */
static size_t RefLz77Decode(const uint8_t *src, size_t srcSize,
                            uint8_t *dst, size_t dstCapacity)
{
    uint32_t header;
    size_t srcPos = 0;
    size_t dstPos = 0;
    uint32_t len;

    if (srcSize < 4u)
        return 0u;
    memcpy(&header, src, sizeof(header));
    srcPos = 4u;
    len = header >> 8;

    while (len > 0u)
    {
        uint8_t d;
        int slot;

        if (srcPos >= srcSize)
            return dstPos;
        d = src[srcPos++];
        if (d != 0u)
        {
            for (slot = 0; slot < 8; slot++)
            {
                if (d & 0x80u)
                {
                    uint16_t data = (uint16_t)((uint16_t)src[srcPos++] << 8);
                    data |= src[srcPos++];
                    {
                        int length = (int)(data >> 12) + 3;
                        int offset = (int)(data & 0x0FFFu);
                        size_t windowOffset = dstPos - (size_t)offset - 1u;
                        int i2;
                        for (i2 = 0; i2 < length; i2++)
                        {
                            if (dstPos >= dstCapacity || windowOffset >= dstPos)
                                return dstPos;
                            dst[dstPos++] = dst[windowOffset++];
                            len--;
                            if (len == 0u)
                                return dstPos;
                        }
                    }
                }
                else
                {
                    if (dstPos >= dstCapacity || srcPos >= srcSize)
                        return dstPos;
                    dst[dstPos++] = src[srcPos++];
                    len--;
                    if (len == 0u)
                        return dstPos;
                }
                d <<= 1;
            }
        }
        else
        {
            for (slot = 0; slot < 8; slot++)
            {
                if (dstPos >= dstCapacity || srcPos >= srcSize)
                    return dstPos;
                dst[dstPos++] = src[srcPos++];
                len--;
                if (len == 0u)
                    return dstPos;
            }
        }
    }
    return dstPos;
}

static void FillPattern(uint8_t *buf, size_t size, unsigned seed)
{
    size_t i;
    for (i = 0; i < size; i++)
        buf[i] = (uint8_t)((i * seed + (i >> 3u)) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Codec: encoded-size math (§3)                                       */
/* ------------------------------------------------------------------ */

static void TestEncodedSize(void)
{
    CHECK("size 0 rejected", Gen3LzLiteral_EncodedSize(0u) == SIZE_MAX);
    CHECK("size 1 -> 13", Gen3LzLiteral_EncodedSize(1u) == 13u);
    CHECK("size 7 -> 13", Gen3LzLiteral_EncodedSize(7u) == 13u);
    CHECK("size 8 -> 13", Gen3LzLiteral_EncodedSize(8u) == 13u);
    CHECK("size 9 -> 22", Gen3LzLiteral_EncodedSize(9u) == 22u);
    CHECK("size 16 -> 22", Gen3LzLiteral_EncodedSize(16u) == 22u);
    CHECK("size 32 -> 40", Gen3LzLiteral_EncodedSize(32u) == 40u);
    CHECK("size 33 -> 49", Gen3LzLiteral_EncodedSize(33u) == 49u);
    CHECK("size 255 -> 292", Gen3LzLiteral_EncodedSize(255u) == 292u);
    CHECK("size 256 -> 292", Gen3LzLiteral_EncodedSize(256u) == 292u);
    CHECK("size 2048 -> 2308", Gen3LzLiteral_EncodedSize(2048u) == 2308u);
    CHECK("size 0xFFFFFF fits",
          Gen3LzLiteral_EncodedSize(0xFFFFFFu)
              == 4u + ((0xFFFFFFu + 7u) / 8u) * 9u);
    CHECK("size 0x1000000 overflow", Gen3LzLiteral_EncodedSize(0x1000000u) == SIZE_MAX);
}

/* ------------------------------------------------------------------ */
/* Codec: exact byte structure (§3)                                    */
/* ------------------------------------------------------------------ */

static void TestEncodeStructure(void)
{
    uint8_t payload[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    uint8_t out[13];
    size_t encoded = 0;
    enum Gen3LzResult r;

    memset(out, 0xA5, sizeof(out));
    r = Gen3LzLiteral_Encode(payload, 8u, out, sizeof(out), &encoded);
    CHECK("encode 8 ok", r == GEN3_LZ_OK);
    CHECK("encoded size", encoded == 13u);
    CHECK("type byte", out[0] == 0x10u);
    CHECK("size lo", out[1] == 0x08u);
    CHECK("size mid", out[2] == 0x00u);
    CHECK("size hi", out[3] == 0x00u);
    CHECK("flag byte literal", out[4] == 0x00u);
    CHECK("literal 0", out[5] == 0x11u);
    CHECK("literal 7", out[12] == 0x88u);
    CHECK("no padding written for exact group", out[12] == 0x88u);

    /* Partial final group pads to 8 with zeroes. */
    {
        uint8_t small[1] = {0xAB};
        uint8_t out2[13];
        r = Gen3LzLiteral_Encode(small, 1u, out2, sizeof(out2), NULL);
        CHECK("encode 1 ok", r == GEN3_LZ_OK);
        CHECK("partial group literal", out2[5] == 0xAB);
        CHECK("partial group zero pad", out2[6] == 0x00u && out2[12] == 0x00u);
    }

    /* Buffer too small: nothing written (capacity checked up front). */
    {
        uint8_t out3[13];
        memset(out3, 0x5A, sizeof(out3));
        r = Gen3LzLiteral_Encode(payload, 8u, out3, 12u, &encoded);
        CHECK("too-small rejected", r == GEN3_LZ_ERR_BUFFER_TOO_SMALL);
        CHECK("too-small wrote nothing", out3[0] == 0x5Au && out3[12] == 0x5Au);
    }

    CHECK("null payload", Gen3LzLiteral_Encode(NULL, 8u, out, sizeof(out), NULL)
          == GEN3_LZ_ERR_INVALID_ARGUMENT);
    CHECK("null out", Gen3LzLiteral_Encode(payload, 8u, NULL, 0u, NULL)
          == GEN3_LZ_ERR_INVALID_ARGUMENT);
    CHECK("zero size", Gen3LzLiteral_Encode(payload, 0u, out, sizeof(out), NULL)
          == GEN3_LZ_ERR_INVALID_ARGUMENT);
    /* 0x1000000 exceeds the 24-bit decoded-size field: the encoder rejects it
     * up front (INVALID_ARGUMENT); the SIZE_OVERFLOW path is the defensive
     * arithmetic guard that remains for the (unreachable on 64-bit size_t)
     * product-overflow case. */
    CHECK("huge size rejected",
          Gen3LzLiteral_Encode(payload, 0x1000000u, out, 0x1000000u, NULL)
              == GEN3_LZ_ERR_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------ */
/* Codec: round-trip through the faithful decoder (§3)                 */
/* ------------------------------------------------------------------ */

static void RoundTripSize(size_t size, unsigned seed)
{
    uint8_t *payload = malloc(size);
    size_t encodedSize = Gen3LzLiteral_EncodedSize((uint32_t)size);
    uint8_t *stream = malloc(encodedSize);
    uint8_t *decoded = malloc(size + 16u);
    enum Gen3LzResult r;
    size_t got;
    char label[64];

    FillPattern(payload, size, seed);
    r = Gen3LzLiteral_Encode(payload, (uint32_t)size, stream, encodedSize, NULL);
    got = RefLz77Decode(stream, encodedSize, decoded, size + 16u);
    snprintf(label, sizeof(label), "roundtrip %zu decodes to %zu", size, got);
    CHECK(label, r == GEN3_LZ_OK && got == size);
    snprintf(label, sizeof(label), "roundtrip %zu matches payload", size);
    CHECK(label, got == size && memcmp(decoded, payload, size) == 0);

    free(payload);
    free(stream);
    free(decoded);
}

static void TestRoundTrips(void)
{
    static const size_t sizes[] = {
        1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u,
        63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 1024u, 2048u,
    };
    size_t i;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        RoundTripSize(sizes[i], (unsigned)(3u + i));
}

/* ------------------------------------------------------------------ */
/* Compatibility image (§12/§2/§13/§20/§21)                            */
/* ------------------------------------------------------------------ */

static void FillFamilyEntries(struct EmeraldResourceCompatSourceEntry *entries,
                              const uint8_t *sheet, size_t sheetSize,
                              const uint8_t *palette, size_t paletteSize,
                              enum EmeraldResourceCompatEntryEncoding encoding,
                              uint32_t expectedSheet)
{
    entries[0].canonicalName = "emerald:test/trainer/brendan/battle/front/sheet";
    entries[0].type = GEN3_RESOURCE_TYPE_TILE_GRAPHICS;
    entries[0].schema = EMERALD_TRAINER_SCHEMA;
    entries[0].payload = sheet;
    entries[0].payloadSize = (uint32_t)sheetSize;
    entries[0].expectedSize = expectedSheet; /* 0 = derive from type (2048) */
    entries[0].encoding = encoding;
    entries[1].canonicalName = "emerald:test/trainer/brendan/battle/front/palette";
    entries[1].type = GEN3_RESOURCE_TYPE_PALETTE;
    entries[1].schema = EMERALD_TRAINER_SCHEMA;
    entries[1].payload = palette;
    entries[1].payloadSize = (uint32_t)paletteSize;
    entries[1].expectedSize = 0u;
    entries[1].encoding = EMERALD_COMPAT_ENTRY_LZ;
}

static void TestImageBuildAndGetters(void)
{
    uint8_t sheet[2048];
    uint8_t palette[32];
    struct EmeraldResourceCompatSourceEntry entries[2];
    struct EmeraldResourceCompatibilityImage *image = NULL;
    struct EmeraldResourceCompatDiagnostics diag;
    const uint8_t *sheetCanon;
    const uint8_t *sheetStream;
    const uint8_t *paletteCanon;
    const uint8_t *paletteStream;
    uint8_t decoded[2048];
    size_t got;
    enum EmeraldResourceCompatStatus status;

    FillPattern(sheet, sizeof(sheet), 7u);
    FillPattern(palette, sizeof(palette), 3u);
    FillFamilyEntries(entries, sheet, sizeof(sheet), palette, sizeof(palette),
                      EMERALD_COMPAT_ENTRY_LZ, 0u);

    status = EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag);
    CHECK("image create ok", status == EMERALD_COMPAT_OK);
    CHECK("image non-NULL", image != NULL);
    if (image == NULL)
        return;

    CHECK("entry count", EmeraldResourceCompatImage_GetEntryCount(image) == 2u);
    CHECK("entry 0 name",
          strcmp(EmeraldResourceCompatImage_GetEntryName(image, 0u),
                 "emerald:test/trainer/brendan/battle/front/sheet") == 0);
    CHECK("entry 1 name",
          strcmp(EmeraldResourceCompatImage_GetEntryName(image, 1u),
                 "emerald:test/trainer/brendan/battle/front/palette") == 0);
    CHECK("entry 0 type",
          EmeraldResourceCompatImage_GetEntryType(image, 0u)
              == GEN3_RESOURCE_TYPE_TILE_GRAPHICS);
    CHECK("entry 1 type",
          EmeraldResourceCompatImage_GetEntryType(image, 1u)
              == GEN3_RESOURCE_TYPE_PALETTE);
    CHECK("sheet decoded size",
          EmeraldResourceCompatImage_GetDecodedSize(image, 0u) == 2048u);
    CHECK("palette decoded size",
          EmeraldResourceCompatImage_GetDecodedSize(image, 1u) == 32u);
    CHECK("sheet canonical size",
          EmeraldResourceCompatImage_GetCanonicalSize(image, 0u) == 2048u);
    CHECK("sheet stream size",
          EmeraldResourceCompatImage_GetStreamSize(image, 0u) == 2308u);
    CHECK("palette stream size",
          EmeraldResourceCompatImage_GetStreamSize(image, 1u) == 40u);

    sheetCanon = EmeraldResourceCompatImage_GetCanonical(image, 0u);
    paletteCanon = EmeraldResourceCompatImage_GetCanonical(image, 1u);
    sheetStream = EmeraldResourceCompatImage_GetStream(image, 0u);
    paletteStream = EmeraldResourceCompatImage_GetStream(image, 1u);
    CHECK("sheet canonical non-NULL", sheetCanon != NULL);
    CHECK("palette canonical non-NULL", paletteCanon != NULL);
    CHECK("sheet stream non-NULL", sheetStream != NULL);
    CHECK("palette stream non-NULL", paletteStream != NULL);

    if (sheetCanon && paletteCanon && sheetStream && paletteStream)
    {
        CHECK("sheet canonical copy matches",
              memcmp(sheetCanon, sheet, sizeof(sheet)) == 0);
        CHECK("palette canonical copy matches",
              memcmp(paletteCanon, palette, sizeof(palette)) == 0);

        /* Streams decode to the canonical payloads (faithful decoder port;
         * the real bios.c decoder cross-checks in the native suite). */
        got = RefLz77Decode(sheetStream, 2308u, decoded, sizeof(decoded));
        CHECK("sheet stream decodes", got == sizeof(sheet));
        CHECK("sheet stream matches canonical",
              got == sizeof(sheet) && memcmp(decoded, sheet, sizeof(sheet)) == 0);

        got = RefLz77Decode(paletteStream, 40u, decoded, sizeof(decoded));
        CHECK("palette stream decodes", got == sizeof(palette));
        CHECK("palette stream matches canonical",
              got == sizeof(palette) && memcmp(decoded, palette, sizeof(palette)) == 0);

        /* Entries keep the caller's order through the arena (R7B §7). */
        CHECK("canonicals in entry order", sheetCanon < paletteCanon);
        CHECK("streams in entry order", sheetStream < paletteStream);
    }

    /* Getter robustness for a NULL image / out-of-range index. */
    CHECK("getter null image", EmeraldResourceCompatImage_GetStream(NULL, 0u) == NULL);
    CHECK("getter null size", EmeraldResourceCompatImage_GetStreamSize(NULL, 0u) == 0u);
    CHECK("getter null decoded", EmeraldResourceCompatImage_GetDecodedSize(NULL, 0u) == 0u);
    CHECK("getter null name", EmeraldResourceCompatImage_GetEntryName(NULL, 0u) == NULL);
    CHECK("getter null count", EmeraldResourceCompatImage_GetEntryCount(NULL) == 0u);
    CHECK("getter bad index", EmeraldResourceCompatImage_GetStream(image, 2u) == NULL);

    /* Status description is stable for every status value. */
    CHECK("status describe ok", strcmp(EmeraldResourceCompatStatus_Describe(EMERALD_COMPAT_OK), "ok") == 0);
    CHECK("status describe resolve",
          strcmp(EmeraldResourceCompatStatus_Describe(EMERALD_COMPAT_ERR_RESOLVE_FAILED),
                 "M0/M1 resolve failed") == 0);

    EmeraldResourceCompatImage_Destroy(image);
}

/* R8: RAW entries serve the canonical payload bytes verbatim as the stream
 * (back sheets: expectedSize override 8192/10240, stream == payload). */
static void TestImageRawEntry(void)
{
    uint8_t backSheet[8192];
    uint8_t palette[32];
    struct EmeraldResourceCompatSourceEntry entries[2];
    struct EmeraldResourceCompatibilityImage *image = NULL;
    struct EmeraldResourceCompatDiagnostics diag;
    const uint8_t *canon;
    const uint8_t *stream;
    enum EmeraldResourceCompatStatus status;

    FillPattern(backSheet, sizeof(backSheet), 13u);
    FillPattern(palette, sizeof(palette), 6u);
    FillFamilyEntries(entries, backSheet, sizeof(backSheet), palette, sizeof(palette),
                      EMERALD_COMPAT_ENTRY_RAW, (uint32_t)sizeof(backSheet));

    status = EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag);
    CHECK("raw family create ok", status == EMERALD_COMPAT_OK);
    CHECK("raw image non-NULL", image != NULL);
    if (image == NULL)
        return;

    CHECK("raw decoded size",
          EmeraldResourceCompatImage_GetDecodedSize(image, 0u) == 8192u);
    CHECK("raw stream size",
          EmeraldResourceCompatImage_GetStreamSize(image, 0u) == 8192u);
    canon = EmeraldResourceCompatImage_GetCanonical(image, 0u);
    stream = EmeraldResourceCompatImage_GetStream(image, 0u);
    CHECK("raw canonical non-NULL", canon != NULL);
    CHECK("raw stream non-NULL", stream != NULL);
    if (canon != NULL && stream != NULL)
    {
        CHECK("raw stream is the payload verbatim",
              memcmp(stream, canon, 8192u) == 0);
        CHECK("raw stream matches source",
              memcmp(stream, backSheet, sizeof(backSheet)) == 0);
    }

    /* The palette entry beside a RAW sheet still uses its LZ stream. */
    CHECK("raw-family palette stream size",
          EmeraldResourceCompatImage_GetStreamSize(image, 1u) == 40u);
    {
        uint8_t decodedPalette[32];
        size_t got = RefLz77Decode(EmeraldResourceCompatImage_GetStream(image, 1u),
                                   40u, decodedPalette, sizeof(decodedPalette));
        CHECK("raw-family palette decodes",
              got == 32u && memcmp(decodedPalette, palette, 32u) == 0);
    }

    EmeraldResourceCompatImage_Destroy(image);
}

/* §2/§21: the image is independent of the source payloads - it copies them.
 * Destroying the caller's payloads must not affect the image. */
static void TestImageSurvivesSourceDestruction(void)
{
    uint8_t *sheet = malloc(2048u);
    uint8_t *palette = malloc(32u);
    struct EmeraldResourceCompatSourceEntry entries[2];
    struct EmeraldResourceCompatibilityImage *image = NULL;
    struct EmeraldResourceCompatDiagnostics diag;
    uint8_t expectedSheet[2048];
    uint8_t expectedPalette[32];
    uint8_t decoded[2048];
    size_t got;

    FillPattern(sheet, 2048u, 11u);
    FillPattern(palette, 32u, 5u);
    memcpy(expectedSheet, sheet, sizeof(expectedSheet));
    memcpy(expectedPalette, palette, sizeof(expectedPalette));
    FillFamilyEntries(entries, sheet, 2048u, palette, 32u,
                      EMERALD_COMPAT_ENTRY_LZ, 0u);

    CHECK("create ok",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u,
              &image, &diag) == EMERALD_COMPAT_OK);

    /* Free the source payloads: the image must still hold the canonical bytes. */
    free(sheet);
    free(palette);

    CHECK("sheet survives source free",
          memcmp(EmeraldResourceCompatImage_GetCanonical(image, 0u),
                 expectedSheet, sizeof(expectedSheet)) == 0);
    CHECK("palette survives source free",
          memcmp(EmeraldResourceCompatImage_GetCanonical(image, 1u),
                 expectedPalette, sizeof(expectedPalette)) == 0);
    got = RefLz77Decode(EmeraldResourceCompatImage_GetStream(image, 0u), 2308u,
                        decoded, sizeof(decoded));
    CHECK("sheet stream still decodes after source free",
          got == sizeof(expectedSheet)
              && memcmp(decoded, expectedSheet, sizeof(expectedSheet)) == 0);

    EmeraldResourceCompatImage_Destroy(image);
}

/* §12/§24: fail closed - wrong sizes never allocate, *outImage stays NULL,
 * diagnostics carry the mismatch. */
static void TestImageFailClosed(void)
{
    uint8_t sheet[2048];
    uint8_t palette[32];
    uint8_t shortSheet[2047];
    uint8_t shortPalette[31];
    struct EmeraldResourceCompatSourceEntry entries[2];
    struct EmeraldResourceCompatibilityImage *image = (void *)0x1;
    struct EmeraldResourceCompatDiagnostics diag;

    FillPattern(sheet, sizeof(sheet), 1u);
    FillPattern(palette, sizeof(palette), 2u);

    FillFamilyEntries(entries, shortSheet, sizeof(shortSheet),
                      palette, sizeof(palette), EMERALD_COMPAT_ENTRY_LZ, 0u);
    CHECK("sheet too small rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag)
              == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
    CHECK("no image on sheet mismatch", image == NULL);
    CHECK("diag canonical sheet",
          strcmp(diag.canonicalName,
                 "emerald:test/trainer/brendan/battle/front/sheet") == 0);
    CHECK("diag stage build", strcmp(diag.stage, "build") == 0);
    CHECK("diag expected size", diag.expectedSize == 2048u);
    CHECK("diag actual size", diag.actualSize == 2047u);

    FillFamilyEntries(entries, sheet, sizeof(sheet),
                      shortPalette, sizeof(shortPalette),
                      EMERALD_COMPAT_ENTRY_LZ, 0u);
    image = (void *)0x1;
    CHECK("palette too small rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag)
              == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
    CHECK("no image on palette mismatch", image == NULL);
    CHECK("diag canonical palette",
          strcmp(diag.canonicalName,
                 "emerald:test/trainer/brendan/battle/front/palette") == 0);
    CHECK("diag actual palette size", diag.actualSize == 31u);

    /* Wrong expectedSize override is also a mismatch (R8). */
    FillFamilyEntries(entries, shortSheet, sizeof(shortSheet),
                      palette, sizeof(palette), EMERALD_COMPAT_ENTRY_RAW,
                      8192u);
    image = (void *)0x1;
    CHECK("override mismatch rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag)
              == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
    CHECK("no image on override mismatch", image == NULL);
    CHECK("override diag expected", diag.expectedSize == 8192u);

    /* Raw 8192-byte sheet + override 0 (type-derived 2048) mismatches too. */
    {
        uint8_t bigSheet[8192];
        FillPattern(bigSheet, sizeof(bigSheet), 9u);
        FillFamilyEntries(entries, bigSheet, sizeof(bigSheet),
                          palette, sizeof(palette), EMERALD_COMPAT_ENTRY_RAW,
                          0u);
        image = (void *)0x1;
        CHECK("raw 8192 without override rejected",
              EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag)
                  == EMERALD_COMPAT_ERR_PAYLOAD_SIZE_MISMATCH);
        CHECK("no image on raw no-override", image == NULL);
    }

    FillFamilyEntries(entries, sheet, sizeof(sheet),
                      palette, sizeof(palette), EMERALD_COMPAT_ENTRY_LZ, 0u);
    image = (void *)0x1;
    entries[0].payload = NULL;
    CHECK("null payload rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, &diag)
              == EMERALD_COMPAT_ERR_INVALID_ARGUMENT);
    CHECK("no image on null payload", image == NULL);
    CHECK("null entries rejected",
          EmeraldResourceCompatImage_CreateFamily(NULL, 2u, &image, &diag)
              == EMERALD_COMPAT_ERR_INVALID_ARGUMENT);
    CHECK("no image on null entries", image == NULL);
    CHECK("zero count rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 0u, &image, &diag)
              == EMERALD_COMPAT_ERR_INVALID_ARGUMENT);
    CHECK("null out rejected",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, NULL, &diag)
              == EMERALD_COMPAT_ERR_INVALID_ARGUMENT);

    FillFamilyEntries(entries, sheet, sizeof(sheet),
                      palette, sizeof(palette), EMERALD_COMPAT_ENTRY_LZ, 0u);
    image = (void *)0x1;
    CHECK("null diag allowed",
          EmeraldResourceCompatImage_CreateFamily(entries, 2u, &image, NULL)
              == EMERALD_COMPAT_OK);
    if (image != NULL)
        EmeraldResourceCompatImage_Destroy(image);
}

int main(void)
{
    TestEncodedSize();
    TestEncodeStructure();
    TestRoundTrips();
    TestImageBuildAndGetters();
    TestImageRawEntry();
    TestImageSurvivesSourceDestruction();
    TestImageFailClosed();

    if (gFailures != 0)
    {
        printf("emerald_resource_lz_unit FAILED: %d/%d checks\n",
               gFailures, gChecks);
        return 1;
    }
    printf("emerald resource lz unit passed (%d checks)\n", gChecks);
    return 0;
}
