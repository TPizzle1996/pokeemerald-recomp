#ifndef GEN3_ELF_MANIFEST_FIXTURE_H
#define GEN3_ELF_MANIFEST_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/util.h"

/* Deterministic synthetic fixture backing the checked-in manifest and the
 * standalone test suite. The ELF symbol table indexes are stable (null symbol,
 * then front at index 1, palette at index 2), and the byte offsets inside the
 * built ELF are pure functions of the two artifact sizes. */

#define FIXTURE_FRONT_ADDR 0x08300000u
#define FIXTURE_PALETTE_ADDR 0x08310000u
#define FIXTURE_FRONT_ROM_OFFSET 0x00300000u
#define FIXTURE_PALETTE_ROM_OFFSET 0x00310000u

#define FIXTURE_FRONT_SYMBOL 1u
#define FIXTURE_PALETTE_SYMBOL 2u
#define FIXTURE_SYMBOL_TABLE_ENTRY_SIZE 16u
#define FIXTURE_SYMBOL_COUNT 3u

/* Multi-resource fixture (R7A §21): the R1A generator scale test needs a
 * fixture that carries a REPRESENTATIVE multi-resource set (a handful of
 * sheets + palettes), not just the two Brendan leaves. Resources are placed at
 * fixed slots so addresses/ROM offsets are pure functions of slot index:
 *   front   slot i : addr 0x08300000 + i*0x1000,  rom 0x00300000 + i*0x1000
 *   palette slot j : addr 0x08310000 + j*0x100,  rom 0x00310000 + j*0x100
 * 0x1000 covers every encoded sheet (max 1296 B) and 0x100 every palette
 * (40 B); slot counts stay tiny, far inside the 16 MiB ROM. */
#define FIXTURE_MULTI_FRONT_ADDR 0x08300000u
#define FIXTURE_MULTI_FRONT_ROM 0x00300000u
#define FIXTURE_MULTI_FRONT_SLOT 0x1000u
#define FIXTURE_MULTI_PALETTE_ADDR 0x08310000u
#define FIXTURE_MULTI_PALETTE_ROM 0x00310000u
#define FIXTURE_MULTI_PALETTE_SLOT 0x100u
#define FIXTURE_MULTI_MAX_RESOURCES 64u

/* Offsets within the built ELF (see fixture_build.c layout). */
uint32_t FixtureFrontSectionOffset(void);
uint32_t FixturePaletteSectionOffset(size_t frontSize);
uint32_t FixtureSymtabOffset(size_t frontSize, size_t paletteSize);
uint32_t FixtureStrtabOffset(size_t frontSize, size_t paletteSize);

/* The front symbol's name lives at FixtureStrtabOffset + FixtureFrontNameOffset. */
uint32_t FixtureFrontNameOffset(void);
uint32_t FixturePaletteNameOffset(void);

bool FixtureBuildElf(const uint8_t *front, size_t frontSize,
                     const uint8_t *palette, size_t paletteSize,
                     struct Gen3Buffer *out);
void FixtureBuildRom(const uint8_t *front, size_t frontSize,
                     const uint8_t *palette, size_t paletteSize,
                     struct Gen3Buffer *out);

/* One payload for the multi-resource fixture. `kind` is 1 for a front sheet
 * (gTrainerFrontPic_ slot), 0 for a palette (gTrainerPalette_ slot). */
struct FixtureResource
{
    const char *symbol;
    const uint8_t *data;
    size_t size;
    int kind; /* 1 = front sheet, 0 = palette */
};

bool FixtureBuildElfMulti(const struct FixtureResource *resources, size_t count,
                          struct Gen3Buffer *out);
void FixtureBuildRomMulti(const struct FixtureResource *resources, size_t count,
                          struct Gen3Buffer *out);

#endif
