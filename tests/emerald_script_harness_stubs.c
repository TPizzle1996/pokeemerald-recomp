/* R13-G3 script-compat harness stubs (test-only).
 *
 * The generated script table references two host-owned surfaces the
 * platform-neutral seam resolves through extern symbols:
 *
 *  - gStringVar4: the one approved writable RAM target (EWRAM
 *    0x02021fc4, 1,000 bytes). The seam never dereferences it in G3;
 *    the test proves the RAM target resolves to this exact symbol.
 *  - the three recomp-local movement bridges (7+2+3 = 12 B). The seam
 *    never dereferences them either (byte identity was the G2 oracle's
 *    proof); the generated table embeds the qualified-ROM bytes for
 *    future byte-identity checks.
 *
 * NOT compiled into the production binary (the game defines these
 * symbols for real).
 */
#include <stdint.h>
#include "global.h"
#include "trainer_see.h"

uint8_t gStringVar4[1000];

const uint8_t Route103_EventScript_RivalExitFacingNorth2[7];
const uint8_t Ferry_EventScript_DepartIslandBoardSouth[2];
const uint8_t Ferry_EventScript_DepartIslandBoardWest[3];

struct ApproachingTrainer gApproachingTrainers[2];
