/* R11-C: inert tileset-animation callbacks for the compat-seam harnesses.
 *
 * The generated tileset structs (tileset_native.generated.h, definition
 * mode in emerald_tileset_compat.c) reference the compiled
 * InitTilesetAnim_* entry points - which stay compiled in the real native
 * build (src/tileset_anims.c). The harnesses link only the seams, so the
 * callbacks are satisfied here as no-ops: the tests assert pointer
 * equality (.callback == InitTilesetAnim_X), never execution.
 */

#include "tileset_anims.h"

void InitTilesetAnim_General(void) {}
void InitTilesetAnim_Petalburg(void) {}
void InitTilesetAnim_Rustboro(void) {}
void InitTilesetAnim_Dewford(void) {}
void InitTilesetAnim_Slateport(void) {}
void InitTilesetAnim_Mauville(void) {}
void InitTilesetAnim_MauvilleGym(void) {}
void InitTilesetAnim_Lavaridge(void) {}
void InitTilesetAnim_Fallarbor(void) {}
void InitTilesetAnim_Fortree(void) {}
void InitTilesetAnim_Lilycove(void) {}
void InitTilesetAnim_Mossdeep(void) {}
void InitTilesetAnim_Sootopolis(void) {}
void InitTilesetAnim_SootopolisGym(void) {}
void InitTilesetAnim_Pacifidlog(void) {}
void InitTilesetAnim_EverGrande(void) {}
void InitTilesetAnim_EliteFour(void) {}
void InitTilesetAnim_Building(void) {}
void InitTilesetAnim_BikeShop(void) {}
void InitTilesetAnim_Cave(void) {}
void InitTilesetAnim_BattlePyramid(void) {}
void InitTilesetAnim_BattleFrontierOutsideWest(void) {}
void InitTilesetAnim_BattleFrontierOutsideEast(void) {}
void InitTilesetAnim_BattleDome(void) {}
void InitTilesetAnim_Underwater(void) {}
