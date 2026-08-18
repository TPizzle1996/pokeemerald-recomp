/* R12-C leaf-test link stubs: the engine-symbol definitions host_memory.c
 * references when compiled standalone (GBA memory-region arrays under
 * -DPORTABLE, the linker-script host_data bounds, and the desktop runtime
 * image probes). The audio leaf test links the REAL host_memory.c so the
 * seam's R12-C logical-address table (RegisterLogicalLabel ->
 * HostMemoryRegisterLogicalAddress) operates on the genuine implementation;
 * these stubs only stand in for the surrounding engine, exactly as
 * /tmp gate's gate_stubs.c does for the offline parity gate.
 *
 * Unlike gate_stubs.c this file deliberately provides NO strong
 * EmeraldResourceCompat_GetRangeIndex: the leaf test owns that symbol (its
 * local index instance), and the seam's weak reference binds to it. */
#include <stddef.h>

#define STUB_PLTT_SIZE  0x400u
#define STUB_OAM_SIZE   0x400u
#define STUB_VRAM_SIZE  0x18000u

unsigned char PLTT[STUB_PLTT_SIZE];
unsigned char OAM[STUB_OAM_SIZE];
unsigned char VRAM_[STUB_VRAM_SIZE];
unsigned char REG_BASE[0x400u];
unsigned char __start_host_data[] = {0};
unsigned char __stop_host_data[] = {0};

typedef int bool32;
bool32 Platform_RuntimeGetImageRange(unsigned long *start, unsigned long *end)
{
    if (start != NULL)
        *start = 0;
    if (end != NULL)
        *end = 0;
    return 0;
}

bool32 Platform_RuntimeAddressIsExecutable(unsigned long address)
{
    (void)address;
    return 0;
}
