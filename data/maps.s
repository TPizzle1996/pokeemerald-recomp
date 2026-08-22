#include "constants/global.h"
#include "constants/layouts.h"
#include "constants/map_types.h"
#include "constants/maps.h"
#include "constants/weather.h"
#include "constants/region_map_sections.h"
#include "constants/songs.h"
#include "constants/trainer_hill.h"
	.include "asm/macros.inc"
	.include "constants/constants.inc"

	.section .rodata

	.include "data/layouts/layouts.inc"
	.include "data/layouts/layouts_table.inc"
	.if (NATIVE_LINUX == 1) && (LINUX64 == 1)
	/* R13-G6: headers.inc (the 518 per-map header structs incl. the 469
	 * *_MapScripts slots) and connections.inc are dead compiled shadows
	 * on LINUX64 - gMapHeaders is the HOST_DATA host array and every
	 * script surface is the pack-loaded arena generation the R13-F seam
	 * rebinds. The GBA build keeps both. */
	.else
	.include "data/maps/headers.inc"
	.endif
	.if (NATIVE_LINUX == 1) && (LINUX64 == 1)
	/* R13-G6: groups_native.inc (generated) points the gMapGroup_* tables
	 * at the R13-F seam array gMapHeaders (HOST_DATA, 518 x 48-byte rows)
	 * by routing index, so the engine's address-identity map lookup
	 * (Overworld_GetMapHeaderByGroupAndId) resolves to the published seam
	 * rows - arena events, staged routing mapScripts - instead of the
	 * compiled header structs (headers.inc is excluded above). The GBA
	 * build keeps groups.inc. */
	.include "data/maps/groups_native.inc"
	.else
	.include "data/maps/groups.inc"
	.endif
	.if (NATIVE_LINUX == 1) && (LINUX64 == 1)
	.else
	.include "data/maps/connections.inc"
	.endif
