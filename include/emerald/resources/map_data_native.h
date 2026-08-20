#ifndef EMERALD_RESOURCES_MAP_DATA_NATIVE_H
#define EMERALD_RESOURCES_MAP_DATA_NATIVE_H

/* R13-F: native map-metadata fill target.
 *
 * Declares the HOST_DATA array the publication seam (emerald_map_compat.c)
 * fills from the production pack: `struct MapHeader gMapHeaders[518]`
 * (48-byte native rows, the mapLayout/mapScripts/events/connections pointers
 * rebuilt to the seam's published layout/event/connection tables). The event
 * and connection native arrays live in seam-owned arenas (like the trainer
 * party / encounter slot arenas), not fixed arrays here. The definition lives
 * in src/emerald/resources/map_data_native.c (native-only); this header is
 * included by BOTH the definition TU and the seam TU, and by the loader/state
 * walk when it needs the published header span.
 *
 * The struct here is the engine's real `struct MapHeader` (from
 * include/global.fieldmap.h), so the seam writes the exact layout engine
 * consumers read through global.fieldmap.h. sizeof under LINUX64 == 48
 * (0x30), checked by the inline struct's STATIC_ASSERT.
 */

#include "gba/types.h"
#include "gba/defines.h"
/* The engine's real `struct MapHeader` (48 bytes native) lives in
 * include/global.fieldmap.h, pulled through global.h exactly as the other
 * seam TUs obtain engine structs (see emerald_object_event_compat.c). */
#include "global.h"

/* Routing-order header count (=== the (mapGroup,mapNum) bijection size). */
#define EMERALD_MAP_HEADER_COUNT 518u

/* The native fill target (see the header comment). Declared unconditionally:
 * global.fieldmap.h provides the real struct. */
extern HOST_DATA struct MapHeader gMapHeaders[EMERALD_MAP_HEADER_COUNT];

#endif /* EMERALD_RESOURCES_MAP_DATA_NATIVE_H */