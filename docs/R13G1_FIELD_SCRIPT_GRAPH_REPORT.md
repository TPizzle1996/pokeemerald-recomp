# R13-G1 — Field Script Graph Report

Status: **complete**. This was an additive extractor/oracle stage only. No
runtime script migration, G2 work, or R13-H work was started.

## Qualified closure

The qualified BPEE01 ROM (`f3ae088181bf583e55daf962a92bb46f4f1d07b7`) and
ELF relocation sources close exactly over 523 semantic modules (468 map, 47
common/shared, and eight ordinary-field mystery-gift modules):

| Metric | Result |
|---|---:|
| Static field bytecode | 207,330 B (206,638 main + 692 mystery gift) |
| Address operands | 16,704 |
| SCRIPT_TARGET | 8,208 |
| TEXT_TARGET | 6,207 |
| MOVEMENT_TARGET | 2,009 |
| STATIC_DATA | 262 |
| RAM_DATA | 18 |
| Script root / interior occurrences | 95 / 8,113 |
| Unresolved / ambiguous | 0 / 0 |
| Map tables / conditional tables | 470 / 158 |
| Logical map rows / unique leaf targets | 919 / 540 |

The 227-opcode grammar and 13 `trainerbattle` shapes are accepted. The
extractor assigns sources from the qualified relocation table rather than a
contiguous bytecode interval; routing and typed-data sources therefore remain
in the graph even when their target is R13-C text or R13-B movement.

**Byte-total correction (G2-verified, 2026-08-20):** the former 206,283 B
figure (205,591 main + 692 gift) was recomp-era bookkeeping derived from
stale/pre-correction R13-C text accounting and is reproducible from neither
current build. The qualified main-object partition closes at 206,638 B G
bytecode + 749,565 B text + 7,416 B movement + 5,749 B routing + 3,152 B
engine = 972,520 B, so the authoritative G-owned bytecode is **207,330 B =
206,638 main + 692 mystery gift**. The 1,047-B delta vs the former figure is
the qualified-vs-recomp text inventory difference (the current binding
catalog matches R13-C's pre-correction 747,007 B on the recomp ELF; the
recomp build itself measures 207,074 B with the identical model). See
`docs/R13G_FIELD_SCRIPT_MIGRATION_PLAN.md` §18.1 for the full
reconciliation. All graph pins below are unaffected and re-check green.

`gStdScripts` has 11 G routing bindings. They are included in the qualified
graph census but deliberately outside the 16,704 canonical operand count,
whose main-object boundary begins after the 44-byte provenance table.

## Interior proof

Every script-target relocation resolves to exactly one of the 523 module
spans and a validated export/instruction boundary. There are 95 offset-zero
exports and 8,113 nonzero module-relative bindings. No target resolves into a
text, movement, or data hole; no containing module is ambiguous. The emitted
proof is `resources/extraction/emerald/bpee01/script/interior_target_proof.generated.toml`.

## Excluded relocation census

The qualified universe contains 17,497 relocations. It closes as 16,704
canonical G operands, 11 included `gStdScripts` routing bindings, and 782
explicit exclusions: 777 command/special engine-routing-prefix `R_ARM_ABS32`
records and five mystery-gift `R_ARM_ABS16` SPECIAL-ID scalars. The latter are
not addresses. Unexplained remainder and duplicate accounting are both zero.
Each excluded source is emitted with address, object/section, type, target
symbol or scalar, reason, and owner in
`resources/extraction/emerald/bpee01/script/qualified_relocation_census.generated.toml`.

## Gates

- Extractor/oracle: PASS; exact graph assertions above.
- Grammar fixtures: PASS.
- `field_script_extractor.py --check`: PASS.
- Deterministic second generation: PASS.
- R13-F CoordEvent wire regression remains green (script offset 12; prior
  real-table/native-world proof remains the accepted F regression gate).

## G2 prerequisites

G2 may consume, but must not alter without a new proof, the G1 module
partition, typed relocation sidecars, module-relative interior bindings,
R13-C text identities, R13-B movement bridge targets, map-routing rows, and
the qualified exclusion census. G2 begins additive resource emission only;
it must preserve the 16,704 partition and 95/8,113 script-binding split.

**STOP — R13-G1 complete. No commit made.**
