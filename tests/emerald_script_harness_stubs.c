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
 * R13-G6 (plan sec 7.3): kBrailleTextAddresses. In the production
 * binary this array is defined by data/text/braille_addresses_native.inc
 * (generated, same assembly unit as braille.inc, pointing at the real
 * compiled braille text). The harness has no asm; the stub provides 22
 * distinct blocks so the test can prove the seam resolves every
 * BRAILLE reloc row to a unique live address. The real-binary byte
 * proof (brailleformat headers at each address) is sweep 7 of
 * verify_binary_isolation.py.
 *
 * NOT compiled into the production binary (the game defines these
 * symbols for real).
 */
#include <stddef.h>
#include <stdint.h>
#include "global.h"
#include "trainer_see.h"

uint8_t gStringVar4[1000];

/* 22 distinct stub blocks, one per braille text label (index order =
 * kBrailleGbaAddrs, sorted GBA provenance). The harness links -no-pie
 * (the generated .inc/.c tables are 32-bit .int relocations by design -
 * the production binary is -no-pie with every live address below
 * 4 GiB), so a pointer-to-uint32_t narrowing cast IS the real model;
 * it is just not a constant expression in C, hence the runtime fill
 * (EmeraldScriptHarness_InitBrailleStub, called by the shared harness
 * setup before any TryInitialize). */
static const uint8_t kBrailleStubBlocks[22][16] = {{0}};
uint32_t kBrailleTextAddresses[22];

void EmeraldScriptHarness_InitBrailleStub(void)
{
    size_t i;
    for (i = 0u; i < 22u; i++)
        kBrailleTextAddresses[i] = (uint32_t)(uintptr_t)kBrailleStubBlocks[i];
}

/* R13-G5: the live gStdScripts publication table (the game defines the
 * compiled .s table; the harness provides writable slots). */
const uint8_t *gStdScripts[11] = {0};
const uint8_t *gStdScripts_End[1];

const uint8_t Route103_Movement_RivalExitFacingNorth2[7];
const uint8_t Ferry_Movement_DepartIslandBoardSouth[2];
const uint8_t Ferry_Movement_DepartIslandBoardWest[3];

struct ApproachingTrainer gApproachingTrainers[2];
