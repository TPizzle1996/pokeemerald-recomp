#ifndef GUARD_PLATFORM_HOST_MEMORY_H
#define GUARD_PLATFORM_HOST_MEMORY_H

#include <stddef.h>
#include "gba/types.h"

/* Runtime handles and persistent identities deliberately use different types
 * and namespaces. Runtime 0xE/0xF handles are process-local; this record is
 * the only pointer representation permitted in native state files. */
struct HostPersistentAddress
{
    u32 value;
    u32 kind;
};

// A host pointer which must cross a GBA-shaped 32-bit field is represented by
// a short-lived handle on 64-bit hosts. Generated fixed-width addresses are
// resolved through the same boundary.
GbaAddr HostPointerToGbaAddr(const void *ptr);
void *HostResolveGbaAddr(GbaAddr addr);
/* Exact-start logical-address table (R12-C §5): audio table labels (voicegroup
 * and cry-table starts, 197 entries) whose GBA address must resolve to the
 * R12-B/C arena's native rows instead of the compiled parity copy. The table
 * is consulted before the handle table, so entries must never overlap the
 * handle ranges; registration inserts in sorted order (publish-time only) and
 * aborts on duplicates. Cleared by ClearMigratedEntries/Republish. */
void HostMemoryRegisterLogicalAddress(GbaAddr addr, void *hostBase);
void HostMemoryClearLogicalAddresses(void);
/* Interval logical-address table (R12-D §5): ONE half-open [romStart, romEnd)
 * GBA interval -> native base, covering the contiguous MP2K song-graph block
 * (0x088FC03C..0x089A3050). Resolution is identity-preserving
 * (hostBase + (addr - romStart)). Consulted after the exact-start table and
 * before the handle table; registration inserts in sorted order (publish-time
 * only), aborts on any overlap with an existing interval AND on any interval
 * that would contain an exact-start entry (the exact-start table must always
 * win). Fixed capacity 16 - a whole zone or per-song interval set must never
 * be registered here (per-song identity lives in the R10 range index). */
void HostMemoryRegisterLogicalRange(GbaAddr romStart, GbaAddr romEnd, void *hostBase);
void HostMemoryClearLogicalRanges(void);
void *HostResolveGbaTableEntry(const GbaAddr *table, u32 index);
GbaAddr HostFunctionToGbaAddr(const void *functionPointerBytes, size_t size);
void HostResolveFunction(GbaAddr addr, void *functionPointerBytes, size_t size);
bool32 HostAddressIsRuntimeHandle(GbaAddr addr);
bool32 HostAddressIsRegisteredRuntimeHandle(GbaAddr addr);
void HostMemoryGetHandleCounters(u32 *dataHandles, u32 *functionHandles);
void HostAssertMemoryRange(const void *ptr, size_t size, const char *owner);

bool32 HostPointerToPersistentAddress(const void *ptr, struct HostPersistentAddress *persistent);
bool32 HostResolvePersistentAddress(const struct HostPersistentAddress *persistent, void **ptr);
bool32 HostPersistentAddressIsData(const struct HostPersistentAddress *persistent);
bool32 HostFunctionToPersistentAddress(const void *functionPointerBytes, size_t size,
                                       struct HostPersistentAddress *persistent);
bool32 HostResolvePersistentFunction(const struct HostPersistentAddress *persistent,
                                     void *functionPointerBytes, size_t size);
bool32 HostPersistentAddressIsFunction(const struct HostPersistentAddress *persistent);

// GNU C's typeof keeps this helper usable for the several callback signatures
// that are packed into vanilla two-halfword task fields.
#define HOST_FUNCTION_ADDR(function) \
    ({ __typeof__(&(function)) hostFunction = &(function); \
       HostFunctionToGbaAddr(&hostFunction, sizeof(hostFunction)); })

#endif // GUARD_PLATFORM_HOST_MEMORY_H
