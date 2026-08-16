/* Native-target compatibility publication (Stage R5, generalized in R7B,
 * extended to the trainer-BACK family in R8).
 * See emerald_trainer_native_compat.h for the contract. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_SDL2) && defined(NATIVE_LINUX)

#include "global.h"
#include "data.h"
#include "sprite.h"
#include "constants/trainers.h"

#include "gen3/resources/resource_resolver.h"
#include "emerald/resources/emerald_resource_compat.h"
#include "emerald/resources/emerald_resource_session.h"
#include "emerald/resources/emerald_pokemon_native_compat.h"
#include "emerald/resources/emerald_trainer_native_compat.h"

/* Session-global state: the published image is retained for the process session
 * (R5 §5) and released only by EmeraldResourceCompat_Shutdown. The registered
 * snapshot is the R6 loader feed; initialization runs at most once per
 * registration. */
static struct EmeraldResourceCompatibilityImage *sSessionImage;
static const struct Gen3ResourceSnapshot *sSessionSnapshot;
static bool32 sInitialized;

/* ------------------------------------------------------------------------- */
/* Family mappings (R7B §2, R8 §2).                                          */
/*                                                                           */
/* One row per gTrainerFrontPicTable slot, mirroring the R7A family          */
/* descriptor (resources/extraction/emerald/bpee01/trainer_front_family.toml,*/
/* canonical-name rule §7): the canonical component plus the                 */
/* gTrainerBackPicPaletteTable slot that ALSO consumes the front normal-     */
/* palette resource (-1 when none). The resource ids are built from the      */
/* canonical component with the descriptor's id templates. The generalized   */
/* test harness verifies this table against the descriptor, so it cannot     */
/* drift. The back family mappings follow below.                             */
/* ------------------------------------------------------------------------- */

struct EmeraldTrainerFrontSlot
{
    const char *canonical;      /* canonical component, e.g. "aqua-grunt-m" */
    int16_t backPaletteIndex;   /* TRAINER_BACK_PIC_* slot or -1 */
};

static const struct EmeraldTrainerFrontSlot kTrainerFrontSlots[EMERALD_TRAINER_FRONT_COUNT] =
{
    /*  0 TRAINER_PIC_HIKER                      */ { "hiker", -1 },
    /*  1 TRAINER_PIC_AQUA_GRUNT_M               */ { "aqua-grunt-m", -1 },
    /*  2 TRAINER_PIC_POKEMON_BREEDER_F          */ { "pokemon-breeder-f", -1 },
    /*  3 TRAINER_PIC_COOLTRAINER_M              */ { "cooltrainer-m", -1 },
    /*  4 TRAINER_PIC_BIRD_KEEPER                */ { "bird-keeper", -1 },
    /*  5 TRAINER_PIC_COLLECTOR                  */ { "collector", -1 },
    /*  6 TRAINER_PIC_AQUA_GRUNT_F               */ { "aqua-grunt-f", -1 },
    /*  7 TRAINER_PIC_SWIMMER_M                  */ { "swimmer-m", -1 },
    /*  8 TRAINER_PIC_MAGMA_GRUNT_M              */ { "magma-grunt-m", -1 },
    /*  9 TRAINER_PIC_EXPERT_M                   */ { "expert-m", -1 },
    /* 10 TRAINER_PIC_AQUA_ADMIN_M               */ { "aqua-admin-m", -1 },
    /* 11 TRAINER_PIC_BLACK_BELT                 */ { "black-belt", -1 },
    /* 12 TRAINER_PIC_AQUA_ADMIN_F               */ { "aqua-admin-f", -1 },
    /* 13 TRAINER_PIC_AQUA_LEADER_ARCHIE         */ { "aqua-leader-archie", -1 },
    /* 14 TRAINER_PIC_HEX_MANIAC                 */ { "hex-maniac", -1 },
    /* 15 TRAINER_PIC_AROMA_LADY                 */ { "aroma-lady", -1 },
    /* 16 TRAINER_PIC_RUIN_MANIAC                */ { "ruin-maniac", -1 },
    /* 17 TRAINER_PIC_INTERVIEWER                */ { "interviewer", -1 },
    /* 18 TRAINER_PIC_TUBER_F                    */ { "tuber-f", -1 },
    /* 19 TRAINER_PIC_TUBER_M                    */ { "tuber-m", -1 },
    /* 20 TRAINER_PIC_COOLTRAINER_F              */ { "cooltrainer-f", -1 },
    /* 21 TRAINER_PIC_LADY                       */ { "lady", -1 },
    /* 22 TRAINER_PIC_BEAUTY                     */ { "beauty", -1 },
    /* 23 TRAINER_PIC_RICH_BOY                   */ { "rich-boy", -1 },
    /* 24 TRAINER_PIC_EXPERT_F                   */ { "expert-f", -1 },
    /* 25 TRAINER_PIC_POKEMANIAC                 */ { "pokemaniac", -1 },
    /* 26 TRAINER_PIC_MAGMA_GRUNT_F              */ { "magma-grunt-f", -1 },
    /* 27 TRAINER_PIC_GUITARIST                  */ { "guitarist", -1 },
    /* 28 TRAINER_PIC_KINDLER                    */ { "kindler", -1 },
    /* 29 TRAINER_PIC_CAMPER                     */ { "camper", -1 },
    /* 30 TRAINER_PIC_PICNICKER                  */ { "picnicker", -1 },
    /* 31 TRAINER_PIC_BUG_MANIAC                 */ { "bug-maniac", -1 },
    /* 32 TRAINER_PIC_POKEMON_BREEDER_M          */ { "pokemon-breeder-m", -1 },
    /* 33 TRAINER_PIC_PSYCHIC_M                  */ { "psychic-m", -1 },
    /* 34 TRAINER_PIC_PSYCHIC_F                  */ { "psychic-f", -1 },
    /* 35 TRAINER_PIC_GENTLEMAN                  */ { "gentleman", -1 },
    /* 36 TRAINER_PIC_ELITE_FOUR_SIDNEY          */ { "elite-four-sidney", -1 },
    /* 37 TRAINER_PIC_ELITE_FOUR_PHOEBE          */ { "elite-four-phoebe", -1 },
    /* 38 TRAINER_PIC_ELITE_FOUR_GLACIA          */ { "elite-four-glacia", -1 },
    /* 39 TRAINER_PIC_ELITE_FOUR_DRAKE           */ { "elite-four-drake", -1 },
    /* 40 TRAINER_PIC_LEADER_ROXANNE             */ { "leader-roxanne", -1 },
    /* 41 TRAINER_PIC_LEADER_BRAWLY              */ { "leader-brawly", -1 },
    /* 42 TRAINER_PIC_LEADER_WATTSON             */ { "leader-wattson", -1 },
    /* 43 TRAINER_PIC_LEADER_FLANNERY            */ { "leader-flannery", -1 },
    /* 44 TRAINER_PIC_LEADER_NORMAN              */ { "leader-norman", -1 },
    /* 45 TRAINER_PIC_LEADER_WINONA              */ { "leader-winona", -1 },
    /* 46 TRAINER_PIC_LEADER_TATE_AND_LIZA       */ { "leader-tate-and-liza", -1 },
    /* 47 TRAINER_PIC_LEADER_JUAN                */ { "leader-juan", -1 },
    /* 48 TRAINER_PIC_SCHOOL_KID_M               */ { "school-kid-m", -1 },
    /* 49 TRAINER_PIC_SCHOOL_KID_F               */ { "school-kid-f", -1 },
    /* 50 TRAINER_PIC_SR_AND_JR                  */ { "sr-and-jr", -1 },
    /* 51 TRAINER_PIC_POKEFAN_M                  */ { "pokefan-m", -1 },
    /* 52 TRAINER_PIC_POKEFAN_F                  */ { "pokefan-f", -1 },
    /* 53 TRAINER_PIC_YOUNGSTER                  */ { "youngster", -1 },
    /* 54 TRAINER_PIC_CHAMPION_WALLACE           */ { "champion-wallace", -1 },
    /* 55 TRAINER_PIC_FISHERMAN                  */ { "fisherman", -1 },
    /* 56 TRAINER_PIC_CYCLING_TRIATHLETE_M       */ { "cycling-triathlete-m", -1 },
    /* 57 TRAINER_PIC_CYCLING_TRIATHLETE_F       */ { "cycling-triathlete-f", -1 },
    /* 58 TRAINER_PIC_RUNNING_TRIATHLETE_M       */ { "running-triathlete-m", -1 },
    /* 59 TRAINER_PIC_RUNNING_TRIATHLETE_F       */ { "running-triathlete-f", -1 },
    /* 60 TRAINER_PIC_SWIMMING_TRIATHLETE_M      */ { "swimming-triathlete-m", -1 },
    /* 61 TRAINER_PIC_SWIMMING_TRIATHLETE_F      */ { "swimming-triathlete-f", -1 },
    /* 62 TRAINER_PIC_DRAGON_TAMER               */ { "dragon-tamer", -1 },
    /* 63 TRAINER_PIC_NINJA_BOY                  */ { "ninja-boy", -1 },
    /* 64 TRAINER_PIC_BATTLE_GIRL                */ { "battle-girl", -1 },
    /* 65 TRAINER_PIC_PARASOL_LADY               */ { "parasol-lady", -1 },
    /* 66 TRAINER_PIC_SWIMMER_F                  */ { "swimmer-f", -1 },
    /* 67 TRAINER_PIC_TWINS                      */ { "twins", -1 },
    /* 68 TRAINER_PIC_SAILOR                     */ { "sailor", -1 },
    /* 69 TRAINER_PIC_MAGMA_ADMIN                */ { "magma-admin", -1 },
    /* 70 TRAINER_PIC_WALLY                      */ { "wally", 6 },
    /* 71 TRAINER_PIC_BRENDAN                    */ { "brendan", 0 },
    /* 72 TRAINER_PIC_MAY                        */ { "may", 1 },
    /* 73 TRAINER_PIC_BUG_CATCHER                */ { "bug-catcher", -1 },
    /* 74 TRAINER_PIC_POKEMON_RANGER_M           */ { "pokemon-ranger-m", -1 },
    /* 75 TRAINER_PIC_POKEMON_RANGER_F           */ { "pokemon-ranger-f", -1 },
    /* 76 TRAINER_PIC_MAGMA_LEADER_MAXIE         */ { "magma-leader-maxie", -1 },
    /* 77 TRAINER_PIC_LASS                       */ { "lass", -1 },
    /* 78 TRAINER_PIC_YOUNG_COUPLE               */ { "young-couple", -1 },
    /* 79 TRAINER_PIC_OLD_COUPLE                 */ { "old-couple", -1 },
    /* 80 TRAINER_PIC_SIS_AND_BRO                */ { "sis-and-bro", -1 },
    /* 81 TRAINER_PIC_STEVEN                     */ { "steven", 7 },
    /* 82 TRAINER_PIC_SALON_MAIDEN_ANABEL        */ { "salon-maiden-anabel", -1 },
    /* 83 TRAINER_PIC_DOME_ACE_TUCKER            */ { "dome-ace-tucker", -1 },
    /* 84 TRAINER_PIC_PALACE_MAVEN_SPENSER       */ { "palace-maven-spenser", -1 },
    /* 85 TRAINER_PIC_ARENA_TYCOON_GRETA         */ { "arena-tycoon-greta", -1 },
    /* 86 TRAINER_PIC_FACTORY_HEAD_NOLAND        */ { "factory-head-noland", -1 },
    /* 87 TRAINER_PIC_PIKE_QUEEN_LUCY            */ { "pike-queen-lucy", -1 },
    /* 88 TRAINER_PIC_PYRAMID_KING_BRANDON       */ { "pyramid-king-brandon", -1 },
    /* 89 TRAINER_PIC_RED                        */ { "red", -1 },
    /* 90 TRAINER_PIC_LEAF                       */ { "leaf", -1 },
    /* 91 TRAINER_PIC_RS_BRENDAN                 */ { "rs-brendan", 4 },
    /* 92 TRAINER_PIC_RS_MAY                     */ { "rs-may", 5 },
};

/* ------------------------------------------------------------------------- */
/* Back family mapping (R8 §2).                                              */
/*                                                                           */
/* One row per gTrainerBackPicTable slot, mirroring the R8 family descriptor */
/* (resources/extraction/emerald/bpee01/trainer_back_family.toml,           */
/* canonical-name rule §7): the canonical component, the TRAINER_BACK_PIC_*  */
/* table index, the SpriteFrameImage frame count (sheet size = frameCount *  */
/* TRAINER_PIC_SIZE) and the frame table itself - the LIVE pixel surface     */
/* (sTrainerBackSpriteTemplates .images). The two Red/Leaf palette slots are */
/* the only back-only palettes (LZ77); the other six back-palette slots are  */
/* R7B aliases of front normal-palette resources and are already served by   */
/* the front loop. The generalized test harness verifies this table against  */
/* the descriptor, so it cannot drift.                                       */
/* ------------------------------------------------------------------------- */

struct EmeraldTrainerBackSlot
{
    const char *canonical;              /* e.g. "brendan" */
    uint8_t tableIndex;                 /* TRAINER_BACK_PIC_* value */
    uint8_t frameCount;                 /* frame array length (4/5) */
    struct SpriteFrameImage *frameTable; /* data.c LIVE pixel surface */
};

static const struct EmeraldTrainerBackSlot
    kTrainerBackSheetSlots[EMERALD_TRAINER_BACK_SHEET_COUNT] =
{
    /*  0 TRAINER_BACK_PIC_BRENDAN             */ { "brendan", TRAINER_BACK_PIC_BRENDAN, 4, gTrainerBackPicTable_Brendan },
    /*  1 TRAINER_BACK_PIC_MAY                 */ { "may", TRAINER_BACK_PIC_MAY, 4, gTrainerBackPicTable_May },
    /*  2 TRAINER_BACK_PIC_RED                 */ { "red", TRAINER_BACK_PIC_RED, 5, gTrainerBackPicTable_Red },
    /*  3 TRAINER_BACK_PIC_LEAF                */ { "leaf", TRAINER_BACK_PIC_LEAF, 5, gTrainerBackPicTable_Leaf },
    /*  4 TRAINER_BACK_PIC_RUBY_SAPPHIRE_BRENDAN */ { "ruby-sapphire-brendan", TRAINER_BACK_PIC_RUBY_SAPPHIRE_BRENDAN, 4, gTrainerBackPicTable_RubySapphireBrendan },
    /*  5 TRAINER_BACK_PIC_RUBY_SAPPHIRE_MAY   */ { "ruby-sapphire-may", TRAINER_BACK_PIC_RUBY_SAPPHIRE_MAY, 4, gTrainerBackPicTable_RubySapphireMay },
    /*  6 TRAINER_BACK_PIC_WALLY               */ { "wally", TRAINER_BACK_PIC_WALLY, 4, gTrainerBackPicTable_Wally },
    /*  7 TRAINER_BACK_PIC_STEVEN              */ { "steven", TRAINER_BACK_PIC_STEVEN, 4, gTrainerBackPicTable_Steven },
};

struct EmeraldTrainerBackPaletteSlot
{
    const char *canonical;      /* canonical component */
    uint8_t tableIndex;         /* TRAINER_BACK_PIC_* value */
};

static const struct EmeraldTrainerBackPaletteSlot
    kTrainerBackPaletteSlots[EMERALD_TRAINER_BACK_PALETTE_COUNT] =
{
    /*  2 TRAINER_BACK_PIC_RED                 */ { "red", TRAINER_BACK_PIC_RED },
    /*  3 TRAINER_BACK_PIC_LEAF                */ { "leaf", TRAINER_BACK_PIC_LEAF },
};

/* Build the canonical back resource id from the R8 descriptor id templates:
 *   sheet:   emerald:trainer/<canonical>/battle/back/sheet   (raw 4bpp)
 *   palette: emerald:trainer/<canonical>/battle/back/palette (gba-lz77)
 * Returns the number of characters written (excluding NUL). */
static int BuildBackResourceId(char *out, size_t outSize,
                               const char *canonical, bool isPalette)
{
    int n;
    if (isPalette)
        n = snprintf(out, outSize, "emerald:trainer/%s/battle/back/palette",
                     canonical);
    else
        n = snprintf(out, outSize, "emerald:trainer/%s/battle/back/sheet",
                     canonical);
    return n;
}

/* Build the canonical resource id for a trainer component from the descriptor
 * id templates. The two forms:
 *   sheet:   emerald:trainer/<canonical>/battle/front/sheet
 *   palette: emerald:trainer/<canonical>/battle/front/normal-palette
 * Returns the number of characters written (excluding NUL). */
static int BuildResourceId(char *out, size_t outSize,
                           const char *canonical, bool isPalette)
{
    int n;
    if (isPalette)
        n = snprintf(out, outSize, "emerald:trainer/%s/battle/front/normal-palette",
                     canonical);
    else
        n = snprintf(out, outSize, "emerald:trainer/%s/battle/front/sheet",
                     canonical);
    return n;
}

static const char *TypeName(enum Gen3ResourceType type)
{
    switch (type)
    {
    case GEN3_RESOURCE_TYPE_TILE_GRAPHICS:
        return "tile-graphics";
    case GEN3_RESOURCE_TYPE_PALETTE:
        return "palette";
    default:
        return "other";
    }
}

static void ClearDiagnostics(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    if (diagnostics != NULL)
        memset(diagnostics, 0, sizeof(*diagnostics));
}

static void RecordView(struct EmeraldResourceCompatDiagnostics *diagnostics,
                       const char *canonicalName,
                       const struct Gen3ResourceView *view)
{
    if (diagnostics == NULL)
        return;
    if (canonicalName != NULL)
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", canonicalName);
    snprintf(diagnostics->stage, sizeof(diagnostics->stage), "resolve");
    if (view != NULL)
    {
        snprintf(diagnostics->actualType, sizeof(diagnostics->actualType),
                 "%s", TypeName(view->type));
        diagnostics->actualSchema = view->schema;
        diagnostics->actualSize = (uint32_t)view->payloadSize;
        if (view->winningProviderId != NULL)
            snprintf(diagnostics->winningProviderId,
                     sizeof(diagnostics->winningProviderId), "%s",
                     view->winningProviderId);
        if (view->winningProviderVersion != NULL)
            snprintf(diagnostics->winningProviderVersion,
                     sizeof(diagnostics->winningProviderVersion), "%s",
                     view->winningProviderVersion);
        diagnostics->winningProviderPrecedence = view->winningProviderPrecedence;
    }
}

/* Resolve one family resource through the NORMAL active snapshot resolver and
 * verify type/schema/payload-size/winner before anything is built (§11/§12).
 * On failure diagnostics carry the stage + expected/actual identifiers and the
 * failure leaves the live tables untouched (the caller builds+publishes only
 * after every resource resolves). */
static enum EmeraldResourceCompatStatus
ResolveFamilyResource(const struct Gen3ResourceSnapshot *snapshot,
                      const char *canonicalName,
                      enum Gen3ResourceType expectedType,
                      uint32_t expectedSchema,
                      uint32_t expectedSize,
                      struct Gen3ResourceView *outView,
                      struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    Gen3ResourceHandle handle;
    enum Gen3ResourceResult result;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL || canonicalName == NULL || outView == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (diagnostics != NULL)
    {
        snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                 "%s", canonicalName);
        snprintf(diagnostics->stage, sizeof(diagnostics->stage), "resolve");
        snprintf(diagnostics->expectedType, sizeof(diagnostics->expectedType),
                 "%s", TypeName(expectedType));
        diagnostics->expectedSchema = expectedSchema;
        diagnostics->expectedSize = expectedSize;
    }

    result = Gen3ResourceSnapshot_FindHandle(snapshot, canonicalName, &handle);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;

    result = Gen3ResourceSnapshot_Resolve(snapshot, handle, expectedType,
                                          expectedSchema, outView);
    if (result != GEN3_RESOURCE_OK)
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;

    /* Resolve enforces type/schema; the payload size and the winner identity
     * are verified here so a wrong-sized or non-ROM_BASE resource fails closed
     * (§11/§12). */
    if (outView->payloadSize != (size_t)expectedSize
     || outView->winningProviderId == NULL
     || strcmp(outView->winningProviderId, EMERALD_ROM_BASE_PROVIDER_ID) != 0)
    {
        RecordView(diagnostics, canonicalName, outView);
        return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
    }

    RecordView(diagnostics, canonicalName, outView);
    return EMERALD_COMPAT_OK;
}

/* Validate the image against the family mappings BEFORE any mutation (§13):
 * entry count must be exactly 196 (2*93 + 10); the front section is checked
 * as in R7B (sheet at 2i, palette at 2i+1, decoding to 2048/32 bytes); the
 * back section then must hold the 8 back sheets (TILE_GRAPHICS, name matches
 * the back mapping's sheet id, decoded size frameCount*2048) followed by the
 * 2 back-only palettes (PALETTE, 32 bytes, red then leaf). The six shared
 * back-pic palette consumers are validated against the SAME front palette
 * entry, so a mismatch can only come from the mappings themselves, which the
 * tests pin against both descriptors. */
static enum EmeraldResourceCompatStatus
ValidateImageAgainstMapping(
    const struct EmeraldResourceCompatibilityImage *image,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    size_t i;

    ClearDiagnostics(diagnostics);
    if (image == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;
    if (EmeraldResourceCompatImage_GetEntryCount(image)
            != EMERALD_TRAINER_FAMILY_ENTRY_COUNT)
    {
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "publish");
        return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
    }
    for (i = 0u; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct EmeraldTrainerFrontSlot *slot = &kTrainerFrontSlots[i];
        size_t sheetIndex = i * 2u;
        size_t paletteIndex = i * 2u + 1u;
        char id[96];
        const char *name;

        if (BuildResourceId(id, sizeof(id), slot->canonical, false) >= (int)sizeof(id))
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        name = EmeraldResourceCompatImage_GetEntryName(image, sheetIndex);
        if (name == NULL || strcmp(name, id) != 0
         || EmeraldResourceCompatImage_GetEntryType(image, sheetIndex)
                != GEN3_RESOURCE_TYPE_TILE_GRAPHICS
         || EmeraldResourceCompatImage_GetDecodedSize(image, sheetIndex)
                != EMERALD_TRAINER_SHEET_SIZE)
        {
            if (diagnostics != NULL)
            {
                snprintf(diagnostics->stage, sizeof(diagnostics->stage), "publish");
                snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                         "%s", id);
            }
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }

        (void)BuildResourceId(id, sizeof(id), slot->canonical, true);
        name = EmeraldResourceCompatImage_GetEntryName(image, paletteIndex);
        if (name == NULL || strcmp(name, id) != 0
         || EmeraldResourceCompatImage_GetEntryType(image, paletteIndex)
                != GEN3_RESOURCE_TYPE_PALETTE
         || EmeraldResourceCompatImage_GetDecodedSize(image, paletteIndex)
                != EMERALD_TRAINER_PALETTE_SIZE)
        {
            if (diagnostics != NULL)
            {
                snprintf(diagnostics->stage, sizeof(diagnostics->stage), "publish");
                snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                         "%s", id);
            }
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct EmeraldTrainerBackSlot *slot = &kTrainerBackSheetSlots[i];
        size_t entryIndex = EMERALD_TRAINER_FRONT_ENTRY_COUNT + i;
        char id[96];
        const char *name;

        if (BuildBackResourceId(id, sizeof(id), slot->canonical, false)
                >= (int)sizeof(id))
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        name = EmeraldResourceCompatImage_GetEntryName(image, entryIndex);
        if (name == NULL || strcmp(name, id) != 0
         || EmeraldResourceCompatImage_GetEntryType(image, entryIndex)
                != GEN3_RESOURCE_TYPE_TILE_GRAPHICS
         || EmeraldResourceCompatImage_GetDecodedSize(image, entryIndex)
                != (uint32_t)slot->frameCount * EMERALD_TRAINER_SHEET_SIZE)
        {
            if (diagnostics != NULL)
            {
                snprintf(diagnostics->stage, sizeof(diagnostics->stage), "publish");
                snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                         "%s", id);
            }
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_PALETTE_COUNT; i++)
    {
        const struct EmeraldTrainerBackPaletteSlot *slot =
            &kTrainerBackPaletteSlots[i];
        size_t entryIndex = EMERALD_TRAINER_FRONT_ENTRY_COUNT
            + EMERALD_TRAINER_BACK_SHEET_COUNT + i;
        char id[96];
        const char *name;

        if (BuildBackResourceId(id, sizeof(id), slot->canonical, true)
                >= (int)sizeof(id))
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        name = EmeraldResourceCompatImage_GetEntryName(image, entryIndex);
        if (name == NULL || strcmp(name, id) != 0
         || EmeraldResourceCompatImage_GetEntryType(image, entryIndex)
                != GEN3_RESOURCE_TYPE_PALETTE
         || EmeraldResourceCompatImage_GetDecodedSize(image, entryIndex)
                != EMERALD_TRAINER_PALETTE_SIZE)
        {
            if (diagnostics != NULL)
            {
                snprintf(diagnostics->stage, sizeof(diagnostics->stage), "publish");
                snprintf(diagnostics->canonicalName, sizeof(diagnostics->canonicalName),
                         "%s", id);
            }
            return EMERALD_COMPAT_ERR_PUBLISH_FAILED;
        }
    }
    return EMERALD_COMPAT_OK;
}

enum EmeraldResourceCompatStatus
EmeraldResourceCompat_PublishTrainerTables(
    const struct EmeraldResourceCompatibilityImage *image,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diagnostics);
    status = ValidateImageAgainstMapping(image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        return status;

    /* §7: change ONLY the migrated payload pointers. The image entries are in
     * family order (front: sheet at 2i, palette at 2i+1 for table index i;
     * back: 8 sheets then 2 palettes appended after the front 186), so each
     * trainer's two streams land in its two front table slots; the six shared
     * back-pic palette slots reuse the SAME normal-palette stream (live
     * consumers: battle_controller_player.c by gender, wally/steven/link/
     * recorded battlers by pic id). The back family (R8) publishes the 8
     * gTrainerBackPicTable sheet slots - DecompressTrainerBackPic
     * (battle_gfx_sfx_util.c) dereferences their data pointers every battle,
     * though the raw-sheet LZ77 decode output is unused and overwritten
     * later - and the 34 gTrainerBackPicTable_<X> SpriteFrameImage slots, the
     * LIVE pixel surface: sTrainerBackSpriteTemplates -> CreateSprite ->
     * RequestSpriteFrameImageCopy reads images[frame].data into OBJ VRAM.
     * Each frame slot gets the session raw sheet plus its frame offset, the
     * exact arithmetic the GBA build compiles statically. Indices, tags,
     * sizes, everything else unchanged. The streams are const (immutable
     * after construction, §5). */
    for (i = 0u; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct EmeraldTrainerFrontSlot *slot = &kTrainerFrontSlots[i];
        const uint8_t *sheetStream =
            EmeraldResourceCompatImage_GetStream(image, i * 2u);
        const uint8_t *paletteStream =
            EmeraldResourceCompatImage_GetStream(image, i * 2u + 1u);

        gTrainerFrontPicTable[i].data = (const u32 *)(const void *)sheetStream;
        gTrainerFrontPicPaletteTable[i].data =
            (const u32 *)(const void *)paletteStream;
        if (slot->backPaletteIndex >= 0)
            gTrainerBackPicPaletteTable[slot->backPaletteIndex].data =
                (const u32 *)(const void *)paletteStream;
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct EmeraldTrainerBackSlot *slot = &kTrainerBackSheetSlots[i];
        const uint8_t *sheetStream = EmeraldResourceCompatImage_GetStream(
            image, EMERALD_TRAINER_FRONT_ENTRY_COUNT + i);
        size_t frame;

        gTrainerBackPicTable[slot->tableIndex].data =
            (const u32 *)(const void *)sheetStream;
        for (frame = 0u; frame < slot->frameCount; frame++)
            slot->frameTable[frame].data =
                (const void *)(sheetStream + EMERALD_TRAINER_SHEET_SIZE * frame);
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_PALETTE_COUNT; i++)
    {
        const struct EmeraldTrainerBackPaletteSlot *slot =
            &kTrainerBackPaletteSlots[i];
        const uint8_t *paletteStream = EmeraldResourceCompatImage_GetStream(
            image, EMERALD_TRAINER_FRONT_ENTRY_COUNT
                       + EMERALD_TRAINER_BACK_SHEET_COUNT + i);

        gTrainerBackPicPaletteTable[slot->tableIndex].data =
            (const u32 *)(const void *)paletteStream;
    }
    return EMERALD_COMPAT_OK;
}

enum EmeraldResourceCompatStatus
EmeraldResourceCompat_Republish(struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    enum EmeraldResourceCompatStatus status;

    ClearDiagnostics(diagnostics);
    if (sSessionImage == NULL)
    {
        /* No valid session image: fail closed. Nothing is re-resolved from ROM,
         * no pack is reread, no stream is rebuilt (§2) - the only source is the
         * already-valid current-session image, which is absent. */
        if (diagnostics != NULL)
            snprintf(diagnostics->stage, sizeof(diagnostics->stage), "republish");
        return EMERALD_COMPAT_ERR_UNAVAILABLE;
    }
    /* Idempotent, allocation-free: re-derives every pointer from the retained
     * image and rewrites the same migrated slots. R9 §5: the Pokémon battle
     * tables are re-derived from their session image the same way. A session
     * in which the Pokémon family never initialized (trainer-only snapshot)
     * has no Pokémon image: that is the additive-degradation state - the
     * Pokémon tables were never migrated - not an error, so
     * EMERALD_COMPAT_ERR_UNAVAILABLE is treated as success here (diagnostics
     * cleared). Any other Pokémon failure is a real fault. */
    status = EmeraldResourceCompat_PublishTrainerTables(sSessionImage, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        return status;
    status = EmeraldPokemonCompat_Republish(diagnostics);
    if (status != EMERALD_COMPAT_OK && status != EMERALD_COMPAT_ERR_UNAVAILABLE)
        return status;
    ClearDiagnostics(diagnostics);
    return EMERALD_COMPAT_OK;
}

void EmeraldResourceCompat_ClearMigratedEntries(void)
{
    size_t i;

    /* Fail-closed sentinel: clear only the migrated native table slots. If a
     * state load ever has to drop the session image, no consumer can read a
     * stale or republished pointer. R8: the whole trainer family is migrated
     * - front sheets/palettes, the 6 shared back-palette slots, and the back
     * family (8 sheet slots, 34 SpriteFrameImage slots, Red/Leaf palette
     * slots). Nothing else is touched. */
    for (i = 0u; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        gTrainerFrontPicTable[i].data = NULL;
        gTrainerFrontPicPaletteTable[i].data = NULL;
    }
    for (i = 0u; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct EmeraldTrainerFrontSlot *slot = &kTrainerFrontSlots[i];
        if (slot->backPaletteIndex >= 0)
            gTrainerBackPicPaletteTable[slot->backPaletteIndex].data = NULL;
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct EmeraldTrainerBackSlot *slot = &kTrainerBackSheetSlots[i];
        size_t frame;

        gTrainerBackPicTable[slot->tableIndex].data = NULL;
        for (frame = 0u; frame < slot->frameCount; frame++)
            slot->frameTable[frame].data = NULL;
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_PALETTE_COUNT; i++)
        gTrainerBackPicPaletteTable[kTrainerBackPaletteSlots[i].tableIndex].data =
            NULL;

    /* R9 §5: the four Pokémon battle tables (every published slot; the
     * external back-EGG slot is never touched). */
    EmeraldPokemonCompat_ClearMigratedEntries();
}

enum EmeraldResourceCompatStatus
EmeraldResourceCompat_InitializeFromSnapshot(
    const struct Gen3ResourceSnapshot *snapshot,
    struct EmeraldResourceCompatDiagnostics *diagnostics)
{
    struct EmeraldResourceCompatSourceEntry entries[EMERALD_TRAINER_FAMILY_ENTRY_COUNT];
    struct Gen3ResourceView views[EMERALD_TRAINER_FAMILY_ENTRY_COUNT];
    char names[EMERALD_TRAINER_FAMILY_ENTRY_COUNT][96];
    struct EmeraldResourceCompatibilityImage *image = NULL;
    enum EmeraldResourceCompatStatus status;
    size_t i;

    ClearDiagnostics(diagnostics);
    if (snapshot == NULL)
        return EMERALD_COMPAT_ERR_INVALID_ARGUMENT;

    /* Phase 1: resolve every family resource through the NORMAL snapshot,
     * verifying type/schema/payload-size/winner==ROM_BASE (§11/§12). Any
     * failure leaves the live tables untouched (nothing has been mutated). */
    for (i = 0u; i < EMERALD_TRAINER_FRONT_COUNT; i++)
    {
        const struct EmeraldTrainerFrontSlot *slot = &kTrainerFrontSlots[i];
        size_t sheetIndex = i * 2u;
        size_t paletteIndex = i * 2u + 1u;

        if (BuildResourceId(names[sheetIndex], sizeof(names[sheetIndex]),
                            slot->canonical, false) < 0
         || BuildResourceId(names[paletteIndex], sizeof(names[paletteIndex]),
                            slot->canonical, true) < 0)
            return EMERALD_COMPAT_ERR_RESOLVE_FAILED;

        status = ResolveFamilyResource(snapshot, names[sheetIndex],
            GEN3_RESOURCE_TYPE_TILE_GRAPHICS, EMERALD_TRAINER_SCHEMA,
            EMERALD_TRAINER_SHEET_SIZE, &views[sheetIndex], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            return status;
        status = ResolveFamilyResource(snapshot, names[paletteIndex],
            GEN3_RESOURCE_TYPE_PALETTE, EMERALD_TRAINER_SCHEMA,
            EMERALD_TRAINER_PALETTE_SIZE, &views[paletteIndex], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            return status;
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        const struct EmeraldTrainerBackSlot *slot = &kTrainerBackSheetSlots[i];
        size_t entryIndex = EMERALD_TRAINER_FRONT_ENTRY_COUNT + i;

        if (BuildBackResourceId(names[entryIndex], sizeof(names[entryIndex]),
                                slot->canonical, false) < 0)
            return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
        status = ResolveFamilyResource(snapshot, names[entryIndex],
            GEN3_RESOURCE_TYPE_TILE_GRAPHICS, EMERALD_TRAINER_SCHEMA,
            (uint32_t)slot->frameCount * EMERALD_TRAINER_SHEET_SIZE,
            &views[entryIndex], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            return status;
    }
    for (i = 0u; i < EMERALD_TRAINER_BACK_PALETTE_COUNT; i++)
    {
        const struct EmeraldTrainerBackPaletteSlot *slot =
            &kTrainerBackPaletteSlots[i];
        size_t entryIndex = EMERALD_TRAINER_FRONT_ENTRY_COUNT
            + EMERALD_TRAINER_BACK_SHEET_COUNT + i;

        if (BuildBackResourceId(names[entryIndex], sizeof(names[entryIndex]),
                                slot->canonical, true) < 0)
            return EMERALD_COMPAT_ERR_RESOLVE_FAILED;
        status = ResolveFamilyResource(snapshot, names[entryIndex],
            GEN3_RESOURCE_TYPE_PALETTE, EMERALD_TRAINER_SCHEMA,
            EMERALD_TRAINER_PALETTE_SIZE, &views[entryIndex], diagnostics);
        if (status != EMERALD_COMPAT_OK)
            return status;
    }

    /* Phase 2: transactional construction - build the whole image first;
     * publish only on success. Any failure frees the image and leaves the
     * tables untouched. The image copies the payloads (and names), so it is
     * independent of the snapshot that produced them (§2/§5). The source
     * entries are zeroed first: the R8 per-entry fields (expectedSize,
     * encoding) default to 0 (derived size, LZ stream) and are only
     * overridden for the back sheets below - uninitialized stack bytes would
     * be garbage that CreateFamily validates against. */
    memset(entries, 0, sizeof(entries));
    for (i = 0u; i < EMERALD_TRAINER_FAMILY_ENTRY_COUNT; i++)
    {
        entries[i].canonicalName = names[i];
        entries[i].type = views[i].type;
        entries[i].schema = views[i].schema;
        entries[i].payload = views[i].payload;
        entries[i].payloadSize = (uint32_t)views[i].payloadSize;
    }
    /* R8: the back sheets are RAW entries with a per-entry size override (their
     * stream IS the payload bytes: sprite-pipeline copies and the
     * DecompressTrainerBackPic raw decode both need the verbatim sheets).
     * Front entries and the back palettes keep the R7B defaults (derived
     * 2048/32 sizes, literal-only LZ77 streams). */
    for (i = 0u; i < EMERALD_TRAINER_BACK_SHEET_COUNT; i++)
    {
        size_t entryIndex = EMERALD_TRAINER_FRONT_ENTRY_COUNT + i;
        const struct EmeraldTrainerBackSlot *slot = &kTrainerBackSheetSlots[i];

        entries[entryIndex].expectedSize =
            (uint32_t)slot->frameCount * EMERALD_TRAINER_SHEET_SIZE;
        entries[entryIndex].encoding = EMERALD_COMPAT_ENTRY_RAW;
    }
    status = EmeraldResourceCompatImage_CreateFamily(entries,
        EMERALD_TRAINER_FAMILY_ENTRY_COUNT, &image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
        return status;

    status = EmeraldResourceCompat_PublishTrainerTables(image, diagnostics);
    if (status != EMERALD_COMPAT_OK)
    {
        EmeraldResourceCompatImage_Destroy(image);
        return status;
    }

    /* Session lifetime (§5): replace any prior image. The old image's streams
     * are no longer referenced by the tables (every migrated pointer now
     * points into the new image), so destroying it is safe. */
    EmeraldResourceCompatImage_Destroy(sSessionImage);
    sSessionImage = image;
    sInitialized = TRUE;

    /* R9 §5: publish the Pokémon battle family from the same snapshot, with
     * the same lifecycle. The Pokémon family is ADDITIVE: a snapshot that
     * does not carry it (the trainer-only unit harness) - or whose Pokémon
     * resources fail validation - leaves the Pokémon tables at their
     * compiled payloads (until R9 §7): nothing mutated, no stale pointer
     * ever installed, and the trainer family's own success stands (the
     * seam's trainer contract predates R9). The diagnostics are cleared so
     * the pinned trainer contract ("success leaves diagnostics empty")
     * holds; the production proof catches a real pack regression anyway,
     * because its post-init Pokémon verification fails loudly on any slot
     * that did not publish. R9 §8 tightens this to hard-fail once the
     * compiled payloads are gone on native. */
    status = EmeraldPokemonCompat_TryInitialize(snapshot, diagnostics);
    if (status != EMERALD_COMPAT_OK)
    {
        fprintf(stderr, "emerald compat: Pokémon battle family not published "
                "(status %d%s%s) - tables stay at compiled payloads\n",
                (int)status,
                diagnostics != NULL && diagnostics->canonicalName[0] != '\0'
                    ? " @ " : "",
                diagnostics != NULL ? diagnostics->canonicalName : "");
        ClearDiagnostics(diagnostics);
    }

    return EMERALD_COMPAT_OK;
}

void EmeraldResourceCompat_SetSnapshot(const struct Gen3ResourceSnapshot *snapshot)
{
    sSessionSnapshot = snapshot;
    sInitialized = FALSE;
}

void EmeraldResourceCompat_ClearSnapshot(void)
{
    sSessionSnapshot = NULL;
    sInitialized = FALSE;
}

void EmeraldResourceCompat_TryInitialize(void)
{
    struct EmeraldResourceCompatDiagnostics diagnostics;

    if (sSessionSnapshot == NULL || sInitialized)
        return;
    /* The runtime hook is advisory until the R6 loader registers a snapshot;
     * the result is not surfaced here. TryInitialize is idempotent either way. */
    (void)EmeraldResourceCompat_InitializeFromSnapshot(sSessionSnapshot, &diagnostics);
}

void EmeraldResourceCompat_Shutdown(void)
{
    EmeraldResourceCompatImage_Destroy(sSessionImage);
    sSessionImage = NULL;
    sInitialized = FALSE;
    EmeraldPokemonCompat_Shutdown();
}

#endif /* PLATFORM_SDL2 && NATIVE_LINUX */
