# R13-G — Emerald Field-Script Ownership Migration Plan

Status: **architecture/inventory only; implementation has not started**

Baseline: `checkpoint-r13f-complete`

Qualified ROM: `../pokeemerald-reference/pokeemerald.gba`

Qualified ELF: `../pokeemerald-reference/pokeemerald.elf`

Qualified ROM SHA-1: `f3ae088181bf583e55daf962a92bb46f4f1d07b7`

R13-G owns Emerald field-script bytecode, its static control/data graph, and
the execution-state relocation needed to run that graph from Tallgrass
resources. It does not own battle scripts, battle-AI scripts, animation
scripts, movement bytecode, or immutable engine dispatch functions. This is a
VM migration, not a byte-copy wave.

The recommended design is a **hybrid graph-aware module resource**: exact ROM
bytes are grouped by their source/map module; exported entry points and
interior labels are sidecar bindings; every address operand has a typed
relocation record. At run time the unmodified 32-bit operand is resolved by a
single indexed resolver. No pointer is patched into bytecode and no operand is
widened.

## 1. Audit method and exact scope

The inventory was recomputed from the current tree, the qualified ROM/ELF,
the qualified `data/event_scripts.o` and `data/mystery_gift.o`, assembler
listings regenerated from the current sources, R13-B/C generated inventories,
and R13-F map/event records. The regenerated GBA sections match the qualified
ROM bytes: event-object SHA-256
`0e0d9b6efaadae37b094593e419b57bca81f0eaad1a2cdf8c8280d6e0e8df710`
and mystery-gift SHA-256
`bd3af08e344df858c359fb6a87d9fc5385cb300d65cefa05d5383d3d97326dd9`.
The `event_scripts.o` GBA `script_data` contribution is exactly `0xED6E8`
(972,520 bytes); the eight same-VM mystery-gift modules occupy `0xD2E` (3,374
bytes) in `.rodata`. The complete qualified ELF `script_data`
section is `0x1036F4` (1,062,644 bytes); the remaining 90,124 bytes are not
silently assigned to G and must be classified by R13-H/remaining cleanup.

### 1.1 Exact field ownership inventory

| Family | Count | Exact bytes/edges | R13-G disposition |
|---|---:|---:|---|
| Map source modules | 468 | included in the 206,638-byte main bytecode total | G module resources |
| Common/shared source modules | 47 | included in the same total | G module resources |
| Movement include | 1 | 7,416 physical bytes in this object; R13-B owns 1,047 pret labels/7,404 bytes and documents three recomp-local labels/12 bytes | not a G payload |
| Static mystery-gift field-script modules | 8 | 692 bytecode bytes + 2,682 text bytes | bytecode in G; text handed to C |
| **Semantic field-script resources** | **523** | **207,330 bytecode bytes** | 468 map + 47 common + 8 mystery-gift |
| Top-level map-script tables | 470 symbols | 5,749 bytes; 158 distinct conditional tables; 919 entry rows / 540 unique entry targets | G script-local routing segments |
| Object/NPC event entry references | 2,163 | 1,708 unique targets | F-to-G binding |
| Coordinate-event entry references | 289 | 194 unique non-null script targets | F-to-G binding; see STOP gate |
| BG/sign entry references | 531 | 359 unique script targets | F-to-G binding |
| `trainerbattle` instructions | 652 | 118 encoded continuation-script operands | field opcode/data parsing in G; battle VM remains H |
| Main object address operands | 16,651 | exact `R_ARM_ABS32` operands after the engine table prefix | typed sidecar relocations |
| Mystery-gift address operands | 53 | 33 script/vaddress + 20 text; five additional `R_ARM_ABS16` relocations are SPECIAL indices, not addresses | typed sidecar relocations |
| **All G address operands** | **16,704** | **8,208 script + 6,207 text + 2,009 movement + 262 static data + 18 writable RAM/data** | exact graph |
| Tail script-local text | — | 1,347 physical bytes at the end of the main object (rel 971,173..972,520) — verified in G2 to be 13 `gText_Save*`/`gText_Birch_*` labels | R13-C text (naming-rule class); part of the 749,565-B text total |
| Command/special routing prefix | 4 tables | 3,152 bytes | split below |

The 3,152-byte prefix is exact: the 228-slot GBA command table is 912 bytes
(227 opcodes plus sentinel), `gSpecialVars` is 88 bytes (22 entries),
`gSpecials` is 2,108 bytes (527 entries), and `gStdScripts` is 44 bytes (11
entries). Command handlers, special-variable addresses, and special callbacks
are `ENGINE_CONSTANT`. `gStdScripts` is a G-owned script routing table whose
11 pointer bindings must publish against the script arena; its exact 44 GBA
bytes remain provenance, not native pointer storage.

The main-object byte partition closes exactly (G2-verified against the
qualified ROM symbol table and the physical section layout): 3,152
engine-routing + 5,749 map routing + 206,638 field bytecode + 7,416 movement
+ 749,565 R13-C text = 972,520. Text = the R13-C catalog (7,914 symbols /
747,011 B in the current binding catalog) + the naming-rule aliases (130 B)
+ the data/text file labels outside the catalog (2,424 B), including the
1,347-byte tail (13 `gText_Save*`/`gText_Birch_*` labels). The partition is
a sum of typed classes, not contiguous ranges: text and movement labels are
interleaved with script labels throughout the object (verified by a
printable-string scan — only 53 B of string-like bytes exist outside the
text class), so a G module is a list of typed segments, not a claim over a
broad ROM interval.

The physical source partition is intentionally not the pack schema. Text and
movement embedded between script labels remain owned by C and B.

### 1.2 Address graph counts

The exact 16,704 address-bearing operands partition without overlap:

| Target class | Main | Mystery gift | Total | Notes |
|---|---:|---:|---:|---|
| `SCRIPT_TARGET` | 8,175 | 33 | **8,208** | calls/jumps, map routing, trainer continuations, virtual anchors |
| `TEXT_TARGET` | 6,187 | 20 | **6,207** | 5,641 unique targets overall |
| `MOVEMENT_TARGET` | 2,009 | 0 | **2,009** | remains B-owned |
| `MART_TABLE_TARGET` / `RAW_DATA_TARGET` | 262 | 0 | **262** | static script-local data |
| `RAM_DATA_TARGET` | 18 | 0 | **18** | writable EWRAM/IWRAM globals |
| **Total** | **16,651** | **53** | **16,704** | five 16-bit SPECIAL IDs excluded |

`MART_TABLE_TARGET` is included in the 262 static-data row and is retained as
a distinct relocation subtype. `MAP_OBJECT_TARGET` has zero address operands:
field commands encode map group/number, local object ID, coordinates, and
variables as scalars. `ENGINE_CALLBACK` has zero qualified bytecode operands
(the unused `callnative`/`gotonative` grammar still permits it); the 777
host-function/global-address entries in the command/special prefix are engine
tables, not bytecode operands. `OTHER` is zero after classification. Scalar
32-bit words in `loadword`, money commands, and `givemon` stay scalar unless
an ELF relocation and grammar role prove otherwise.

Under the chosen 523-module resource boundary, 95 script-address operands
target module offset zero and **8,113 target an interior offset**. There are
5,130 unique interior targets in the main object; mystery-gift contributes
additional module-local labels. This is an occurrence count: duplicate edges
remain duplicate because each operand needs its own validation record.

There are no unresolved operands in the qualified address-relocation set.
An extractor that merely walks reachable fallthrough can produce false
unknown opcodes by decoding adjacent data; symbol/source segmentation plus
relocation coverage is therefore mandatory. Qualified-vanilla acceptance is
zero unresolved and zero ambiguous containing modules.

### 1.3 Separation from other ownership stages

| Class | Owner |
|---|---|
| Field bytecode and field control-flow graph | R13-G |
| Map dispatch/conditional tables and marts/field-local lists | R13-G |
| Field text already catalogued in `script_data` | R13-C |
| Twenty mystery-gift field-text labels (2,682 bytes) not yet in C | additive C handoff prerequisite inside G2 |
| Movement bytecode | R13-B, still `COMPILED_PENDING_MIGRATION` |
| Command table, special callback table, native callbacks | engine image constants |
| Dynamic Wonder Card/RAM scripts | run-time mutable state, not immutable resources |
| Battle, animation, and battle-AI bytecode | R13-H |

## 2. Field-script execution engines

Emerald has one reusable `ScriptContext` interpreter core and two command
languages using it. The ordinary field VM has two contexts plus a RAM-script
mode. Mystery Event is a genuinely distinct 17-command language using its own
context. The eight static `data/scripts/gift_*.inc` files are not that second
language: they use the ordinary field opcodes and belong to G.

```text
map/object/coord/bg/trainer entry
             |
             v
   ordinary field command table (227 opcodes)
      |                         |
      v                         v
Context1: async/yielding     Context2: immediate map dispatch
  scriptPtr + stack[20]       same representation; must finish in call
  native wait callback        never a legal capture point while active
      |
      +--> saved/dynamic RAM script (same VM; returnram/endram/vaddress)

downloaded Mystery Event payload
             |
             v
  distinct 17-op command table + EWRAM ScriptContext
  dynamic buffer/nativeBase; never an immutable G resource
```

| Path | IP/stack now | Dispatch and suspension | Persistence decision |
|---|---|---|---|
| Global `sGlobalScriptContext` (Context1) | native `const u8 *scriptPtr`; 20 native return pointers | `gScriptCmdTable`; bytecode/native modes; wait commands set global WAITING; callbacks resume it | top-level `src/script.o` BSS is linker-owned `game_bss`, already captured with pointer walking; G adds typed arena containment/sidecars |
| `sImmediateScriptContext` (Context2) | same native layout | runs synchronously for map load/transition/resume/dive and conditional dispatch | capture is refused/asserted unless stopped; no sidecar records needed in a valid save |
| RAM/Wonder Card field scripts | Context1 points into `SaveBlock1.ramScript.data.script`; `gRamScriptRetAddr` points back to static script | ordinary table; `returnram`, `endram`, `trywondercardscript`; vaddress family handles downloadable virtual addresses | dynamic buffer identity + offset; static return uses G resource identity |
| Trainer approach | `gApproachingTrainers[2].trainerScriptPtr`, then three `battle_setup.c` continuation pointers | field `trainerbattle` parser redirects to common field scripts; battle engine runs separately; post-battle resumes field VM | all five static script-pointer sources require G relocation |
| Mystery Event interpreter | EWRAM context plus `game_bss`-captured `sMysteryEventScriptNativeBase` | separate 17-op table; command-at-a-time or synchronous run | dynamic buffer identity; validate/reconstruct native base; not a G static arena range |

There is no separate Emerald quest-log/event-replay field interpreter. The
FireRed quest-log opcodes/macros that survive numerically are no-op command
slots in Emerald, not a replay VM.

## 3. Field bytecode grammar

G1 must emit a machine-readable grammar record for all 227 opcodes. Each row
contains `{opcode, name, fixedSize|dynamicRule, operands[], flow,
maySuspend}`. Operand primitives are `u8`, `u16le`, `u32le`, `var16`, and
`addr32(class)`. `addr32` is always four encoded bytes. It is never a host
pointer.

### 3.1 Complete opcode enumeration

The following compact enumeration is the implementation grammar. `b`, `h`,
and `w` mean scalar `u8`, `u16le`, and `u32le`; `S/T/M/D/F/V` mean typed
32-bit script/text/movement/data/native-function/virtual address. The leading
opcode byte is included in every size. Flow is ordinary fallthrough unless
stated.

| Opcodes | Commands and encoded grammar |
|---|---|
| 00–0F | `00 nop(1)`, `01 nop1(1)`, `02 end(1,end)`, `03 return(1,return)`, `04 call(S,5,call)`, `05 goto(S,5,jump)`, `06 goto_if(b,S,6,conditional jump)`, `07 call_if(b,S,6,conditional call)`, `08 gotostd(b,2,jump)`, `09 callstd(b,2,call)`, `0A gotostd_if(b,b,3)`, `0B callstd_if(b,b,3)`, `0C returnram(1,jump)`, `0D endram(1,end)`, `0E setmysteryeventstatus(b,2)`, `0F loadword(b,w,6)` |
| 10–1F | `10 loadbyte(b,b,3)`, `11 setptr(b,D,6)`, `12 loadbytefromptr(b,D,6)`, `13 setptrbyte(b,D,6)`, `14 copylocal(b,b,3)`, `15 copybyte(D,D,9)`, `16 setvar(h,h,5)`, `17 addvar(h,h,5)`, `18 subvar(h,h,5)`, `19 copyvar(h,h,5)`, `1A setorcopyvar(h,h,5)`, `1B compare_local_to_local(b,b,3)`, `1C compare_local_to_value(b,b,3)`, `1D compare_local_to_ptr(b,D,6)`, `1E compare_ptr_to_local(D,b,6)`, `1F compare_ptr_to_value(D,b,6)` |
| 20–2F | `20 compare_ptr_to_ptr(D,D,9)`, `21 compare_var_to_value(h,h,5)`, `22 compare_var_to_var(h,h,5)`, `23 callnative(F,5)`, `24 gotonative(F,5,native jump)`, `25 special(h,3)`, `26 specialvar(h,h,5)`, `27 waitstate(1,suspend)`, `28 delay(h,3,suspend)`, `29 setflag(h,3)`, `2A clearflag(h,3)`, `2B checkflag(h,3)`, `2C initclock(h,h,5)`, `2D dotimebasedevents(1)`, `2E gettime(1)`, `2F playse(h,3)` |
| 30–3F | `30 waitse(1,suspend)`, `31 playfanfare(h,3)`, `32 waitfanfare(1,suspend)`, `33 playbgm(h,b,4)`, `34 savebgm(h,3)`, `35 fadedefaultbgm(1)`, `36 fadenewbgm(h,3)`, `37 fadeoutbgm(b,2)`, `38 fadeinbgm(b,2)`, `39 warp(b,b,b,h,h,8)`, `3A warpsilent(same,8)`, `3B warpdoor(same,8)`, `3C warphole(b,b,3)`, `3D warpteleport(same-as-warp,8)`, `3E setwarp(same,8)`, `3F setdynamicwarp(same,8)` |
| 40–4F | `40 setdivewarp(b,b,b,h,h,8)`, `41 setholewarp(same,8)`, `42 getplayerxy(h,h,5)`, `43 getpartysize(1)`, `44 additem(h,h,5)`, `45 removeitem(h,h,5)`, `46 checkitemspace(h,h,5)`, `47 checkitem(h,h,5)`, `48 checkitemtype(h,3)`, `49 addpcitem(h,h,5)`, `4A checkpcitem(h,h,5)`, `4B adddecoration(h,3)`, `4C removedecoration(h,3)`, `4D checkdecor(h,3)`, `4E checkdecorspace(h,3)`, `4F applymovement(h,M,7)` |
| 50–5B | `50 applymovementat(h,M,b,b,9)`, `51 waitmovement(h,3)`, `52 waitmovementat(h,b,b,5)`, `53 removeobject(h,3)`, `54 removeobjectat(h,b,b,5)`, `55 addobject(h,3)`, `56 addobjectat(h,b,b,5)`, `57 setobjectxy(h,h,h,7)`, `58 showobjectat(h,b,b,5)`, `59 hideobjectat(h,b,b,5)`, `5A faceplayer(1)`, `5B turnobject(h,b,4)` |
| 5C | `trainerbattle`: dynamic, described below |
| 5D–6F | `5D dotrainerbattle(1,suspend)`, `5E gotopostbattlescript(1,jump)`, `5F gotobeatenscript(1,jump)`, `60 checktrainerflag(h,3)`, `61 settrainerflag(h,3)`, `62 cleartrainerflag(h,3)`, `63 setobjectxyperm(h,h,h,7)`, `64 copyobjectxytoperm(h,3)`, `65 setobjectmovementtype(h,b,4)`, `66 waitmessage(1,suspend)`, `67 message(T,5)`, `68 closemessage(1)`, `69 lockall(1)`, `6A lock(1)`, `6B releaseall(1)`, `6C release(1)`, `6D waitbuttonpress(1,suspend)`, `6E yesnobox(b,b,3)`, `6F multichoice(b,b,b,b,5)` |
| 70–7F | `70 multichoicedefault(b,b,b,b,b,6)`, `71 multichoicegrid(b,b,b,b,b,6)`, `72 drawbox(1; Emerald consumes no operands)`, `73 erasebox(b,b,b,b,5)`, `74 drawboxtext(b,b,b,b,5)`, `75 showmonpic(h,b,b,5)`, `76 hidemonpic(1)`, `77 showcontestpainting(b,2)`, `78 braillemessage(T,5)`, `79 givemon(h,b,h,w,w,b,15; words are scalar zero fields)`, `7A giveegg(h,3)`, `7B setmonmove(b,b,h,5)`, `7C checkpartymove(h,3)`, `7D bufferspeciesname(b,h,4)`, `7E bufferleadmonspeciesname(b,2)`, `7F bufferpartymonnick(b,h,4)` |
| 80–8F | `80 bufferitemname(b,h,4)`, `81 bufferdecorationname(b,h,4)`, `82 buffermovename(b,h,4)`, `83 buffernumberstring(b,h,4)`, `84 bufferstdstring(b,h,4)`, `85 bufferstring(b,T,6)`, `86 pokemart(D,5)`, `87 pokemartdecoration(D,5)`, `88 pokemartdecoration2(D,5)`, `89 playslotmachine(h,3)`, `8A setberrytree(b,b,b,4)`, `8B choosecontestmon(1)`, `8C startcontest(1)`, `8D showcontestresults(1)`, `8E contestlinktransfer(1)`, `8F random(h,3)` |
| 90–9F | `90 addmoney(w,b,6)`, `91 removemoney(w,b,6)`, `92 checkmoney(w,b,6)`, `93 showmoneybox(b,b,b,4)`, `94 hidemoneybox(1 at run time; source macro's two zero bytes execute as two following NOPs)`, `95 updatemoneybox(b,b,b,4)`, `96 getpokenewsactive(h,3)`, `97 fadescreen(b,2)`, `98 fadescreenspeed(b,b,3)`, `99 setflashlevel(h,3)`, `9A animateflash(b,2)`, `9B messageautoscroll(T,5)`, `9C dofieldeffect(h,3)`, `9D setfieldeffectargument(b,h,4)`, `9E waitfieldeffect(h,3)`, `9F setrespawn(h,3)` |
| A0–AF | `A0 checkplayergender(1)`, `A1 playmoncry(h,h,5)`, `A2 setmetatile(h,h,h,h,9)`, `A3 resetweather(1)`, `A4 setweather(h,3)`, `A5 doweather(1)`, `A6 setstepcallback(b,2)`, `A7 setmaplayoutindex(h,3)`, `A8 setobjectsubpriority(h,b,b,b,6)`, `A9 resetobjectsubpriority(h,b,b,5)`, `AA createvobject(b,b,b,b,h,h,9)`, `AB turnvobject(b,b,3)`, `AC opendoor(h,h,5)`, `AD closedoor(h,h,5)`, `AE waitdooranim(1)`, `AF setdooropen(h,h,5)` |
| B0–BF | `B0 setdoorclosed(h,h,5)`, `B1 addelevmenuitem(b,h,h,h,8)`, `B2 showelevmenu(1)`, `B3 checkcoins(h,3)`, `B4 addcoins(h,3)`, `B5 removecoins(h,3)`, `B6 setwildbattle(h,b,h,6)`, `B7 dowildbattle(1,suspend)`, `B8 setvaddress(V,5)`, `B9 vgoto(V,5,jump)`, `BA vcall(V,5,call)`, `BB vgoto_if(b,V,6)`, `BC vcall_if(b,V,6)`, `BD vmessage(V-text,5)`, `BE vbuffermessage(V-text,5)`, `BF vbufferstring(b,V-text,6)` |
| C0–CF | `C0 showcoinsbox(b,b,3)`, `C1 hidecoinsbox(b,b,3)`, `C2 updatecoinsbox(b,b,3)`, `C3 incrementgamestat(b,2)`, `C4 setescapewarp(b,b,b,h,h,8)`, `C5 waitmoncry(1)`, `C6 bufferboxname(b,h,4)`, `C7 textcolor(1,nop1)`, `C8 loadhelp(1,nop1)`, `C9 unloadhelp(1,nop1)`, `CA signmsg(1,nop1)`, `CB normalmsg(1,nop1)`, `CC comparehiddenvar(1,nop1)`, `CD setmodernfatefulencounter(h,3)`, `CE checkmodernfatefulencounter(h,3)`, `CF trywondercardscript(1,conditional jump)` |
| D0–DF | `D0 setworldmapflag(1,nop1)`, `D1 warpspinenter(b,b,b,h,h,8)`, `D2 setmonmetlocation(b,h,4)`, `D3 moverotatingtileobjects(h,3)`, `D4 turnrotatingtileobjects(1)`, `D5 initrotatingtilepuzzle(h,3)`, `D6 freerotatingtilepuzzle(1)`, `D7 warpmossdeepgym(b,b,b,h,h,8)`, `D8 selectapproachingtrainer(1)`, `D9 lockfortrainer(1)`, `DA closebraillemessage(1)`, `DB messageinstant(T,5)`, `DC fadescreenswapbuffers(b,2)`, `DD buffertrainerclassname(b,h,4)`, `DE buffertrainername(b,h,4)`, `DF pokenavcall(T,5)` |
| E0–E2 | `E0 warpwhitefade(b,b,b,h,h,8)`, `E1 buffercontestname(b,h,4)`, `E2 bufferitemnameplural(b,h,h,6)` |

`5C trainerbattle` begins `opcode, type:u8, trainer:u16,
localId:u16`. Its legal type-specific sizes and pointer fields are:

| Types | Size | Pointer sequence |
|---|---:|---|
| 0 single | 14 | intro text, defeat text |
| 1 continue; 2 continue/no-music | 18 | intro text, defeat text, field continuation script |
| 3 no-intro | 10 | defeat text |
| 4 double | 18 | intro, defeat, insufficient-party text |
| 5 rematch; 9 pyramid; 10 set-A; 11 set-B; 12 hill | 14 | intro text, defeat text |
| 6 continue-double; 8 continue-double/no-music | 22 | intro, defeat, insufficient-party text, field continuation script |
| 7 rematch-double | 18 | intro, defeat, insufficient-party text |

An unrecognised trainer type is malformed bytecode. Slots C7–CC and D0 consume
only the opcode in Emerald even if an obsolete FRLG source macro suggests
arguments. This exception and `hidemoneybox` are required oracle fixtures.

### 3.2 Non-opcode grammars

Map dispatch is `tag:u8, target:addr32(SCRIPT_TARGET)` repeated until tag 0.
Tags 2 and 4 point to conditional rows
`var:u16, value:u16, target:addr32(SCRIPT_TARGET)` terminated by `var == 0`.
Marts/decor lists are typed halfword arrays with their command-specific
sentinel. The 17-op Mystery Event grammar remains owned by
`mystery_event_script.c`; G1 records it only to prevent accidental
classification as the ordinary field language.

## 4. Canonical address and resource model

### 4.1 Current x64 representation

The native build has not widened the bytecode grammar. The main native object
still contains 16,651 four-byte link relocations in field operands (and the
mystery-gift object has 53); their encoded value is a 32-bit logical/link-time
address. By contrast, `ScriptContext.scriptPtr`, its 20 stack entries, native
callback, and command-table pointers are eight-byte host pointers. Native
engine dispatch tables use host-width entries. This mixed representation is
why casting an operand to a pointer or storing a GBA word into an x64 pointer
object is unsafe.

`ScriptReadPointer` currently turns the four-byte value into a host pointer via
`HostResolveGbaAddr`. Normal call/jump/text/movement/data consumers mostly
funnel through it. Map dispatch uses `T2_READ_PTR` directly. F's map, coord,
and script-bearing BG fields are native pointers after hydration, while
`ObjectEventTemplate.script` deliberately remains a four-byte `GbaAddr` and
its accessor calls `HostResolveGbaAddr`. The trainer argument loader writes
four-byte encoded values into pointer variables. The vaddress family bypasses
the resolver and performs host-delta arithmetic. These are the exact seams G
must change; no blanket pointer widening is valid.

### 4.2 Identity

Canonical script identity is `(profile, resource key, payload offset)` with
original GBA address provenance. Live identity is `(script arena base,
resource live offset)`. A GBA address is evidence and an import key, not the
sole semantic resource name.

Keys are profile-scoped and lower-case/dash-normalised:

```text
emerald:script/map/<map-name>
emerald:script/common/<source-module>
emerald:script/mystery-gift/<gift-name>
```

Each resource exports named bindings such as `...#<symbol>`. The binding row
contains semantic label, original GBA address, module-relative provenance
offset, packed-payload offset, boundary kind, and aliases. Duplicate symbols
at one address are aliases to one binding. An unnamed target becomes
`#anon-<gba-address>` deterministically inside the containing module; an
interior label becomes `#<label>` or `#at-<provenance-offset>`. Anonymous
blocks are owned by the smallest unique source module. Overlapping candidate
modules or an address with no unique owner is a refusal.

Arrays/tables are typed segments in their owner module and have their own
binding. A dynamically referenced static address still resolves through the
provenance index. Dynamic RAM addresses never acquire static resource keys.

### 4.3 Granularity decision

Use a **hybrid graph-aware bundle**, one resource per the 523 semantic source
modules, with sparse typed segments and stable exported offsets. This is not
one resource per symbol and not one ROM-wide bank.

- A symbol-per-resource model cannot represent 8,113 interior target edges
  safely and makes return addresses unstable.
- A contiguous ROM bank makes a one-NPC mod replace far too much content and
  entangles C text/B movement ownership.
- A module preserves local label geometry, trainer continuations, and return
  addresses while still allowing one map, common module, or gift module to be
  replaced independently.
- Text and movement holes are external bindings; their payload bytes are not
  duplicated in G.

Insertion/removal is allowed inside a replacement module. The pack generator
recomputes its binding offsets and relocation records. Other modules call the
exported binding identity, not the old byte position.

### 4.4 Canonical pack record

The byte payload contains exact qualified-ROM bytes in deterministic segment
order. It is never rewritten to native pointers. The sidecar contains:

```text
module key, schema, exact-byte digest
segments[] = {kind, originalGbaStart, byteCount, payloadOffset}
exports[]  = {name, originalGbaAddress, provenanceOffset, payloadOffset,
              boundaryKind, aliases[]}
relocs[]   = {operandPayloadOffset, operandWidth=4, originalEncodedGba,
              targetClass, targetResourceKey, targetExport/targetOffset,
              runtimeResolutionRequired}
```

All 16,704 address operands retain their original encoded four bytes. Every
record proves that the operand lies wholly inside an instruction/table field,
the recorded raw value equals the payload, and the target class matches the
grammar. `runtimeResolutionRequired` is true for live pointers and false for
provenance-only/scalar-special cases. There are no native pointers in pack
payload or metadata. `bytePatchRequired` is false for every operand in the
chosen model; resolution occurs when an operand or published binding is
consumed.

## 5. Runtime resolver and interpreter budget

### 5.1 Why byte patching is rejected

The VM reads four-byte operands. A native x64 pointer cannot be written into
that field without corrupting the next instruction. Low-address allocation is
not a stable ABI or State-v5 identity. Rewriting to a native command format
would duplicate the VM and lose byte parity. The current global logical
address registry is also not the script graph: it has hard capacities of 197
exact addresses and 16 ranges, far below this graph.

### 5.2 Selected model

`EmeraldScriptCompat` builds two immutable indexes during validation:

1. source index: live operand address -> relocation row;
2. target index: canonical GBA provenance/binding -> typed live target.

`ScriptReadPointer(ctx)` records the operand location, reads the unchanged
`u32`, and asks the typed resolver for the relocation at that source. Static
arena operands must have an exact sidecar row; dynamic RAM scripts resolve the
raw value against the target index or their virtual-base anchor. The resolver
checks expected/actual target family before returning a host pointer.

Map dispatch and F entry publication resolve exported bindings directly.
Engine callbacks continue through `HostResolveFunction`; they never enter the
script arena. Raw writable-data targets resolve only to approved EWRAM/IWRAM
ranges. This is an O(log n) indexed lookup on pointer-bearing commands, not on
every opcode; it is negligible compared with text, movement, and rendering
work.

### 5.3 Exact code-change budget

The intended interpreter budget is **12 command handlers plus one central
reader**, not a VM rewrite:

- one `ScriptReadPointer` replacement covers the ordinary pointer-consuming
  handlers (`call/goto`, data reads/writes, text, movement, marts, etc.);
- eight virtual-address handlers: `setvaddress`, `vgoto`, `vcall`,
  `vgoto_if`, `vcall_if`, `vmessage`, `vbuffermessage`, `vbufferstring`;
- four deferred-message sites (`message`, `messageautoscroll`,
  `messageinstant`, `pokenavcall`) share one raw-text resolver for
  `ctx->data[0]`.

Outside those handlers: two map-dispatch pointer reads, one central
trainer-argument loader, the two `ObjectEventTemplate` script accessors, the F
publication/rebind seam, and execution-state range/validation changes.
`ScriptContext`, opcode values, stack depth, command table ABI, and byte
grammar remain unchanged. The object getter resolves its stable GBA provenance
through G; the setter reverse-maps a G-arena pointer back to that provenance so
save-block template copies never retain a generation-local handle. The trainer
loader must stop placing a four-byte GBA value into an x64 pointer object via
`SetU32`; text and continuation fields are resolved by type, while its implicit
post-command return is already a live instruction pointer.

The four `gotostd`/`callstd` handlers need no grammar or flow change:
`gStdScripts` becomes an 11-slot native publication table filled from G
bindings during phase 2. Its compiled object contains slots/metadata only, not
the original GBA pointer payload.

## 6. `sAddressOffset` decision

`sAddressOffset` is not dead code. `setvaddress` currently computes
`encodedVirtualBase - nativeAddressOfOpcode`; the seven `v*` consumers subtract
that delta to reconstruct a host pointer. It makes relocatable downloaded
scripts work when their embedded virtual addresses do not equal their buffer
address. On x64 it is an `intptr_t`, but it is host-base-dependent, is static
captured EWRAM state, and is not a stable cross-process identity despite being
serialized.

The audit found exactly one writer (`ScrCmd_setvaddress`) and seven readers
(`vgoto`, `vcall`, `vgoto_if`, `vcall_if`, `vmessage`, `vbuffermessage`, and
`vbufferstring`); there are no other owners to preserve implicitly.

R13-G must **replace, not delete or neutralise**, it. The replacement is a
captured virtual-address anchor:

```text
{ encodedVirtualBase:GbaAddr,
  liveBufferIdentity:dynamic-buffer-id | static-resource-key,
  liveBaseOffset:u32 }
```

Static G scripts normally use direct typed bindings. Dynamic RAM scripts use
`liveBase + (encodedTarget - encodedVirtualBase)` after bounds and target
boundary checks. No creator-process delta is persisted. `sAddressOffset` is
removed only after fixtures prove all eight vaddress handlers, the eight
mystery-gift modules, and downloaded RAM scripts through fresh-process load.

## 7. Cross-stage handoffs

### 7.1 R13-F map/event handoff

F resource keys and schemas remain unchanged. G consumes the four stored GBA
provenances and publishes native pointers:

- `MapHeader.mapScripts` -> G map-module dispatch binding;
- `ObjectEventTemplate.script` -> its four-byte `GbaAddr` field remains stable
  provenance, while the narrow getter/setter pair resolves/reverse-resolves a
  G exported binding; the x64 structure cannot and must not hold a raw arena
  pointer in this field;
- `CoordEvent.script` -> G binding;
- `BgEvent.script` for script-bearing kinds -> G binding.

Boot order becomes dependencies first, scripts second, maps last:

```text
R13-B movement arena + R13-C text arenas
  -> validate/build EmeraldScriptCompat (no publication)
  -> publish script arena/index/ranges
  -> publish R13-F maps using the new binding set
  -> expose gMapHeaders and enable field execution
```

No dependency cycle exists because G validates F's generated provenance/index
records without requiring published native map objects. At boot no post-script
callback is needed. Republish/mod sessions need one narrow
`EmeraldMapCompat_RebindScripts(bindingSet)` transaction that stages every
map/event pointer and swaps only after complete validation; it does not change
F resource identity or rebuild layouts/connections.

### 7.2 STOP gate: coordinate-event wire offsets

The current R13-F seam reads a 16-byte GBA `CoordEvent` as trigger at byte 5,
index at byte 7, and script at byte 8. `asm/macros/map.inc` and the qualified
ROM prove the GBA layout is elevation at 4, pad at 5, trigger at 6, index at 8,
pad at 10, and script at **12**. The generated coordinate script-provenance
array inherits the same wrong byte-8 read (mostly values 0/1), so its green
self-parity test does not prove the real pointer.

The authoritative ROM audit produces 289 coordinate script references / 194
unique targets. **This is a STOP-level prerequisite for G live cutover.** G1
must first add a ROM-layout oracle and correct/regenerate the F-derived
coordinate provenance and native hydration. It is a narrow seam correction,
not a broad F rewrite. No production change is made by this plan.

### 7.3 R13-C text

The main object has 6,207 text edges (qualified-build measurement; the
recomp-era 6,187 figure was stale — see §18.1) to 5,621 unique targets. All
have an existing R13-C identity; missing main identities: zero. The
script-side text families remain C-owned even where C currently marks their
compiled symbols pending cutover. G resolves those identities into the
existing C arena and does not copy text into G.

The eight `.rodata` mystery-gift modules add 20 edges to 20 unique text labels
(2,682 bytes) that are not in the present C inventory. G2 must add these to C's
text catalog/arena before G can publish the gift scripts. Missing identities
after that prerequisite must be zero. This is an additive C ownership handoff,
not permission to duplicate their bytes in G.

**Done in G2:** the 20 identities are live in C's catalog as passive schema-15u
inventory records (`emerald:text/mystery-gift/*`, 2,682 B); the seam pins moved
to 12,797 labels / 905,839 B with the misc arena summary at 6,211 / 551,273.
Zero missing text identities remain (verified by the three-way oracle and the
script-module loader suite).

### 7.4 R13-B movement

The decision is **A: continue the B bridge, with only the field-edge binding
needed by G**. G does not assume ownership or perform a whole movement-family
cutover. For all 2,009 edges the typed resolver maps encoded provenance to an
R13-B arena binding where one exists; the three recomp-local/12-byte cases get
an explicit, named compiled bridge until B's remaining consumers and payloads
are cut together. If the current additive leaf seam cannot expose all required
bindings, G2 adds that lookup surface; it does not create G movement resources
or remove movement symbols. Movement isolation remains an explicit exception.

**Done in G2:** all 2,009 edges resolve to R13-B ownership with the three
recomp-local bridges as explicit named compiled bridges
(`emerald:movement/bridge/<slug>`, 12 B: Route103 rival exit, two ferry
depart-island boards) and 11 gStdScripts shadow-only binding records; the
movement binding surface (1,055 binding records) is complete.

### 7.5 Script-local data

The 262 static data operands are classified by command grammar, never merely
by address:

| Kind | Policy |
|---|---|
| Marts and decoration shop lists | typed G module data segment; sentinel validated |
| Map dispatch and conditional tables | typed G routing segments; 5,749 exact bytes |
| Trainerbattle inline metadata | stays inline in exact field bytecode; continuation/text relocations typed |
| Braille format prefix/menu/item/contest byte or halfword lists | G local segment if field-command-owned; text portion remains C |
| `gSpecials`, `gSpecialVars`, command handlers | engine constants, not pack payload |
| 18 writable EWRAM/IWRAM targets | explicit allowlisted `RAM_DATA_TARGET`, never arena data |
| Battle/AI/animation tables | H, and invalid as a G local target unless a named boundary contract says otherwise |

No unclassified compiled static data target is allowed after cutover.

### 7.6 Trainerbattle G-to-H contract

The `trainerbattle` instruction and all its inline bytes belong to G. G parses
and resolves its field text and optional post-battle **field-script**
continuation. `BattleSetup_ConfigureTrainerBattle` may redirect the field
context to common G scripts such as `EventScript_TryDoNormalTrainerBattle`.
The resulting opponent IDs, battle configuration, and the separate battle
VM's bytecode/IP/stack belong to H.

The only cross-VM handoff is typed engine state: G supplies resolved field
continuations and text; the battle engine returns through the existing field
callback and G continuation identity. A trainer operand can never be resolved
as `BATTLE_SCRIPT_TARGET` in G. H may later migrate its own bytecode without
changing the field grammar or G resource keys.

## 8. Dynamic and RAM scripts

Immutable G resources are only qualified-ROM static modules. SaveBlock RAM
scripts, Wonder Card scripts, downloaded Mystery Event programs, and buffers
constructed or patched at run time retain mutable storage.

Dynamic identity is `(buffer kind, captured owner object, offset, generation)`:

- `SAVE_RAM_SCRIPT`: offset inside `SaveBlock1.ramScript.data.script`, already
  part of saved/captured game memory;
- `MYSTERY_EVENT_BUFFER`: offset inside its explicit captured transfer buffer;
- transient immediate buffer: cannot cross a save unless registered as a
  captured runtime object.

Dynamic-to-static operands use the canonical target index. Virtual internal
operands use the captured vaddress anchor and must remain within the dynamic
buffer. `gRamScriptRetAddr` is either null or a G resource binding+offset.
Dynamic bytes are checksum/bounds validated but never put into an immutable
pack or isolation sweep.

## 9. State-v5 plan

No State-v5 file-format change is required. Existing resource-sidecar records
already encode stable key/type/schema/role/range offset. G registers each of
the 523 live module spans under its resource key (role `CANONICAL`) and uses
the existing sidecar before ordinary pointer normalisation.

The actual post-E3b range count is 5,851. F registers three ranges (headers,
event arena, connection arena), so the **actual post-F count is 5,854**, not
the F plan's earlier approximate 5,855. G adds exactly 523 script module
ranges, yielding **6,377 / 8,192**. The mystery-gift text handoff should reuse
the C family arena; if implementation proves it needs one new C family range,
the gated count is 6,378. Both are below capacity. The resource sidecar's
4,096-record cap is per captured pointer record, not the registered-range
count; the execution surfaces below are far smaller.

### 9.1 Pointer surfaces

| Surface | Slots | Stable form / rule |
|---|---:|---|
| Global Context1 `scriptPtr` | 1 | G key + instruction offset, or dynamic-buffer identity + offset |
| Global Context1 return stack | 20 maximum | G key + exact next-instruction offset; only `stackDepth` entries may be non-null |
| `gRamScriptRetAddr` | 1 | G key + offset or null |
| `gApproachingTrainers[].trainerScriptPtr` | 2 | G key + trainer-opcode offset |
| `sTrainerBattleEndScript`, A/B return pointers | 3 | G key + exact post-command/encoded continuation offset |
| Mystery Event context IP + stack | 21 maximum | dynamic buffer + offset, not G key |
| Mystery Event native base / vaddress anchor | 1 | dynamic buffer identity + offset + encoded base |
| Immediate Context2 IP + stack | 21 maximum | must all be inactive/null at capture; otherwise refuse capture |
| Context native callbacks | one per active context | existing engine-image-relative pointer handling; callback is not a script resource |
| Command table/end pointers | two per context | reinitialise from engine constants or existing image relocation; never G records |
| Trainer text pointers | 6 | R13-C identities, not G |
| Current `gMapHeader.mapScripts` and F event graph | published compat ranges | existing F captured-range handling after atomic G rebind |

Thus the static field-execution set has **27 possible G script pointer slots**
(21 Context1 + one RAM return + two approaching-trainer + three battle
continuations). Dynamic Mystery Event adds 22 buffer-relative slots. Context2
has 21 prohibited-at-capture transient slots. Native callbacks and six trainer
text slots are separate typed families.

The linker script places top-level `src/*.o` BSS in the captured `game_bss`
slice. Therefore the global/immediate contexts, status/field-lock scalars, and
`sMysteryEventScriptNativeBase` are already serialized; `sAddressOffset` is
also in captured EWRAM. G does **not** add a slice or move the contexts merely
for persistence. As refined by G3/G4, G4 uses a family adapter against staged
G3 generations and does **not** publish the 523 module ranges; G5 publishes
them atomically with the live cutover. G4 validates Context2 inactivity and
provides the stable-anchor persistence model out of band. A Mystery Event
native base must resolve inside its captured dynamic buffer or be reconstructed
from that buffer identity. This is existing-format relocation work, not a file
format revision.

### 9.2 Capture/restore invariants

Capture performs containment before pointer classification:

1. locate the unique script module or dynamic buffer;
2. convert live pointer to resource/dynamic offset;
3. prove an IP, branch target, or return address is on a decoded instruction
   boundary (a return may be exactly the next instruction);
4. store the existing sidecar identity; poison/remove the creator pointer;
5. on restore, republish at any arena base, resolve identity, repeat bounds and
   boundary proof, then write the new native pointer.

Required automated cases are idle, mid-dialogue, movement wait, nested calls,
interior jump, immediate map-script completion, coord-event, NPC, arena
republish at a different address, and exact-next-opcode resume. Add trainer
battle continuation and RAM/vaddress cases. A stale creator-process pointer,
ambiguous containing resource, pointer in an arena hull but no resource, or
inactive stack garbage is a hard restore refusal.

## 10. Interior-pointer proof

G1 emits an instruction-boundary bitmap and typed-data-boundary table per
module. Every export and relocation target has exactly one containing
resource and a valid boundary kind. Stack returns are captured after operand
consumption, so they must be `NEXT_INSTRUCTION`; current IPs must be
`INSTRUCTION_START`; map/F entry points must be `ENTRYPOINT`; marts and text
must use their own target family.

The validator rejects a pointer outside the arena, offset past payload, offset
into another segment type, malformed opcode boundary, target in a removed
hole, ambiguous module, and interior target lacking an emitted binding. The
8,113 interior occurrences are a first-class parity pin, not tolerated as
anonymous host arithmetic.

## 11. Transactional publication

`EmeraldScriptCompat` is REFUSE-class and publishes no partial graph.

**Phase 1 — resolve and validate**

- require exactly 523 resources and exact schema/representation;
- validate all segment sizes/digests and bytecode grammar;
- validate all 16,704 relocation records against bytes and operand fields;
- resolve all 8,208 script, 6,207 text, 2,009 movement, and 280 data/RAM
  edges with correct family;
- validate all 470 map tables, 158 conditional tables, 11 standard scripts,
  and every F entry provenance (including corrected coord offsets);
- validate instruction/interior boundaries, non-overlap, State-v5 range count,
  and active-context lifecycle.

**Phase 2 — stage**

- allocate one deterministic arena with 523 contiguous module spans;
- copy exact segment bytes without operand patching;
- build source-relocation, target-binding, boundary, reverse-containment, and
  State-v5 range indexes;
- stage `gStdScripts`, trainer/text/movement/data bridges, and all F rebinds.

**Phase 3 — commit**

- register 523 resource ranges;
- atomically swap the arena/index generation;
- publish standard-script and F map/event bindings;
- expose field entry points only after every store can succeed.

Failure destroys the staged generation and preserves the prior complete
generation, or refuses initial session startup. After live cutover there is
no compiled-script fallback.

## 12. Failure matrix

All are hard refusal after cutover:

| Failure | Required result |
|---|---|
| Missing/extra script resource or wrong schema | refuse before allocation |
| Malformed bytecode, unknown expected-vanilla opcode, truncated fixed/dynamic command | refuse with module + offset |
| Invalid trainerbattle type/length | refuse |
| Relocation outside operand, wrong width, raw value mismatch | refuse |
| Missing script target/export | refuse |
| Invalid interior offset or wrong boundary kind | refuse |
| Missing/mis-typed text target | refuse |
| Missing movement target/bridge | refuse |
| Unresolved mart/local table or bad sentinel | refuse |
| Unapproved writable RAM target | refuse |
| F map/object/coord/bg provenance with no binding | refuse |
| Overlapping or ambiguous resources/segments | refuse |
| State pointer with no unique containing resource | refuse save/load as appropriate |
| Stale creator pointer or out-of-generation arena pointer | refuse restore |
| Context2 active during capture | refuse capture |
| Range/sidecar capacity exceeded | refuse publication/capture |
| Any phase-3 partial store/registration possibility | design error; phase 3 must be infallible |

## 13. Parity and oracle strategy

### 13.1 Static oracle

The graph extractor compares ELF relocation, ROM bytes, source grammar, and
generated binding independently. For every address operand it asserts:

```text
ROM encoded value
  == ELF relocation result
  == generated relocation.originalEncodedGba
generated target key+offset
  resolves to the same provenance address and target family
```

Pins: 207,330 bytecode bytes (206,638 main + 692 mystery gift; see §18.1 for
the reconciliation from the former 206,283 figure), 523 modules, 16,704
address operands, class partition, 8,113 interior occurrences, 470/158/919
map-routing counts, and all F inbound counts. It also proves zero
unresolved/ambiguous targets and exact ROM section hashes.

### 13.2 VM differential oracle

Build a dual-run harness around `RunScriptCommand`: compiled baseline and
resource generation each execute one command at a time from identical cloned
state. Compare opcode, pre/post canonical IP, stack depth and canonical return
offsets, mode/status, vars/flags, native callback identity, resolved text,
movement and data identities, specials, map transitions, trainer
continuations, and suspension/resume point. Nondeterministic engine effects
use a recorded special/callback boundary rather than pointer equality.

Qualified scripts currently exercise 176 main opcodes plus the virtual
address family in mystery-gift modules. Synthetic exact-byte fixtures cover
all 227 slots, including unused Emerald commands, C7–CC/D0 no-op consumption,
`hidemoneybox`'s following zero NOPs, all 13 trainer types, malformed/truncated
forms, maximum nested stack, RAM returns, and every pointer family.

Full-world oracles run representative NPC, sign, coord, map transition/on-load
/on-frame, dialogue, movement wait, mart, trainer battle, warp, and mystery
gift flows. State-v5 tests fork a fresh process, force a different arena base,
restore each mandatory case, and compare the exact next canonical opcode.

## 14. Isolation after cutover

Isolation operates on qualified provenance intervals and symbol families, not
only convenient globals:

- link-map/nm/readelf sweep rejects every G bytecode export, local label, map
  script table, standard-script pointer table, and migrated local-data segment;
- byte-window sweep rejects exact and interior windows from all 523 resource
  payloads, including anonymous spans and alias-normalised symbols;
- relocation sweep rejects native map/event pointers into compiled
  `script_data`/mystery-gift script spans;
- runtime proof asserts every F entry, Context1 IP/stack, trainer continuation,
  and `gStdScripts` entry lies in the active G arena generation or an approved
  dynamic buffer;
- no broad field-script exemption and no fallback branch remain.

Explicit exclusions are H battle/animation/AI bytecode, dynamic RAM/Mystery
Event buffers, engine command/special dispatch tables, and B movement scripts
while intentionally deferred. Exclusions are address/symbol allowlists with
owners, not section-wide exemptions.

## 15. ROM-hack import model

An eventual importer analyzes clean and hacked ROMs with the same grammar and
graph extractor. It seeds semantic identities from clean provenance/map/event
ownership, then reconstructs hacked reachable modules and bindings. It can
detect changed bytes, relocated labels, inserted blocks, changed typed edges,
new F entry references, and changed text/movement targets. Matching uses map
identity, exported labels/graph neighbourhood, and clean GBA provenance; raw
address is a deterministic fallback, not the only identity.

Relocation metadata lets a moved or expanded hacked script retain a semantic
resource while acquiring new provenance and offsets. New reachable blocks get
deterministic anonymous keys under their owning map/common module. Ambiguous
ownership is reported for author resolution rather than merged. The importer
is not implemented in G.

## 16. Tallgrass mod model

A mod may replace `emerald:script/map/<map>` as a unit, but a one-NPC change
does not require that: it supplies a new
`emerald:script/mod/<namespace>/<name>` module and overrides the semantic
export `emerald:script/map/<map>#<npc-entry>` to its new entry binding. Direct
callers and the F object entry both resolve that export through the composed
binding table. The same mechanism overrides one map-script tag/entry. A new
script simply exports a new binding; another mod calls it by declared
dependency. New branches/calls name exports and the pack composer emits the
encoded/provenance-independent relocation sidecar. An override updates only
the F-to-G binding metadata, not the F map resource itself. Changing one line
of dialogue overrides the C text identity and leaves G bytecode untouched.

Load order selects one provider per canonical key. A resource may declare
dependencies on another mod's module/export and a compatible schema/version.
Missing exports, cycles that prevent composition, and two same-priority
providers are composition failures. Control-flow cycles are legal after all
bindings resolve. Cross-mod raw host pointers and raw-address-only references
are forbidden.

## 17. Recommended implementation subdivision

R13-G should be six gated waves. State readiness precedes live cutover so no
intermediate build can run arena scripts while saving creator pointers.

### G1 — grammar, graph extractor, and F coord STOP repair

Own the machine grammar, exact segment/relocation graph, boundary maps, all
inventory pins, and an independent ROM oracle for F entry fields. Gate: zero
unresolved/ambiguous operands, all counts in this plan reproduced, and the
coordinate offset defect corrected/proved. No live resource consumer.

### G2 — additive module resources and dependency completion

Generate 523 exact-byte module resources, exports, typed relocation sidecars,
and local tables. Add the 20 missing mystery-gift text identities to C and the
complete movement binding surface to B. Gate: deterministic pack, three-way
ROM/ELF/resource parity, zero missing C/B dependencies. Still no live cutover.

**G2 COMPLETE (2026-08-21) — confirmed facts** (report:
`docs/R13G2_FIELD_SCRIPT_RESOURCE_REPORT.md`):

- **523 modules** (467 payload-embedded in the production pack, 56
  routing-only catalog identities), **207,330 B** bytecode, **16,704**
  relocations, class partition 8,208/6,207/2,009/262/18, bindings
  95 root + 8,113 interior, **3,501** F inbound entrypoints — every
  §1/§18.1 pin reproduced by the generator pins and the independent
  three-way oracle; zero unresolved/zero ambiguous.
- **C handoff complete:** the 20 mystery-gift identities (2,682 B) are
  live in the text catalog (`emerald:text/mystery-gift/*`, schema 15u,
  passive); seam pins moved to 12,797 labels / 905,839 B and the misc
  arena summary to 6,211 / 551,273 (the seam's inventory model counts
  the gift labels in both representations; the generator's census model
  is unchanged at 12,777 / 903,157). The 22 braille labels remain named
  pending keys — C-side naming is post-G2.
- **B movement surface complete:** all 2,009 MOVEMENT_TARGET edges
  resolve to R13-B ownership; 3 recomp-local bridges (12 B) as explicit
  named compiled bridges; 11 gStdScripts shadow-only records.
- **§14 range-count impact: zero.** G2 registers no script ranges at
  any session state (final range-index count 5,854 in the runtime
  loader suite, none script-family). The only runtime-visible edits are
  the passive text inventory + seam pins and one robustness fix in
  `emerald_text_compat.c`: the text unregister is now position-
  independent base-identity removal instead of a block-mark match.
  (The old mark-based unregister could orphan the 16 text spans when
  other families' ranges sorted below the text block — surfaced by G2
  pack growth in the loader harness; details in report §8.)
- **Gate:** all six regression suites green (loader 65,745 checks,
  script-module 1,938, state cross-restart, ranges, generator `--check`,
  three-way oracle) — report §8. No live cutover; `sAddressOffset`,
  `ScriptReadPointer`, R13-F repointing, and State-v5 script execution
  handling all unchanged.

### G3 — `EmeraldScriptCompat` staging/resolver seam

Implement all three transactional phases, source/target/boundary indexes,
standard-script staging, dynamic-buffer resolver, and failure matrix in shadow
mode. Gate: resolver parity for all 16,704 operands and fault injection with
no publication/consumer changes.

**G3 COMPLETE (2026-08-21) — confirmed facts** (report:
`docs/R13G3_SCRIPT_COMPAT_SHADOW_REPORT.md`):

- `EmeraldScriptCompat` (include/emerald/resources/emerald_script_compat.h +
  src/emerald/resources/emerald_script_compat.c) stages a deterministic
  shadow generation: 523 spans (210,880 B arena, bytewise id order, 16-aligned,
  byte-identical to the pack - zero operand patching), a 15,874-row live source
  index + 830 GBA routing rows (16,704 sources total), a 12,016-row dynamic
  encoded-GBA target index, 51,814 instruction boundaries, staged 11-entry
  gStdScripts and a staged 3,501-row F rebind plan. Shadow state only: no range
  registration, no live publication.
- **Parity: 16,704/16,704, zero mismatches**, run on two generations at
  different arena bases with identical canonical aggregates (the test's
  cross-base checksum). Target sub-kinds measured and pinned: SCRIPT_PAYLOAD
  8,198 + SCRIPT_ROUTING 7 + SCRIPT_BRIDGE 3; TEXT_BUNDLE_MEMBER 6,187 +
  TEXT_LABEL 20; MOVEMENT_RESOURCE 2,009; MART_TABLE 38 + DISPATCH 198 +
  BRAILLE 26; RAM_HOST 18. Dispositions: 8,236 staged-arena / 8,216 sibling
  seam / 3 compiled bridge / 18 host RAM (gStringVar4) / 231 deferred
  (routing dispatch + braille pending + 7 routing-class script targets).
  95 root / 8,113 interior re-pinned in the seam.
- **Boundary model:** the G1 census walk plus a supplementary walk seeded
  from every script entry point (chains stop before census-decoded starts).
  2,115 opaque bytes (data / dynamically-reached code, incl. the 8
  mystery-gift modules' 692 B - their virtual-address opcode family is not
  in the generated grammar; adding it is a G4 prerequisite). Export position
  classes: 466 offset-zero / 39 typed-data (mart spans) / 19 opaque / decoded
  instruction starts.
- **State-v5 invariant:** the range index stays at exactly 5,854 ranges with
  zero script-family ranges and zero intersection with the shadow arena
  (asserted in the focused test after staging/restage/clear/restage). The
  seam TU references no live execution surface (isolation sweep gate).
- **Pack/resource invariant:** 20,988 entries, byte-identical; the G2
  payloads untouched; the meta enrichment (instruction_count / target_kind /
  target_payload_offset / per-module boundaries) is additive and `--check`
  regenerates all 1,059 artifacts byte-identically.
- **Fault matrix:** 21 injected faults (20 table mutations + the
  deterministic partial-allocation failure) each refused with the exact
  pinned status; pack-variant RESOURCE faults covered in the main suite; the
  focused test passes 7,1xx checks incl. the ASan/UBSan variant.
- **Game binaries unchanged:** the seam is not linked in G3 (shadow-only) -
  the fresh release/DINFO builds are byte-identical to G2 and
  `--verify-game-data` passes.

### G4 — State-v5 execution readiness

**G4 COMPLETE (2026-08-21) — confirmed facts** (report:
`docs/R13G4_SCRIPT_STATE_READINESS_REPORT.md`):

- The existing State-v5 64-byte resource sidecar represents staged static G
  pointers exactly as key + type/schema/role + payload offset; boundary role
  is enforced from the exact destination surface. No file-format change.
- All 27 possible static execution slots are pinned across existing
  `GAME_BSS`, `EWRAM`, and `COMMON` slices. Context1 captures only active
  frames; active/transient Context2 always refuses. Mystery Event and saved
  RAM script pointers use registered captured-buffer identities and offsets.
- A real process-A/process-B harness passes five capture and five restore
  transactions at forced-different 210,880-byte arena bases/generations. The
  nested hard gate uses a nonzero IP plus two nonzero return offsets, executes
  the exact next `RETURN` sequence, and lands at the same canonical offset.
  Separate snapshots pass RAM return, dialogue/C text/native callback,
  movement wait/B ownership, object/coord/map-dispatch/BG, and trainer cases.
- The pointer-free vaddress anchor covers all eight `0xB8..0xBF` operations.
  Live `sAddressOffset` and opcode handlers are unchanged. The earlier G3
  mystery-gift opacity diagnosis was a supplementary-walk section-bound bug,
  not missing grammar; the sparse owned-byte walk now emits 52,042 boundaries
  and leaves 1,423 opaque data bytes.
- The focused refusal/identity matrix passes 32/32, including explicit refusal
  of a legacy nonzero `sAddressOffset` creator-process delta. Existing generic
  State-v5 cross-restart and corruption transactions remain green.
- Live ranges remain exactly **5,854 / 8,192**, with **zero** G ranges. The
  dry run proves all 523 future spans and **6,377 / 8,192** projected capacity;
  no new C range and no cap increase. Static G + trainer C sidecars peak at
  33 / 4,096; dynamic script pointers use captured-storage identities.
- Production links only the weak family adapter; the G3 seam/table remain
  harness-only until G5. `ScriptReadPointer`, live opcodes, `gStdScripts`, F
  bindings, compiled scripts, and resource ownership are unchanged.

G4 stops here. G5 remains the first live publication/cutover wave.

### G5 — atomic VM and R13-F live cutover

Land the 12-handler/central-reader budget, trainer loader, map dispatch,
boot ordering, and narrow F rebind. Gate: full static and stepwise differential
oracles, real map/event integration, sanitizers, native world/render, and no
compiled fallback path. This is the first live wave.

### G6 — compiled field-script removal and isolation

Remove only G-owned compiled payloads/local tables, enforce robust symbol,
address, relocation, and byte sweeps, and rerun the complete regression and
fresh-process State-v5 batteries. Gate: every G resource `ROM_BASE_ONLY`, all
documented B/H/dynamic/engine exclusions still present and named, no R13-H
migration begun.

Each wave has a usable permanent artifact; none introduces patched bytecode,
a temporary native bytecode dialect, or a live state gap.

## 18. Hardest single problem and STOP gate

The hardest problem is **interior script identity across a persisted call
stack**. A return pointer denotes the byte after a call in one module; a jump
or trainer continuation may target one of 8,113 nonzero module offsets. A
resource-per-symbol model loses containment, while raw native pointers resume
in freed creator-process memory after republish.

The failure mode is silent and severe: restore appears successful, then a
return or wait resume fetches an opcode from the wrong module/offset, often
only after dialogue or battle. The architecture contains it with module
resources, exact export/relocation sidecars, instruction-boundary maps,
reverse containment, and existing State-v5 key+offset records.

**STOP gate:** G5 may not start until G4 demonstrates a nested-call save whose
IP and at least two return addresses are interior offsets, restores in a fresh
process at a forced-different arena base, and executes the exact next canonical
opcode/return sequence. Independently, the F coordinate-event offset defect
must pass G1 before any map/event live cutover.

### 18.1 Byte-total reconciliation (G2-verified, 2026-08-20)

The qualified ROM (`f3ae088181bf583e55daf962a92bb46f4f1d07b7`) and its ELF
close at **207,330 B** of G-owned field bytecode: **206,638 B** main object +
**692 B** mystery gift. The plan's former **206,283 B** (205,591 + 692) was
recomp-era bookkeeping derived from stale/pre-correction R13-C text
accounting and is reproducible from neither current build:

| Class (qualified build, symbol-verified) | Measured | Former plan figure |
|---|---:|---:|
| Engine routing (physical prefix) | 3,152 | 3,152 ✓ |
| Map routing (470 tables + 158 cond tables) | 5,749 | 5,749 ✓ |
| Movement (1,047 B labels + 3 recomp-local) | 7,416 | 7,416 ✓ |
| R13-C text (catalog + naming rule + data/text files) | **749,565** | 750,612 |
| **G main field bytecode (residual)** | **206,638** | 205,591 |
| Mystery-gift field bytecode | 692 | 692 ✓ |
| **Total G bytecode** | **207,330** | 206,283 |

Evidence chain (all against the qualified build, absolute paths):

1. **Text class completeness.** The text class (C binding catalog 747,011 B
   + naming-rule aliases 130 B + the 36 `data/text/*.inc` file labels
   2,424 B) is verified by a printable-string scan of the entire main
   object: only 53 B of string-like bytes exist outside the class (four
   script spans whose opcode bytes happen to be printable), so no text is
   misattributed to G. The 1,347-byte tail (rel 971,173..972,520) is
   confirmed as 13 `gText_Save*`/`gText_Birch_*` labels — text, not
   "script-local data".
2. **The former 750,612 B text figure** = R13-C's corrected catalog
   (749,265 B) + 1,347 B tail, but the current `text/bindings.generated.toml`
   measures 747,011 B ≈ R13-C's *pre-correction* 747,007 B: the binding
   catalog was generated from the recomp ELF (7,934 symbol matches there vs
   7,914 on the qualified ELF — the 20-label recomp-vs-qualified drift,
   the same +20 seen in G1's stale text-edge pin 6,187 vs the measured
   6,207). The recomp build itself measures 207,074 B with the identical
   model — so 206,283 matches neither build.
3. **G1 graph facts are unaffected.** 523 modules, 16,704 operands, 95/8,113
   script bindings, 8,208/6,207/2,009/262/18 class partition, 470/158/919/540
   map-routing counts, and 17,497-census closure are all qualified-build
   facts and re-check green (see G1 report §update).

**Authoritative pin (supersedes all earlier G text):** module byte sums and
every ownership assertion gate on **207,330 B = 206,638 main + 692 gift**.
206,283 survives only as the historical figure in this reconciliation. The
qualified-ROM parity requirement (Section 3) is unchanged and remains the
higher invariant.

## 19. Relative complexity

R13-G is substantially more complex than R13-C text and R13-F map metadata.
The byte volume is smaller than C text, but G changes an active VM, reconstructs
a typed graph, validates dynamic grammar, preserves thousands of interior
targets, crosses C/B/F ownership seams, and makes live execution state
fresh-process relocatable. Relative engineering/risk estimate:

- R13-C text: 1.0 baseline;
- R13-F map metadata: about 1.2;
- **R13-G field scripts: about 3.0–4.0**;
- R13-H combined battle/animation/AI families: likely 4.0–6.0 overall, but it
  is several VMs and should be subdivided; no H work belongs in G.

The estimate is driven by interpreter/state/graph surfaces, not bytes.

## 20. Architecture decision summary

- Exact static field bytecode: **207,330 bytes in 523 module resources**
  (206,638 main + 692 mystery gift; see §18.1 for the 1,047-B reconciliation
  from the former 206,283 figure).
- Exact address operands: **16,704** (the earlier 16,651 is exact for the main
  `event_scripts.o`; mystery-gift adds 53).
- Script-target edges: **8,208**; interior occurrences: **8,113**.
- Text edges: **6,207**; movement edges: **2,009**; static+RAM data edges:
  **280**.
- Static State-v5 script pointer surfaces: **27 possible slots**; dynamic
  Mystery Event: **22**; immediate Context2: **21 prohibited transient slots**.
- Resource model: per-map/common/gift graph-aware module with sparse typed
  segments and stable exports/interior offsets.
- Runtime model: exact unmodified four-byte operands plus typed sidecar
  resolution at operand consumption; no bytecode patching/widening.
- `sAddressOffset`: replace with a captured virtual-base/buffer identity anchor
  after vaddress parity; do not merely delete or zero it.
- Six waves: **G1 grammar/graph/F-coordinate gate; G2 resources/dependencies;
  G3 resolver seam; G4 State-v5; G5 live cutover; G6 isolation**.
- Highest risk: interior IP/return-address identity across fresh-process state
  relocation.
- STOP-level blocker found: current R13-F coordinate-event wire decode uses
  script offset 8 instead of qualified GBA offset 12. It must be corrected and
  independently proven in G1. No State-v5 format blocker was found.

STOP. This document does not implement R13-G, modify production code, commit,
or begin R13-H.
