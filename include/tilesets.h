#ifndef GUARD_tilesets_H
#define GUARD_tilesets_H

#if !defined(NATIVE_LINUX)
/* R11-C: these payload symbols are declared in emerald/resources/tileset_native.generated.h
 * on native (defined by the compat seam). GBA-only externs here. */
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
extern const u32 gTilesetTiles_General[];
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
extern const u16 gTilesetPalettes_General[][16];
#endif /* !NATIVE_LINUX */

extern const struct Tileset *const gTilesetPointer_SecretBase;
extern const struct Tileset *const gTilesetPointer_SecretBaseRedCave;

#endif //GUARD_tilesets_H
