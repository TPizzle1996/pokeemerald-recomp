#ifndef GEN3_RESOURCES_RESOURCE_TYPES_H
#define GEN3_RESOURCES_RESOURCE_TYPES_H

#include <stddef.h>
#include <stdint.h>

enum Gen3ResourceType
{
    GEN3_RESOURCE_TYPE_INVALID = 0,
    GEN3_RESOURCE_TYPE_BITMAP,
    GEN3_RESOURCE_TYPE_TILE_GRAPHICS,
    GEN3_RESOURCE_TYPE_PALETTE,
    GEN3_RESOURCE_TYPE_SPRITE_SHEET,
    GEN3_RESOURCE_TYPE_SPRITE_METADATA,
    GEN3_RESOURCE_TYPE_TILESET,
    GEN3_RESOURCE_TYPE_TILEMAP,
    GEN3_RESOURCE_TYPE_FONT,
    GEN3_RESOURCE_TYPE_TEXT,
    GEN3_RESOURCE_TYPE_AUDIO_SAMPLE,
    GEN3_RESOURCE_TYPE_MUSIC_SEQUENCE,
    GEN3_RESOURCE_TYPE_SOUND_EFFECT,
    GEN3_RESOURCE_TYPE_CRY,
    GEN3_RESOURCE_TYPE_BINARY,
};

struct Gen3TileGraphicsView
{
    const uint8_t *bytes;
    size_t size;
};

struct Gen3PaletteView
{
    const uint8_t *bytes;
    size_t size;
    size_t colorCount;
};

const char *Gen3ResourceType_Name(enum Gen3ResourceType type);

#endif
