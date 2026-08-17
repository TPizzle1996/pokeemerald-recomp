/* R11-E/F: Harness B/C/D Overworld accessor stub + SaveBlock1 fixture.
 *
 * The neighborhood module resolves map headers through
 * Overworld_GetMapHeaderByGroupAndId (overworld.h). The harnesses link the
 * REAL compiled map tables (build/linux64/data/maps.o) instead of the game's
 * overworld.c, so this TU provides the accessor whose body is the verbatim
 * PORTABLE branch (overworld.c:595-603):
 *
 *     const GbaAddr *mapGroupTable = HostResolveGbaAddr(gMapGroups[mapGroup]);
 *     return HostResolveGbaTableEntry(mapGroupTable, mapNum);
 *
 * `gMapGroups` is maps.o's 34-entry GbaAddr table (data/maps/groups.inc, bare
 * `gba_ptr` rows, no per-group count word; MAP_GROUPS_COUNT,
 * constants/map_groups.h:594). The compiled map headers carry REAL host
 * pointers in their mapLayout/events/connections fields (host_ptr .quad
 * emission on LINUX64, data/maps/<map>/header.inc), and on linux64
 * HostResolveGbaAddr is the identity conversion (host_memory.c:308-326) --
 * valid only in a non-PIE link (< 4 GiB), which the runners use (-no-pie).
 *
 * The real `struct SaveBlock1` fixture is shared here too: the module reads
 * gSaveBlock1Ptr->location.{mapGroup,mapNum} (global.h:1003-1006, 1100; all
 * map changes funnel through ApplyCurrentWarp -> location).
 */

#include "global.h"
#include "overworld.h"
#include "platform/host_memory.h"

/* maps.o's GbaAddr table (data/maps.s). */
extern const GbaAddr gMapGroups[];

struct MapHeader const *const Overworld_GetMapHeaderByGroupAndId(u16 mapGroup, u16 mapNum)
{
    const GbaAddr *mapGroupTable = HostResolveGbaAddr(gMapGroups[mapGroup]);
    return HostResolveGbaTableEntry(mapGroupTable, mapNum);
}

static struct SaveBlock1 gSaveBlock1;
struct SaveBlock1 *gSaveBlock1Ptr = &gSaveBlock1;
