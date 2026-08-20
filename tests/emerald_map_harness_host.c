/* R13-F focused-harness host shim (test-only).
 *
 * The R13-E runtime loader harness links the resource seams directly but not
 * src/platform/host_memory.c (it pulls the whole engine via global.h). The
 * map seam resolves script/layout GBA addresses through HostResolveGbaAddr /
 * HostResolveGbaTableEntry, so this TU supplies the host-identity resolution
 * those resolve to for non-registered addresses on a non-PIE native link -
 * identical semantics to host_memory.c's identity fallback. gMapLayouts is the
 * compiled INDEX_ROUTING layout-address table (normally an asm symbol); the
 * harness fills it with identity GBA entries so header->mapLayout resolution
 * is addressable and deterministic in test.
 *
 * NOT compiled into the production binary (only the focused test link).
 */
#include "gba/types.h"
#include "platform/host_memory.h"

#include <string.h>

#if defined(LINUX64) && LINUX64
GbaAddr HostPointerToGbaAddr(const void *ptr)
{
    return (GbaAddr)(uintptr_t)ptr;
}

void *HostResolveGbaAddr(GbaAddr addr)
{
    return (void *)(uintptr_t)addr;
}

void *HostResolveGbaTableEntry(const GbaAddr *table, u32 index)
{
    GbaAddr a = 0;
    if (table == NULL)
        return NULL;
    memcpy(&a, &table[index], sizeof(GbaAddr));
    return (void *)(uintptr_t)a;
}
#endif

/* Identity layout-address table (index = mapLayoutId - 1). Sized to cover the
 * {441} distinct layout ids; entries are filled at load by the seam test. */
const GbaAddr gMapLayouts[512] = {0};