/* R13-F: native map-metadata fill target definitions (native-only).
 *
 * Defines the HOST_DATA `struct MapHeader gMapHeaders[518]` array the
 * EmeraldMapCompat seam publishes from the pack (see the header). The event
 * and connection native arrays are seam-owned arenas (allocated in
 * emerald_map_compat.c), not fixed arrays here, exactly like the trainer
 * party / encounter slot arenas.
 */

#include "global.h"
#include "emerald/resources/map_data_native.h"

HOST_DATA struct MapHeader gMapHeaders[EMERALD_MAP_HEADER_COUNT] = {0};

/* Static assert on the native row size the seam writes and the loader/state
 * walk relies on (MapHeader must be 48 bytes on the host). */
_Static_assert(sizeof(struct MapHeader) == 0x30,
               "MapHeader native size must be 48");