# Current desktop content to canonical resource inventory

This is an M0 planning inventory. It does not change the existing
`desktop_game_content.c` IDs, ROM offsets, package, or hydration behavior.

Several current entries are aggregate ROM tables. The stable catalog should
fan those tables out into semantic resources rather than preserve the current
numeric package entry as a public identity.

| Current entry | Proposed canonical family |
| --- | --- |
| `CONTENT_SPECIES_NAMES` | `gen3:species/<species>/name/en` |
| `CONTENT_MOVE_NAMES` | `gen3:move/<move>/name/en` |
| `CONTENT_BATTLE_MOVES` | `gen3:move/<move>/battle-data` (future structured-data schema) |
| `CONTENT_EXPERIENCE_TABLES` | `gen3:growth-rate/<rate>/experience-table` (future structured-data schema) |
| `CONTENT_SPECIES_INFO` | `gen3:species/<species>/battle-data` (future structured-data schema) |
| `CONTENT_FONT_SMALL_NARROW_LATIN` | `gen3:font/small-narrow/latin/glyphs` |
| `CONTENT_FONT_SMALL_LATIN` | `gen3:font/small/latin/glyphs` |
| `CONTENT_FONT_NARROW_LATIN` | `gen3:font/narrow/latin/glyphs` |
| `CONTENT_FONT_SHORT_LATIN` | `gen3:font/short/latin/glyphs` |
| `CONTENT_FONT_NORMAL_LATIN` | `gen3:font/normal/latin/glyphs` |
| `CONTENT_FONT_SMALL_JAPANESE` | `gen3:font/small/japanese/glyphs` |
| `CONTENT_FONT_NORMAL_JAPANESE` | `gen3:font/normal/japanese/glyphs` |
| `CONTENT_FONT_FRLG_MALE_JAPANESE` | `gen3:font/frlg-male/japanese/glyphs` pending shared-semantics audit |
| `CONTENT_FONT_FRLG_FEMALE_JAPANESE` | `gen3:font/frlg-female/japanese/glyphs` pending shared-semantics audit |
| `CONTENT_FONT_SHORT_JAPANESE` | `gen3:font/short/japanese/glyphs` |

The two Stage M0 proof catalog bindings are:

| Existing Emerald binding | Canonical ID |
| --- | --- |
| `gTrainerFrontPicTable[TRAINER_PIC_BRENDAN]` | `emerald:trainer/brendan/battle/front/sheet` |
| `gTrainerFrontPicPaletteTable[TRAINER_PIC_BRENDAN]` | `emerald:trainer/brendan/battle/front/normal-palette` |

The first five proposed families are not enabled by the M1 generic resource
core because their structured schemas belong to the later data-modding phase.
The two FRLG-named font entries remain subject to an actual cross-game semantic
audit before their `gen3:` namespace is frozen.
