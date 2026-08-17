#include "global.h"
#include "platform/desktop_runtime.h"
#include "platform/host_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void abort(void);

#define HOST_HANDLE_BASE 0xF0000000u
#define HOST_FUNCTION_HANDLE_BASE 0xE0000000u
#define HOST_HANDLE_INDEX_MASK 0x0000FFFFu
#define HOST_HANDLE_CAPACITY (HOST_HANDLE_INDEX_MASK + 1)

#define HOST_PERSISTENT_DATA_LOGICAL 0x50444C47u /* PDLG */
#define HOST_PERSISTENT_DATA_IMAGE   0x5044494Du /* PDIM */
#define HOST_PERSISTENT_FUNC_LOGICAL 0x50464C47u /* PFLG */
#define HOST_PERSISTENT_FUNC_IMAGE   0x5046494Du /* PFIM */
#define HOST_PERSISTENT_FUNCTION_CAPACITY 16384

struct HostPersistentFunctionEntry
{
    u32 stableId;
    uintptr_t native;
};

// The handle tables map host pointers/functions into the GBA-shaped 32-bit
// address space and are only referenced by the UINTPTR_MAX > UINT32_MAX code
// paths below (HostPointerToGbaAddr/HostResolveGbaAddr/HostFunctionToGbaAddr/
// HostResolveFunction). On GBA the pointer space IS 32-bit, so the identity
// branches run and the tables are dead -- but the two 256 KiB arrays would
// still land in .bss and overflow IWRAM, so they are excluded there.
#if UINTPTR_MAX > UINT32_MAX
HOST_DATA static const void *sHostPointers[HOST_HANDLE_CAPACITY];
HOST_DATA static unsigned char sHostFunctions[HOST_HANDLE_CAPACITY][sizeof(uintptr_t)];
HOST_DATA static u32 sNextHostHandle = 1;
HOST_DATA static u32 sNextHostFunctionHandle = 1;
#endif
#if defined(LINUX64) && LINUX64
HOST_DATA static struct HostPersistentFunctionEntry sPersistentFunctions[HOST_PERSISTENT_FUNCTION_CAPACITY];
HOST_DATA static u32 sPersistentFunctionCount;

extern unsigned char __start_host_data[];
extern unsigned char __stop_host_data[];
#endif

STATIC_ASSERT(sizeof(GbaAddr) == 4, GbaAddrSize);
STATIC_ASSERT(sizeof(GbaOffset) == 4, GbaOffsetSize);
STATIC_ASSERT(sizeof(struct HostPersistentAddress) == 8, HostPersistentAddressSize);

static void HostMemoryAbort(const char *message, uintptr_t addr)
{
#ifdef PORTABLE
    fprintf(stderr, "host memory error: %s (address=0x%zx)\n", message, addr);
    abort();
#else
    // GBA build: this reports native-only failures (handle-table exhaustion,
    // invalid handle, unrepresentable pointer). On the GBA the 32-bit identity
    // branches run, so these paths are unreachable in practice. Hang rather than
    // pull libc fprintf/abort (and the syscalls.o '_sbrk'/'end' dependency they
    // drag into the image) into the GBA ROM.
    (void)message;
    (void)addr;
    while (1)
        ;
#endif
}

static void HostMemoryRegionError(const char *owner, const char *region, size_t regionSize)
{
#ifdef PORTABLE
    fprintf(stderr, "host memory error: %s exceeds %s (%zu bytes)\n", owner, region, regionSize);
    abort();
#else
    (void)owner;
    (void)region;
    (void)regionSize;
    while (1)
        ;
#endif
}

#if defined(LINUX64) && LINUX64
static bool32 HostIsGameImageAddress(uintptr_t native)
{
    uintptr_t imageStart;
    uintptr_t imageEnd;

    return Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
        && native >= imageStart
        && native < imageEnd
        && !(native >= (uintptr_t)__start_host_data
          && native < (uintptr_t)__stop_host_data);
}

static bool32 HostIsExecutableAddress(uintptr_t native)
{
    return Platform_RuntimeAddressIsExecutable(native);
}

static bool32 HostRegisterPersistentFunction(u32 stableId, uintptr_t native)
{
    u32 i;

    for (i = 0; i < sPersistentFunctionCount; i++)
    {
        if (sPersistentFunctions[i].stableId == stableId)
            return sPersistentFunctions[i].native == native;
        if (sPersistentFunctions[i].native == native)
            return FALSE;
    }
    if (sPersistentFunctionCount >= HOST_PERSISTENT_FUNCTION_CAPACITY)
        return FALSE;
    sPersistentFunctions[sPersistentFunctionCount].stableId = stableId;
    sPersistentFunctions[sPersistentFunctionCount].native = native;
    sPersistentFunctionCount++;
    return TRUE;
}
#endif

bool32 HostPointerToPersistentAddress(const void *ptr, struct HostPersistentAddress *persistent)
{
    uintptr_t native = (uintptr_t)ptr;

    if (persistent == NULL)
        return FALSE;
    persistent->value = 0;
    persistent->kind = 0;
    if (ptr == NULL)
        return TRUE;
#if defined(LINUX64) && LINUX64
    uintptr_t imageStart;
    uintptr_t imageEnd;

    if (!Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
     || !HostIsGameImageAddress(native) || HostIsExecutableAddress(native)
     || native < imageStart
     || native - imageStart > UINT32_MAX)
        return FALSE;
    persistent->value = (u32)(native - imageStart);
    persistent->kind = HOST_PERSISTENT_DATA_IMAGE;
    return TRUE;
#else
    if (native > UINT32_MAX)
        return FALSE;
    persistent->value = (u32)native;
    persistent->kind = HOST_PERSISTENT_DATA_LOGICAL;
    return TRUE;
#endif
}

bool32 HostResolvePersistentAddress(const struct HostPersistentAddress *persistent, void **ptr)
{
    uintptr_t native;

    if (persistent == NULL || ptr == NULL)
        return FALSE;
    if (persistent->kind == 0 && persistent->value == 0)
    {
        *ptr = NULL;
        return TRUE;
    }
#if defined(LINUX64) && LINUX64
    uintptr_t imageStart;
    uintptr_t imageEnd;

    if (persistent->kind == HOST_PERSISTENT_DATA_LOGICAL)
        native = persistent->value;
    else if (persistent->kind == HOST_PERSISTENT_DATA_IMAGE
          && Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
          && persistent->value <= UINTPTR_MAX - imageStart)
        native = imageStart + persistent->value;
    else
        return FALSE;
    if (!HostIsGameImageAddress(native) || HostIsExecutableAddress(native))
        return FALSE;
#else
    if (persistent->kind != HOST_PERSISTENT_DATA_LOGICAL)
        return FALSE;
    native = persistent->value;
#endif
    *ptr = (void *)native;
    return TRUE;
}

bool32 HostPersistentAddressIsData(const struct HostPersistentAddress *persistent)
{
    return persistent != NULL
        && ((persistent->kind == 0 && persistent->value == 0)
         || persistent->kind == HOST_PERSISTENT_DATA_LOGICAL
         || persistent->kind == HOST_PERSISTENT_DATA_IMAGE);
}

bool32 HostFunctionToPersistentAddress(const void *functionPointerBytes, size_t size,
                                       struct HostPersistentAddress *persistent)
{
    uintptr_t native = 0;

    if (functionPointerBytes == NULL || persistent == NULL
     || size == 0 || size > sizeof(native))
        return FALSE;
    memcpy(&native, functionPointerBytes, size);
    persistent->value = 0;
    persistent->kind = 0;
    if (native == 0)
        return TRUE;
#if defined(LINUX64) && LINUX64
    uintptr_t imageStart;
    uintptr_t imageEnd;

    if (!HostIsGameImageAddress(native) || !HostIsExecutableAddress(native)
     || !Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
     || native < imageStart || native - imageStart > UINT32_MAX)
        return FALSE;
    persistent->value = (u32)(native - imageStart);
    persistent->kind = HOST_PERSISTENT_FUNC_IMAGE;
    return HostRegisterPersistentFunction(persistent->value, native);
#else
    if (native > UINT32_MAX)
        return FALSE;
    persistent->value = (u32)native;
    persistent->kind = HOST_PERSISTENT_FUNC_LOGICAL;
    return TRUE;
#endif
}

bool32 HostResolvePersistentFunction(const struct HostPersistentAddress *persistent,
                                     void *functionPointerBytes, size_t size)
{
    uintptr_t native;

    if (persistent == NULL || functionPointerBytes == NULL
     || size == 0 || size > sizeof(native))
        return FALSE;
    if (persistent->kind == 0 && persistent->value == 0)
    {
        memset(functionPointerBytes, 0, size);
        return TRUE;
    }
#if defined(LINUX64) && LINUX64
    uintptr_t imageStart;
    uintptr_t imageEnd;

    if (persistent->kind == HOST_PERSISTENT_FUNC_LOGICAL)
        native = persistent->value;
    else if (persistent->kind == HOST_PERSISTENT_FUNC_IMAGE
          && Platform_RuntimeGetImageRange(&imageStart, &imageEnd)
          && persistent->value <= UINTPTR_MAX - imageStart)
        native = imageStart + persistent->value;
    else
        return FALSE;
    if (!HostIsGameImageAddress(native) || !HostIsExecutableAddress(native)
     || !HostRegisterPersistentFunction(persistent->value, native))
        return FALSE;
#else
    if (persistent->kind != HOST_PERSISTENT_FUNC_LOGICAL)
        return FALSE;
    native = persistent->value;
#endif
    memset(functionPointerBytes, 0, size);
    memcpy(functionPointerBytes, &native, size);
    return TRUE;
}

bool32 HostPersistentAddressIsFunction(const struct HostPersistentAddress *persistent)
{
    return persistent != NULL
        && ((persistent->kind == 0 && persistent->value == 0)
         || persistent->kind == HOST_PERSISTENT_FUNC_LOGICAL
         || persistent->kind == HOST_PERSISTENT_FUNC_IMAGE);
}

GbaAddr HostPointerToGbaAddr(const void *ptr)
{
    uintptr_t native;
    u32 i;

    if (ptr == NULL)
        return 0;

#if UINTPTR_MAX <= UINT32_MAX
    return (GbaAddr)(uintptr_t)ptr;
#else
    native = (uintptr_t)ptr;

    // Non-PIE executable and generated-data addresses are intentionally kept
    // in the logical 32-bit GBA address space. This is also required for
    // pointer-bearing state that can be copied into a save block and loaded
    // by a later process. Only transient host allocations need handles.
    if (native <= UINT32_MAX)
        return (GbaAddr)native;

    for (i = 1; i < sNextHostHandle; i++)
    {
        if (sHostPointers[i] == ptr)
            return HOST_HANDLE_BASE | i;
    }

    if (sNextHostHandle >= HOST_HANDLE_CAPACITY)
        HostMemoryAbort("host pointer handle table exhausted", native);

    sHostPointers[sNextHostHandle] = ptr;
    return HOST_HANDLE_BASE | sNextHostHandle++;
#endif
}

void *HostResolveGbaAddr(GbaAddr addr)
{
    u32 index;

    if (addr == 0)
        return NULL;

#if UINTPTR_MAX > UINT32_MAX
    if ((addr & 0xFFFF0000u) == HOST_HANDLE_BASE)
    {
        index = addr & HOST_HANDLE_INDEX_MASK;
        if (index == 0 || index >= sNextHostHandle || sHostPointers[index] == NULL)
            HostMemoryAbort("invalid host pointer handle", addr);
        return (void *)sHostPointers[index];
    }
#endif

    // Non-handle values are generated four-byte symbolic references. The
    // linux64 link is non-PIE and keeps vanilla generated data below 4 GiB;
    // retaining this explicit conversion makes the contract visible and
    // allows a future relocation table to replace it without changing users.
    return (void *)(uintptr_t)addr;
}

void *HostResolveGbaTableEntry(const GbaAddr *table, u32 index)
{
    GbaAddr logicalAddr;

    memcpy(&logicalAddr, &table[index], sizeof(logicalAddr));
    return HostResolveGbaAddr(logicalAddr);
}

GbaAddr HostFunctionToGbaAddr(const void *functionPointerBytes, size_t size)
{
    uintptr_t native = 0;
    u32 i;

    if (functionPointerBytes == NULL || size == 0 || size > sizeof(native))
        HostMemoryAbort("invalid function pointer representation", 0);

    memcpy(&native, functionPointerBytes, size);

#if UINTPTR_MAX <= UINT32_MAX
    return (GbaAddr)native;
#else
    if (native <= UINT32_MAX)
        return (GbaAddr)native;

    for (i = 1; i < sNextHostFunctionHandle; i++)
    {
        if (memcmp(sHostFunctions[i], functionPointerBytes, size) == 0)
            return HOST_FUNCTION_HANDLE_BASE | i;
    }

    if (sNextHostFunctionHandle >= HOST_HANDLE_CAPACITY)
        HostMemoryAbort("host function handle table exhausted", 0);

    memcpy(sHostFunctions[sNextHostFunctionHandle], functionPointerBytes, size);
    return HOST_FUNCTION_HANDLE_BASE | sNextHostFunctionHandle++;
#endif
}

void HostResolveFunction(GbaAddr addr, void *functionPointerBytes, size_t size)
{
    uintptr_t native;
    u32 index;

    if (functionPointerBytes == NULL || size == 0 || size > sizeof(native))
        HostMemoryAbort("invalid function pointer destination", addr);

#if UINTPTR_MAX > UINT32_MAX
    if ((addr & 0xFFFF0000u) == HOST_FUNCTION_HANDLE_BASE)
    {
        index = addr & HOST_HANDLE_INDEX_MASK;
        if (index == 0 || index >= sNextHostFunctionHandle)
            HostMemoryAbort("invalid host function handle", addr);
        memcpy(functionPointerBytes, sHostFunctions[index], size);
        return;
    }
#endif

    native = addr;
    memset(functionPointerBytes, 0, size);
    memcpy(functionPointerBytes, &native, size);
}

bool32 HostAddressIsRuntimeHandle(GbaAddr addr)
{
#if UINTPTR_MAX > UINT32_MAX
    return (addr & 0xFFFF0000u) == HOST_FUNCTION_HANDLE_BASE
        || (addr & 0xFFFF0000u) == HOST_HANDLE_BASE;
#else
    (void)addr;
    return FALSE;
#endif
}

bool32 HostAddressIsRegisteredRuntimeHandle(GbaAddr addr)
{
#if UINTPTR_MAX > UINT32_MAX
    u32 index = addr & HOST_HANDLE_INDEX_MASK;

    if ((addr & 0xFFFF0000u) == HOST_HANDLE_BASE)
        return index != 0 && index < sNextHostHandle && sHostPointers[index] != NULL;
    if ((addr & 0xFFFF0000u) == HOST_FUNCTION_HANDLE_BASE)
        return index != 0 && index < sNextHostFunctionHandle;
#else
    (void)addr;
#endif
    return FALSE;
}

#if UINTPTR_MAX > UINT32_MAX
void HostMemoryGetHandleCounters(u32 *dataHandles, u32 *functionHandles)
{
    if (dataHandles != NULL)
        *dataHandles = sNextHostHandle - 1;
    if (functionHandles != NULL)
        *functionHandles = sNextHostFunctionHandle - 1;
}
#endif

void HostAssertMemoryRange(const void *ptr, size_t size, const char *owner)
{
    uintptr_t start = (uintptr_t)ptr;
    uintptr_t end;
    uintptr_t regionStart;
    uintptr_t regionEnd;

    if (ptr == NULL && size != 0)
    {
#ifdef PORTABLE
        fprintf(stderr, "host memory error: %s received NULL for %zu bytes\n", owner, size);
        abort();
#else
        HostMemoryAbort("received NULL range", start);
#endif
    }

    if (size > UINTPTR_MAX - start)
    {
#ifdef PORTABLE
        fprintf(stderr, "host memory error: %s range overflow\n", owner);
        abort();
#else
        HostMemoryAbort("range overflow", start);
#endif
    }
    end = start + size;

#define CHECK_REGION(region, regionSize) \
    do { \
        regionStart = (uintptr_t)(region); \
        regionEnd = regionStart + (regionSize); \
        if (start >= regionStart && start <= regionEnd && end > regionEnd) \
            HostMemoryRegionError(owner, #region, (size_t)(regionSize)); \
    } while (0)

    if (size != 0)
    {
        CHECK_REGION(REG_BASE, 0x400);
        CHECK_REGION(VRAM, VRAM_SIZE);
        CHECK_REGION(PLTT, PLTT_SIZE);
        CHECK_REGION(OAM, OAM_SIZE);
    }

#undef CHECK_REGION
}
