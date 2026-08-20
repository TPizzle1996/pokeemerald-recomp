#ifndef EMERALD_RESOURCES_GAMEPLAY_ITEM_CALLBACKS_NATIVE_H
#define EMERALD_RESOURCES_GAMEPLAY_ITEM_CALLBACKS_NATIVE_H

/* R13-D2: native item-use callback resolution (ENGINE_CONSTANT metadata).
 *
 * The 27 distinct GBA use-function addresses across the 377 gItems rows are
 * enumerated in gameplay_callbacks.generated.{h,c} as a stable semantic
 * `enum GameplayItemUseAction` (kGameplayItemCallbacks: gbaAddr -> action ->
 * symbol). The resolver maps each action to the native `ItemUseFunc` the
 * engine link provides (src/item_use.c), so the publication seam
 * (emerald_gameplay_compat.c) can resolve an item row's two GBA addresses
 * (fieldUseFunc @28 / battleUseFunc @36) to a live native function pointer.
 *
 * This table is engine metadata, NOT a resource: it is compiled into the
 * native link and never stored in the pack. No native function pointer ever
 * appears in a resource payload (the pack holds the exact 44-byte ROM rows,
 * GBA addresses included; the seam does the validation + resolution).
 */

#include "emerald/resources/gameplay_callbacks.generated.h"
#include "emerald/resources/gameplay_data_native.h" /* GameplayItemUseFunc */

/* Resolve a semantic action to its native item-use function. Every action in
 * the 27-value census maps to exactly one engine function; the resolver is a
 * direct switch (falling back to NULL only on an out-of-range action, which
 * a validated seam can never ask for). The engine table is complete: the
 * reference census is the 27 kGameplayItemCallbacks rows, so no native
 * function is added or dropped here unless the census changes. The return
 * type is `void (*)(u8)` (GameplayItemUseFunc), the same function-pointer
 * type as struct Item::fieldUseFunc/battleUseFunc. */
GameplayItemUseFunc GameplayItemUseFunctionForAction(enum GameplayItemUseAction action);

#endif /* EMERALD_RESOURCES_GAMEPLAY_ITEM_CALLBACKS_NATIVE_H */