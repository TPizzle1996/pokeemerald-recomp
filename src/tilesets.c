#include "global.h"
#include "tilesets.h"
#include "tileset_anims.h"

#if defined(NATIVE_LINUX)
/* R11-C: native declarations of the payload leaves and Tileset objects;
 * definitions live in the compat seam (TILESET_NATIVE_DEFINE). */
#include "emerald/resources/tileset_native.generated.h"
#endif

#include "data/tilesets/graphics.h"
#include "data/tilesets/metatiles.h"
#include "data/tilesets/headers.h"
