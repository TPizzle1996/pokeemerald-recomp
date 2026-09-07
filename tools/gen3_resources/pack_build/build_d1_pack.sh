#!/bin/bash
# R13-D1: build the production emerald-bpee01-v1.rpack from all manifest+catalog
# sources (12 manifests + 12 catalogs now; R13-G2 added the script-module
# family at script/modules/, R13-H2 adds the battle-module family at
# battle/modules/; gameplay excluded the diverged evolution family).
# Deterministic; pass --check to verify byte-identical.
root="${1:-.}"
check="${2:-}"
cd "$root" || exit 2
make -C tools/gen3_resources/pack_build all >/dev/null 2>&1
set --
cmd=(tools/gen3_resources/pack_build/gen3-pack-build
  --rom ../pokeemerald-reference/pokeemerald.gba
  --output games/emerald/base/emerald-bpee01-v1.rpack
  --manifest resources/extraction/emerald/bpee01/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/pokemon_battle/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/object_event/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/tileset/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/layout/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/audio/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/movement/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/multiboot/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/text/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/gameplay/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/script/modules/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/battle/modules/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/battle_anim_gfx/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/item_icon/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/wallpaper/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/weather/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/ui/manifest.production.toml
  --manifest resources/extraction/emerald/bpee01/easy_chat/manifest.production.toml
  --catalog resources/catalogs/emerald/catalog.toml
  --catalog resources/extraction/emerald/bpee01/pokemon_battle/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/object_event/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/tileset/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/layout/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/audio/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/movement/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/multiboot/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/text/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/gameplay/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/script/modules/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/battle/modules/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/battle_anim_gfx/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/item_icon/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/wallpaper/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/weather/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/ui/catalog.generated.toml
  --catalog resources/extraction/emerald/bpee01/easy_chat/catalog.generated.toml
)
if [ -n "$check" ]; then cmd+=(--check); fi
"${cmd[@]}" "$@" 2>&1 | tail -4