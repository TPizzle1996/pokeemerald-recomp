/* R11-E/F: inert platform-runtime stubs for the neighborhood harnesses.
 *
 * host_memory.c (HostResolveGbaAddr / HostResolveGbaTableEntry /
 * HostPointerToGbaAddr) references the platform runtime memory-range
 * helpers and the GBA register/memory globals; the real native build
 * provides them (desktop_runtime.c + the linker script). The harnesses
 * link only the neighborhood module, so they are satisfied here. The
 * harnesses are non-PIE (-no-pie, as Makefile_pc:104/449), which keeps
 * HostPointerToGbaAddr's identity conversion valid.
 */

#include <stdint.h>

#include "global.h"
#include "gba/defines.h"

unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE];
unsigned char OAM[OAM_SIZE];
unsigned char REG_BASE[0x4000];

bool32 Platform_RuntimeGetImageRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    (void)outStart;
    (void)outEnd;
    return FALSE;
}

bool32 Platform_RuntimeGetGameBssRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    (void)outStart;
    (void)outEnd;
    return FALSE;
}

bool32 Platform_RuntimeGetGameDataRange(uintptr_t *outStart, uintptr_t *outEnd)
{
    (void)outStart;
    (void)outEnd;
    return FALSE;
}

bool32 Platform_RuntimeAddressIsExecutable(uintptr_t address)
{
    (void)address;
    return FALSE;
}
