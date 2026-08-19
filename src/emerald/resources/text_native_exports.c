// Native-only exports for seam-owned compiled constants that are `static` in
// their GBA-compiled sources.
//
// The generated skeleton tables (text_skeleton_arrays.generated.c) reference
// these by extern (kind-2 sub-table pointers / label rows).  In the GBA build
// each referencing TU gets its own copy via the static definitions in
// union_room.h (sText_Info/sText_Exit) and pokenav_match_call_data.c
// (sWallyLocationData); the native build compiles the generated tables in
// their own TU, so a single external definition must exist here.
//
// These files stay untouched (GBA parity); this TU is native-only
// (src/emerald/resources/*.c is filtered out of the GBA build in the pret
// Makefile).  Content mirrors the GBA definitions exactly.
#include "global.h"
#include "match_call.h"
#include "event_data.h"
#include "constants/region_map_sections.h"

ALIGNED(4) const u8 sText_Info[] = _("INFO");
ALIGNED(4) const u8 sText_Exit[] = _("EXIT");

const struct MatchCallLocationOverride sWallyLocationData[] = {
    { FLAG_HIDE_MAUVILLE_CITY_WALLY, MAPSEC_VERDANTURF_TOWN },
    { FLAG_GROUDON_AWAKENED_MAGMA_HIDEOUT, MAPSEC_NONE },
    { FLAG_HIDE_VICTORY_ROAD_ENTRANCE_WALLY, MAPSEC_VICTORY_ROAD },
    { 0xFFFF, MAPSEC_NONE }
};
