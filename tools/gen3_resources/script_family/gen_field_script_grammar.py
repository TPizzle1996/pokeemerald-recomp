#!/usr/bin/env python3
"""R13-G1: Emerald field-script opcode grammar generator.

Produces a machine-readable grammar record for all 227 opcodes (0x00-0xE2)
of the Emerald field script interpreter. Each opcode row contains:
  opcode, canonical name, fixed size or dynamic rule, operand list with
  types, control-flow class, maySuspend, terminal/return behavior.

Special cases:
  - trainerbattle 0x5C: 13 legal shapes, dynamic by type byte
  - C7-CC/D0: no-op (consume only the opcode byte)
  - hidemoneybox 0x94: no operands consumed at runtime (source macro's two
    zero bytes execute as two following NOPs)
  - map-script routing tables, conditional map-script rows
  - mart/decor lists (typed halfword arrays with sentinel)

Output: tools/gen3_resources/script_family/field_script_grammar.generated.toml
--check: verify the file exists and matches deterministic regeneration.

Usage:
  gen_field_script_grammar.py [--check]
"""

import argparse
import sys
from pathlib import Path


def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


# ─── operand type vocabulary ───────────────────────────────────────────
# U8, U16_LE, U32_LE: scalar unsigned integers
# VAR16: 16-bit variable index (gSaveBlock1Ptr->vars[index])
# ADDR32_SCRIPT: 4-byte GBA address of a field-script instruction
# ADDR32_TEXT: 4-byte GBA address of a text string
# ADDR32_MOVEMENT: 4-byte GBA address of movement bytecode
# ADDR32_DATA: 4-byte GBA address of static script-local data (mart, table)
# ADDR32_NATIVE_FUNCTION: 4-byte native function pointer (callnative/gotonative)
# SPECIAL_ID: 16-bit special callback index (gSpecials table)

# ─── flow classes ──────────────────────────────────────────────────────
# fallthrough: normal sequential execution
# jump: unconditional branch (goto, gotostd, returnram, etc.)
# call: push return address, branch (call, callstd)
# conditional_jump: branch if condition matches (goto_if)
# conditional_call: call if condition matches (call_if)
# native_jump: switch to native mode (gotonative)
# end: stop script context (end, endram)
# return: pop return address (return)
# suspend: pause script context, wait for callback (waitstate, delay, etc.)


def build_grammar():
    """Construct the complete opcode grammar as a list of dicts."""

    # Shorthand for operand types
    b = "U8"
    h = "U16_LE"
    w = "U32_LE"
    S = "ADDR32_SCRIPT"
    T = "ADDR32_TEXT"
    M = "ADDR32_MOVEMENT"
    D = "ADDR32_DATA"
    F = "ADDR32_NATIVE_FUNCTION"
    sp = "SPECIAL_ID"
    vr = "VAR16"

    ops = []

    def add(opcode, name, operands, size, flow="fallthrough",
            may_suspend=False, terminal=False, notes=""):
        ops.append(dict(
            opcode=opcode,
            name=name,
            operand_types=operands,
            encoded_size=size,
            flow=flow,
            may_suspend=may_suspend,
            terminal=terminal,
            notes=notes,
        ))

    # 0x00-0x0F
    add(0x00, "nop", [], 1, notes="no operation")
    add(0x01, "nop1", [], 1, notes="no operation (alternate)")
    add(0x02, "end", [], 1, flow="end", terminal=True,
        notes="stop script context")
    add(0x03, "return", [], 1, flow="return",
        notes="pop return address from call stack")
    add(0x04, "call", [S], 5, flow="call",
        notes="push return address, jump to target script")
    add(0x05, "goto", [S], 5, flow="jump",
        notes="unconditional jump to target script")
    add(0x06, "goto_if", [b, S], 6, flow="conditional_jump",
        notes="jump if condition matches comparison result")
    add(0x07, "call_if", [b, S], 6, flow="conditional_call",
        notes="call if condition matches comparison result")
    add(0x08, "gotostd", [b], 2, flow="jump",
        notes="jump to standard script by index (gStdScripts)")
    add(0x09, "callstd", [b], 2, flow="call",
        notes="call standard script by index (gStdScripts)")
    add(0x0A, "gotostd_if", [b, b], 3, flow="conditional_jump",
        notes="conditional jump to standard script")
    add(0x0B, "callstd_if", [b, b], 3, flow="conditional_call",
        notes="conditional call to standard script")
    add(0x0C, "returnram", [], 1, flow="jump",
        notes="return to gRamScriptRetAddr")
    add(0x0D, "endram", [], 1, flow="end", terminal=True,
        notes="clear RAM script, stop context")
    add(0x0E, "setmysteryeventstatus", [b], 2,
        notes="set mystery event status byte")
    add(0x0F, "loadword", [b, w], 6,
        notes="load 32-bit value into local data array (scalar, not a pointer)")

    # 0x10-0x1F
    add(0x10, "loadbyte", [b, b], 3,
        notes="load 8-bit value into local data array")
    add(0x11, "setptr", [b, D], 6,
        notes="store data pointer in local data array")
    add(0x12, "loadbytefromptr", [b, D], 6,
        notes="load byte from data pointer into local data array")
    add(0x13, "setptrbyte", [b, D], 6,
        notes="store byte at data pointer from local data array")
    add(0x14, "copylocal", [b, b], 3,
        notes="copy local data between slots")
    add(0x15, "copybyte", [D, D], 9,
        notes="copy byte between two data pointers")
    add(0x16, "setvar", [vr, h], 5,
        notes="set variable to value")
    add(0x17, "addvar", [vr, h], 5,
        notes="add value to variable")
    add(0x18, "subvar", [vr, h], 5,
        notes="subtract variable from variable")
    add(0x19, "copyvar", [vr, vr], 5,
        notes="copy variable to variable")
    add(0x1A, "setorcopyvar", [vr, vr], 5,
        notes="set or copy variable (depends on context)")
    add(0x1B, "compare_local_to_local", [b, b], 3,
        notes="compare two local data slots")
    add(0x1C, "compare_local_to_value", [b, b], 3,
        notes="compare local data slot to immediate value")
    add(0x1D, "compare_local_to_ptr", [b, D], 6,
        notes="compare local data slot to dereferenced pointer")
    add(0x1E, "compare_ptr_to_local", [D, b], 6,
        notes="compare dereferenced pointer to local data slot")
    add(0x1F, "compare_ptr_to_value", [D, b], 6,
        notes="compare dereferenced pointer to immediate value")

    # 0x20-0x2F
    add(0x20, "compare_ptr_to_ptr", [D, D], 9,
        notes="compare two dereferenced pointers")
    add(0x21, "compare_var_to_value", [vr, h], 5,
        notes="compare variable to immediate value")
    add(0x22, "compare_var_to_var", [vr, vr], 5,
        notes="compare two variables")
    add(0x23, "callnative", [F], 5,
        notes="call native function (does not switch mode)")
    add(0x24, "gotonative", [F], 5, flow="native_jump",
        notes="switch to native execution mode")
    add(0x25, "special", [sp], 3,
        notes="call special callback by index (gSpecials)")
    add(0x26, "specialvar", [vr, sp], 5,
        notes="call special callback, store result in variable")
    add(0x27, "waitstate", [], 1, flow="suspend", may_suspend=True,
        notes="suspend script until native callback resumes it")
    add(0x28, "delay", [h], 3, flow="suspend", may_suspend=True,
        notes="suspend script for N frames")
    add(0x29, "setflag", [h], 3,
        notes="set game flag")
    add(0x2A, "clearflag", [h], 3,
        notes="clear game flag")
    add(0x2B, "checkflag", [h], 3,
        notes="check game flag")
    add(0x2C, "initclock", [h, h], 5,
        notes="initialize RTC clock (hour, minute)")
    add(0x2D, "dotimebasedevents", [], 1,
        notes="process time-based events")
    add(0x2E, "gettime", [], 1,
        notes="get current time")
    add(0x2F, "playse", [h], 3,
        notes="play sound effect")

    # 0x30-0x3F
    add(0x30, "waitse", [], 1, flow="suspend", may_suspend=True,
        notes="wait for sound effect to finish")
    add(0x31, "playfanfare", [h], 3,
        notes="play fanfare")
    add(0x32, "waitfanfare", [], 1, flow="suspend", may_suspend=True,
        notes="wait for fanfare to finish")
    add(0x33, "playbgm", [h, b], 4,
        notes="play background music (songid, save)")
    add(0x34, "savebgm", [h], 3,
        notes="save current BGM to resume later")
    add(0x35, "fadedefaultbgm", [], 1,
        notes="fade to default map BGM")
    add(0x36, "fadenewbgm", [h], 3,
        notes="fade to new BGM")
    add(0x37, "fadeoutbgm", [b], 2,
        notes="fade out BGM at speed")
    add(0x38, "fadeinbgm", [b], 2,
        notes="fade in BGM at speed")
    add(0x39, "warp", [b, b, b, h, h], 8,
        notes="warp to map (group, num, warpId, x, y)")
    add(0x3A, "warpsilent", [b, b, b, h, h], 8,
        notes="silent warp (no animation)")
    add(0x3B, "warpdoor", [b, b, b, h, h], 8,
        notes="door warp animation")
    add(0x3C, "warphole", [b, b], 3,
        notes="hole warp (group, num)")
    add(0x3D, "warpteleport", [b, b, b, h, h], 8,
        notes="teleport warp")
    add(0x3E, "setwarp", [b, b, b, h, h], 8,
        notes="set warp destination")
    add(0x3F, "setdynamicwarp", [b, b, b, h, h], 8,
        notes="set dynamic warp destination")

    # 0x40-0x4F
    add(0x40, "setdivewarp", [b, b, b, h, h], 8,
        notes="set dive warp destination")
    add(0x41, "setholewarp", [b, b, b, h, h], 8,
        notes="set hole warp destination")
    add(0x42, "getplayerxy", [vr, vr], 5,
        notes="store player X,Y in variables")
    add(0x43, "getpartysize", [], 1,
        notes="get party size (stores in gSpecialVar_Result)")
    add(0x44, "additem", [h, h], 5,
        notes="add item to bag")
    add(0x45, "removeitem", [h, h], 5,
        notes="remove item from bag")
    add(0x46, "checkitemspace", [h, h], 5,
        notes="check bag space for item")
    add(0x47, "checkitem", [h, h], 5,
        notes="check if player has item")
    add(0x48, "checkitemtype", [h], 3,
        notes="check item type")
    add(0x49, "addpcitem", [h, h], 5,
        notes="add item to PC")
    add(0x4A, "checkpcitem", [h, h], 5,
        notes="check if PC has item")
    add(0x4B, "adddecoration", [h], 3,
        notes="add decoration to PC")
    add(0x4C, "removedecoration", [h], 3,
        notes="remove decoration from PC")
    add(0x4D, "checkdecor", [h], 3,
        notes="check if player has decoration")
    add(0x4E, "checkdecorspace", [h], 3,
        notes="check decoration space")
    add(0x4F, "applymovement", [h, M], 7,
        notes="apply movement script to object")

    # 0x50-0x5B
    add(0x50, "applymovementat", [h, M, b, b], 9,
        notes="apply movement script to object at map")
    add(0x51, "waitmovement", [h], 3,
        notes="wait for object movement to finish")
    add(0x52, "waitmovementat", [h, b, b], 5,
        notes="wait for movement at map to finish")
    add(0x53, "removeobject", [h], 3,
        notes="remove object from map")
    add(0x54, "removeobjectat", [h, b, b], 5,
        notes="remove object at map")
    add(0x55, "addobject", [h], 3,
        notes="add object to map")
    add(0x56, "addobjectat", [h, b, b], 5,
        notes="add object at map")
    add(0x57, "setobjectxy", [h, h, h], 7,
        notes="set object position")
    add(0x58, "showobjectat", [h, b, b], 5,
        notes="show object at map")
    add(0x59, "hideobjectat", [h, b, b], 5,
        notes="hide object at map")
    add(0x5A, "faceplayer", [], 1,
        notes="turn object to face player")
    add(0x5B, "turnobject", [h, b], 4,
        notes="turn object to direction")

    # 0x5C: trainerbattle (dynamic)
    add(0x5C, "trainerbattle", [], 0, flow="dynamic",
        notes="trainer battle (dynamic: see trainerbattle_types)")

    # 0x5D-0x5F
    add(0x5D, "dotrainerbattle", [], 1, flow="suspend", may_suspend=True,
        notes="start trainer battle")
    add(0x5E, "gotopostbattlescript", [], 1, flow="jump",
        notes="jump to post-battle script")
    add(0x5F, "gotobeatenscript", [], 1, flow="jump",
        notes="jump to beaten script")

    # 0x60-0x6F
    add(0x60, "checktrainerflag", [h], 3,
        notes="check if trainer has been defeated")
    add(0x61, "settrainerflag", [h], 3,
        notes="set trainer defeated flag")
    add(0x62, "cleartrainerflag", [h], 3,
        notes="clear trainer defeated flag")
    add(0x63, "setobjectxyperm", [h, h, h], 7,
        notes="set object permanent position")
    add(0x64, "copyobjectxytoperm", [h], 3,
        notes="copy object position to permanent")
    add(0x65, "setobjectmovementtype", [h, b], 4,
        notes="set object movement type")
    add(0x66, "waitmessage", [], 1, flow="suspend", may_suspend=True,
        notes="wait for message box to close")
    add(0x67, "message", [T], 5,
        notes="display text message")
    add(0x68, "closemessage", [], 1,
        notes="close message box")
    add(0x69, "lockall", [], 1,
        notes="lock all objects")
    add(0x6A, "lock", [], 1,
        notes="lock current object")
    add(0x6B, "releaseall", [], 1,
        notes="release all objects")
    add(0x6C, "release", [], 1,
        notes="release current object")
    add(0x6D, "waitbuttonpress", [], 1, flow="suspend", may_suspend=True,
        notes="wait for button press")
    add(0x6E, "yesnobox", [b, b], 3,
        notes="display yes/no box")
    add(0x6F, "multichoice", [b, b, b, b], 5,
        notes="display multi-choice box")

    # 0x70-0x7F
    add(0x70, "multichoicedefault", [b, b, b, b, b], 6,
        notes="display multi-choice box with default")
    add(0x71, "multichoicegrid", [b, b, b, b, b], 6,
        notes="display multi-choice grid")
    add(0x72, "drawbox", [], 1,
        notes="draw box (Emerald: no-op, body commented out)")
    add(0x73, "erasebox", [b, b, b, b], 5,
        notes="erase box (no-op in Emerald)")
    add(0x74, "drawboxtext", [b, b, b, b], 5,
        notes="draw box text (no-op in Emerald)")
    add(0x75, "showmonpic", [h, b, b], 5,
        notes="show Pokemon picture")
    add(0x76, "hidemonpic", [], 1,
        notes="hide Pokemon picture")
    add(0x77, "showcontestpainting", [b], 2,
        notes="show contest painting")
    add(0x78, "braillemessage", [T], 5,
        notes="display braille message")
    add(0x79, "givemon", [h, b, h, w, w, b], 15,
        notes="give Pokemon to player (words are scalar, not pointers)")
    add(0x7A, "giveegg", [h], 3,
        notes="give egg to player")
    add(0x7B, "setmonmove", [b, b, h], 5,
        notes="set Pokemon move")
    add(0x7C, "checkpartymove", [h], 3,
        notes="check if party has move")
    add(0x7D, "bufferspeciesname", [b, h], 4,
        notes="buffer species name into string var")
    add(0x7E, "bufferleadmonspeciesname", [b], 2,
        notes="buffer lead Pokemon species name")
    add(0x7F, "bufferpartymonnick", [b, h], 4,
        notes="buffer party Pokemon nickname")

    # 0x80-0x8F
    add(0x80, "bufferitemname", [b, h], 4,
        notes="buffer item name")
    add(0x81, "bufferdecorationname", [b, h], 4,
        notes="buffer decoration name")
    add(0x82, "buffermovename", [b, h], 4,
        notes="buffer move name")
    add(0x83, "buffernumberstring", [b, h], 4,
        notes="buffer number as string")
    add(0x84, "bufferstdstring", [b, h], 4,
        notes="buffer standard string by index")
    add(0x85, "bufferstring", [b, T], 6,
        notes="buffer string from text pointer")
    add(0x86, "pokemart", [D], 5,
        notes="open Pokemart with item list")
    add(0x87, "pokemartdecoration", [D], 5,
        notes="open decoration shop (type 1)")
    add(0x88, "pokemartdecoration2", [D], 5,
        notes="open decoration shop (type 2)")
    add(0x89, "playslotmachine", [h], 3,
        notes="play slot machine")
    add(0x8A, "setberrytree", [b, b, b], 4,
        notes="set berry tree state")
    add(0x8B, "choosecontestmon", [], 1,
        notes="choose contest Pokemon")
    add(0x8C, "startcontest", [], 1,
        notes="start contest")
    add(0x8D, "showcontestresults", [], 1,
        notes="show contest results")
    add(0x8E, "contestlinktransfer", [], 1,
        notes="contest link transfer")
    add(0x8F, "random", [h], 3,
        notes="generate random number")

    # 0x90-0x9F
    add(0x90, "addmoney", [w, b], 6,
        notes="add money (word is scalar, not pointer)")
    add(0x91, "removemoney", [w, b], 6,
        notes="remove money")
    add(0x92, "checkmoney", [w, b], 6,
        notes="check money")
    add(0x93, "showmoneybox", [b, b, b], 4,
        notes="show money box")
    add(0x94, "hidemoneybox", [], 1,
        notes="hide money box (operand reads commented out; "
              "source macro's two zero bytes execute as two NOPs)")
    add(0x95, "updatemoneybox", [b, b, b], 4,
        notes="update money box")
    add(0x96, "getpokenewsactive", [h], 3,
        notes="check if Pok\u00e9News is active")
    add(0x97, "fadescreen", [b], 2,
        notes="fade screen (mode)")
    add(0x98, "fadescreenspeed", [b, b], 3,
        notes="fade screen at speed")
    add(0x99, "setflashlevel", [h], 3,
        notes="set flash level")
    add(0x9A, "animateflash", [b], 2,
        notes="animate flash")
    add(0x9B, "messageautoscroll", [T], 5,
        notes="display auto-scrolling message")
    add(0x9C, "dofieldeffect", [h], 3,
        notes="do field effect")
    add(0x9D, "setfieldeffectargument", [b, h], 4,
        notes="set field effect argument")
    add(0x9E, "waitfieldeffect", [h], 3,
        notes="wait for field effect")
    add(0x9F, "setrespawn", [h], 3,
        notes="set respawn location (heal location index)")

    # 0xA0-0xAF
    add(0xA0, "checkplayergender", [], 1,
        notes="check player gender")
    add(0xA1, "playmoncry", [h, h], 5,
        notes="play Pokemon cry")
    add(0xA2, "setmetatile", [h, h, h, h], 9,
        notes="set metatile at position")
    add(0xA3, "resetweather", [], 1,
        notes="reset weather to default")
    add(0xA4, "setweather", [h], 3,
        notes="set weather")
    add(0xA5, "doweather", [], 1,
        notes="do weather effects")
    add(0xA6, "setstepcallback", [b], 2,
        notes="set step callback")
    add(0xA7, "setmaplayoutindex", [h], 3,
        notes="set map layout index")
    add(0xA8, "setobjectsubpriority", [h, b, b, b], 6,
        notes="set object sub-priority")
    add(0xA9, "resetobjectsubpriority", [h, b, b], 5,
        notes="reset object sub-priority")
    add(0xAA, "createvobject", [b, b, h, h, b, b], 9,
        notes="create virtual object")
    add(0xAB, "turnvobject", [b, b], 3,
        notes="turn virtual object")
    add(0xAC, "opendoor", [h, h], 5,
        notes="open door at position")
    add(0xAD, "closedoor", [h, h], 5,
        notes="close door at position")
    add(0xAE, "waitdooranim", [], 1,
        notes="wait for door animation")
    add(0xAF, "setdooropen", [h, h], 5,
        notes="set door as open")

    # 0xB0-0xBF
    add(0xB0, "setdoorclosed", [h, h], 5,
        notes="set door as closed")
    add(0xB1, "addelevmenuitem", [b, h, h, h], 8,
        notes="add elevator menu item (no-op in Emerald)")
    add(0xB2, "showelevmenu", [], 1,
        notes="show elevator menu (no-op in Emerald)")
    add(0xB3, "checkcoins", [vr], 3,
        notes="check coin count")
    add(0xB4, "addcoins", [h], 3,
        notes="add coins")
    add(0xB5, "removecoins", [h], 3,
        notes="remove coins")
    add(0xB6, "setwildbattle", [h, b, h], 6,
        notes="set wild battle parameters")
    add(0xB7, "dowildbattle", [], 1, flow="suspend", may_suspend=True,
        notes="start wild battle")
    add(0xB8, "setvaddress", [S], 5,
        notes="set virtual address base (V-script target)")
    add(0xB9, "vgoto", [S], 5, flow="jump",
        notes="virtual goto (V-script target)")
    add(0xBA, "vcall", [S], 5, flow="call",
        notes="virtual call (V-script target)")
    add(0xBB, "vgoto_if", [b, S], 6, flow="conditional_jump",
        notes="conditional virtual goto (V-script target)")
    add(0xBC, "vcall_if", [b, S], 6, flow="conditional_call",
        notes="conditional virtual call (V-script target)")
    add(0xBD, "vmessage", [T], 5,
        notes="virtual message (V-text target)")
    add(0xBE, "vbuffermessage", [T], 5,
        notes="virtual buffer message (V-text target)")
    add(0xBF, "vbufferstring", [b, T], 6,
        notes="virtual buffer string (V-text target)")

    # 0xC0-0xCF
    add(0xC0, "showcoinsbox", [b, b], 3,
        notes="show coins box")
    add(0xC1, "hidecoinsbox", [b, b], 3,
        notes="hide coins box")
    add(0xC2, "updatecoinsbox", [b, b], 3,
        notes="update coins box")
    add(0xC3, "incrementgamestat", [b], 2,
        notes="increment game stat")
    add(0xC4, "setescapewarp", [b, b, b, h, h], 8,
        notes="set escape rope warp destination")
    add(0xC5, "waitmoncry", [], 1,
        notes="wait for Pokemon cry to finish")
    add(0xC6, "bufferboxname", [b, h], 4,
        notes="buffer PC box name")
    add(0xC7, "textcolor", [], 1,
        notes="RS text color command; Emerald no-op (nop1)")
    add(0xC8, "loadhelp", [], 1,
        notes="RS load help system; Emerald no-op (nop1)")
    add(0xC9, "unloadhelp", [], 1,
        notes="RS unload help system; Emerald no-op (nop1)")
    add(0xCA, "signmsg", [], 1,
        notes="RS signpost message; Emerald no-op (nop1)")
    add(0xCB, "normalmsg", [], 1,
        notes="RS normal message; Emerald no-op (nop1)")
    add(0xCC, "comparehiddenvar", [], 1,
        notes="RS compare hidden var; Emerald no-op (nop1)")
    add(0xCD, "setmodernfatefulencounter", [h], 3,
        notes="set modern fateful encounter flag")
    add(0xCE, "checkmodernfatefulencounter", [h], 3,
        notes="check modern fateful encounter flag")
    add(0xCF, "trywondercardscript", [], 1, flow="conditional_jump",
        notes="try to execute Wonder Card script")

    # 0xD0-0xDF
    add(0xD0, "setworldmapflag", [], 1,
        notes="RS set world map flag; Emerald no-op (nop1)")
    add(0xD1, "warpspinenter", [b, b, b, h, h], 8,
        notes="spin enter warp")
    add(0xD2, "setmonmetlocation", [h, b], 4,
        notes="set Pokemon met location")
    add(0xD3, "moverotatingtileobjects", [h], 3,
        notes="move rotating tile objects")
    add(0xD4, "turnrotatingtileobjects", [], 1,
        notes="turn rotating tile objects")
    add(0xD5, "initrotatingtilepuzzle", [h], 3,
        notes="initialize rotating tile puzzle")
    add(0xD6, "freerotatingtilepuzzle", [], 1,
        notes="free rotating tile puzzle")
    add(0xD7, "warpmossdeepgym", [b, b, b, h, h], 8,
        notes="warp in Mossdeep Gym")
    add(0xD8, "selectapproachingtrainer", [], 1,
        notes="select approaching trainer")
    add(0xD9, "lockfortrainer", [], 1,
        notes="lock for trainer battle")
    add(0xDA, "closebraillemessage", [], 1,
        notes="close braille message")
    add(0xDB, "messageinstant", [T], 5,
        notes="display instant message (no text speed)")
    add(0xDC, "fadescreenswapbuffers", [b], 2,
        notes="fade screen and swap buffers")
    add(0xDD, "buffertrainerclassname", [b, h], 4,
        notes="buffer trainer class name")
    add(0xDE, "buffertrainername", [b, h], 4,
        notes="buffer trainer name")
    add(0xDF, "pokenavcall", [T], 5,
        notes="Pok\u00e9Nav call")

    # 0xE0-0xE2
    add(0xE0, "warpwhitefade", [b, b, b, h, h], 8,
        notes="white fade warp")
    add(0xE1, "buffercontestname", [b, h], 4,
        notes="buffer contest name")
    add(0xE2, "bufferitemnameplural", [b, h, h], 6,
        notes="buffer item name plural")

    return ops


def build_trainerbattle_types():
    """Build the 13 legal trainerbattle type shapes.
    
    Operand classification:
      T = ADDR32_TEXT   (introText, defeatText, cantBattleText)
      S = ADDR32_SCRIPT (continueScript only)
    The implicit endScript (return address) is stored by the trainer
    loader and is not a grammar operand.
    """
    b = "U8"
    h = "U16_LE"
    T = "ADDR32_TEXT"
    S = "ADDR32_SCRIPT"

    types = []

    # Type 0: single battle (trainerId, localId, introText, defeatText)
    types.append(dict(
        type_id=0,
        name="TRAINER_BATTLE_SINGLE",
        encoded_size=14,
        operands=[h, h, T, T],
        description="single battle: trainerId, localId, introText, defeatText",
    ))

    # Type 1: continue script, no music
    types.append(dict(
        type_id=1,
        name="TRAINER_BATTLE_CONTINUE_SCRIPT_NO_MUSIC",
        encoded_size=18,
        operands=[h, h, T, T, S],
        description="continue script (no music): trainerId, localId, "
                    "introText, defeatText, continueScript",
    ))

    # Type 2: continue script
    types.append(dict(
        type_id=2,
        name="TRAINER_BATTLE_CONTINUE_SCRIPT",
        encoded_size=18,
        operands=[h, h, T, T, S],
        description="continue script: trainerId, localId, introText, "
                    "defeatText, continueScript",
    ))

    # Type 3: no intro text (trainerId, localId, defeatText)
    types.append(dict(
        type_id=3,
        name="TRAINER_BATTLE_SINGLE_NO_INTRO_TEXT",
        encoded_size=10,
        operands=[h, h, T],
        description="single battle (no intro): trainerId, localId, defeatText",
    ))

    # Type 4: double battle (trainerId, localId, introText, defeatText, cantBattleText)
    types.append(dict(
        type_id=4,
        name="TRAINER_BATTLE_DOUBLE",
        encoded_size=18,
        operands=[h, h, T, T, T],
        description="double battle: trainerId, localId, introText, "
                    "defeatText, cantBattleText",
    ))

    # Type 5: rematch (same as type 0)
    types.append(dict(
        type_id=5,
        name="TRAINER_BATTLE_REMATCH",
        encoded_size=14,
        operands=[h, h, T, T],
        description="rematch battle: trainerId, localId, introText, defeatText",
    ))

    # Type 6: continue script double
    types.append(dict(
        type_id=6,
        name="TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE",
        encoded_size=22,
        operands=[h, h, T, T, T, S],
        description="continue script double: trainerId, localId, introText, "
                    "defeatText, cantBattleText, continueScript",
    ))

    # Type 7: rematch double (same as type 4)
    types.append(dict(
        type_id=7,
        name="TRAINER_BATTLE_REMATCH_DOUBLE",
        encoded_size=18,
        operands=[h, h, T, T, T],
        description="rematch double: trainerId, localId, introText, "
                    "defeatText, cantBattleText",
    ))

    # Type 8: continue script double no music (same as type 6)
    types.append(dict(
        type_id=8,
        name="TRAINER_BATTLE_CONTINUE_SCRIPT_DOUBLE_NO_MUSIC",
        encoded_size=22,
        operands=[h, h, T, T, T, S],
        description="continue script double (no music): trainerId, localId, "
                    "introText, defeatText, cantBattleText, continueScript",
    ))

    # Type 9: pyramid (same as type 0)
    types.append(dict(
        type_id=9,
        name="TRAINER_BATTLE_PYRAMID",
        encoded_size=14,
        operands=[h, h, T, T],
        description="pyramid battle: trainerId, localId, introText, defeatText",
    ))

    # Type 10: set trainer A (same as type 0, returns NULL)
    types.append(dict(
        type_id=10,
        name="TRAINER_BATTLE_SET_TRAINER_A",
        encoded_size=14,
        operands=[h, h, T, T],
        description="set trainer A: trainerId, localId, introText, defeatText "
                    "(returns NULL, no event script)",
    ))

    # Type 11: set trainer B (same as type 0, returns NULL)
    types.append(dict(
        type_id=11,
        name="TRAINER_BATTLE_SET_TRAINER_B",
        encoded_size=14,
        operands=[h, h, T, T],
        description="set trainer B: trainerId, localId, introText, defeatText "
                    "(returns NULL, no event script)",
    ))

    # Type 12: hill (same as type 0)
    types.append(dict(
        type_id=12,
        name="TRAINER_BATTLE_HILL",
        encoded_size=14,
        operands=[h, h, T, T],
        description="hill battle: trainerId, localId, introText, defeatText",
    ))

    return types


def build_non_opcode_grammars():
    """Build the non-opcode grammar entries (map dispatch, conditional tables,
    mart/decor lists, mystery event)."""
    S = "ADDR32_SCRIPT"
    vr = "VAR16"
    b = "U8"
    h = "U16_LE"

    grammars = []

    # Map dispatch table
    grammars.append(dict(
        name="map_dispatch_table",
        description="Map script dispatch table: repeated {tag:u8, script:ADDR32_SCRIPT} "
                    "terminated by tag=0. Tags 2 (ON_FRAME_TABLE) and 4 "
                    "(ON_WARP_INTO_MAP_TABLE) point to conditional tables.",
        row_format=[b, S],
        terminator="tag == 0",
        row_size=5,
        conditional_tags=[2, 4],
    ))

    # Conditional map-script table
    grammars.append(dict(
        name="map_conditional_table",
        description="Conditional map-script row table: repeated "
                    "{var:VAR16, value:VAR16, script:ADDR32_SCRIPT} "
                    "terminated by var == 0. First matching row is executed.",
        row_format=[vr, vr, S],
        terminator="var == 0",
        row_size=8,
    ))

    # Mart list
    grammars.append(dict(
        name="mart_list",
        description="Pokemart item list: sequence of u16 item IDs "
                    "terminated by ITEM_NONE (0). Referenced by pokemart command.",
        row_format=[h],
        terminator="ITEM_NONE (0x0000)",
        row_size=2,
    ))

    # Decor shop list
    grammars.append(dict(
        name="decor_shop_list",
        description="Decoration shop item list: sequence of u16 decoration IDs "
                    "terminated by DECOR_NONE (0). Referenced by "
                    "pokemartdecoration/pokemartdecoration2 commands.",
        row_format=[h],
        terminator="DECOR_NONE (0x0000)",
        row_size=2,
    ))

    # Mystery Event grammar (17-op distinct VM)
    grammars.append(dict(
        name="mystery_event_vm",
        description="Mystery Event script interpreter: distinct 17-command "
                    "VM with its own EWRAM context. Not part of the ordinary "
                    "field script language. Recorded for exclusion from "
                    "ordinary classification.",
        opcode_count=17,
        owned_by="mystery_event_script.c",
        is_ordinary_field=False,
    ))

    return grammars


def build_operand_type_table():
    """Build the operand type vocabulary table."""
    return [
        dict(name="U8", width=1, description="unsigned 8-bit integer"),
        dict(name="U16_LE", width=2, description="unsigned 16-bit integer, little-endian"),
        dict(name="U32_LE", width=4, description="unsigned 32-bit integer, little-endian (scalar, not a pointer)"),
        dict(name="VAR16", width=2, description="16-bit variable index into gSaveBlock1Ptr->vars[]"),
        dict(name="SPECIAL_ID", width=2, description="16-bit special callback index into gSpecials[]"),
        dict(name="ADDR32_SCRIPT", width=4, description="32-bit GBA address of a field-script instruction (SCRIPT_TARGET)"),
        dict(name="ADDR32_TEXT", width=4, description="32-bit GBA address of a text string (TEXT_TARGET)"),
        dict(name="ADDR32_MOVEMENT", width=4, description="32-bit GBA address of movement bytecode (MOVEMENT_TARGET)"),
        dict(name="ADDR32_DATA", width=4, description="32-bit GBA address of static script-local data (MART_TABLE_TARGET / RAW_DATA_TARGET)"),
        dict(name="ADDR32_NATIVE_FUNCTION", width=4, description="32-bit native function pointer (callnative/gotonative)"),
    ]


def build_flow_class_table():
    """Build the flow class vocabulary table."""
    return [
        dict(name="fallthrough", description="normal sequential execution"),
        dict(name="jump", description="unconditional branch"),
        dict(name="call", description="push return address, branch"),
        dict(name="conditional_jump", description="branch if condition matches"),
        dict(name="conditional_call", description="call if condition matches"),
        dict(name="native_jump", description="switch to native execution mode"),
        dict(name="end", description="stop script context"),
        dict(name="return", description="pop return address"),
        dict(name="suspend", description="pause script, wait for callback"),
        dict(name="dynamic", description="variable-length instruction (trainerbattle)"),
    ]


# ─── TOML emission ─────────────────────────────────────────────────────

def emit_toml(ops, trainer_types, non_opcode_grammars, operand_types,
              flow_classes):
    """Emit the complete grammar TOML."""
    lines = []
    lines.append("# Generated by tools/gen3_resources/script_family/gen_field_script_grammar.py")
    lines.append("# Do not edit by hand; re-run the generator (regeneration must be")
    lines.append("# a no-op diff).")
    lines.append("")
    lines.append("# ─── Emerald field-script opcode grammar ───")
    lines.append("# All 227 opcodes (0x00-0xE2) of the ordinary field script")
    lines.append("# interpreter, plus non-opcode grammars (map dispatch, conditional")
    lines.append("# tables, mart/decor lists, mystery-event VM).")
    lines.append("")
    lines.append("grammar_version = 1")
    lines.append('game = "emerald"')
    lines.append('rom_profile = "bpee01-rev0"')
    lines.append("")

    # Operand types
    lines.append("# ─── operand type vocabulary ───")
    lines.append("")
    for ot in operand_types:
        lines.append("[[operand_types]]")
        lines.append(f'name = "{ot["name"]}"')
        lines.append(f"width = {ot['width']}")
        lines.append(f'description = "{ot["description"]}"')
        lines.append("")

    # Flow classes
    lines.append("# ─── flow class vocabulary ───")
    lines.append("")
    for fc in flow_classes:
        lines.append("[[flow_classes]]")
        lines.append(f'name = "{fc["name"]}"')
        lines.append(f'description = "{fc["description"]}"')
        lines.append("")

    # Opcodes
    lines.append("# ─── opcode table (0x00-0xE2) ───")
    lines.append("")
    for op in ops:
        lines.append("[[opcodes]]")
        lines.append(f'opcode = {op["opcode"]}')
        lines.append(f'name = "{op["name"]}"')
        if op["operand_types"]:
            ops_str = ", ".join(f'"{t}"' for t in op["operand_types"])
            lines.append(f"operand_types = [{ops_str}]")
        else:
            lines.append("operand_types = []")
        lines.append(f'encoded_size = {op["encoded_size"]}')
        lines.append(f'flow = "{op["flow"]}"')
        lines.append(f'may_suspend = {str(op["may_suspend"]).lower()}')
        lines.append(f'terminal = {str(op["terminal"]).lower()}')
        if op["notes"]:
            lines.append(f'notes = "{op["notes"]}"')
        lines.append("")

    # Trainerbattle types
    lines.append("# ─── trainerbattle dynamic type shapes ───")
    lines.append("# 13 legal types; unknown type = malformed bytecode")
    lines.append("")
    for tt in trainer_types:
        lines.append("[[trainerbattle_types]]")
        lines.append(f'type_id = {tt["type_id"]}')
        lines.append(f'name = "{tt["name"]}"')
        lines.append(f'encoded_size = {tt["encoded_size"]}')
        ops_str = ", ".join(f'"{t}"' for t in tt["operands"])
        lines.append(f"operand_types = [{ops_str}]")
        lines.append(f'description = "{tt["description"]}"')
        lines.append("")

    # Non-opcode grammars
    lines.append("# ─── non-opcode grammars ───")
    lines.append("")
    for ng in non_opcode_grammars:
        lines.append("[[non_opcode_grammars]]")
        lines.append(f'name = "{ng["name"]}"')
        lines.append(f'description = "{ng["description"]}"')
        if "row_format" in ng:
            fmt_str = ", ".join(f'"{t}"' for t in ng["row_format"])
            lines.append(f"row_format = [{fmt_str}]")
            lines.append(f'terminator = "{ng["terminator"]}"')
            lines.append(f"row_size = {ng['row_size']}")
            if "conditional_tags" in ng:
                tags_str = ", ".join(str(t) for t in ng["conditional_tags"])
                lines.append(f"conditional_tags = [{tags_str}]")
        if "opcode_count" in ng:
            lines.append(f"opcode_count = {ng['opcode_count']}")
        if "owned_by" in ng:
            lines.append(f'owned_by = "{ng["owned_by"]}"')
        if "is_ordinary_field" in ng:
            lines.append(f'is_ordinary_field = {str(ng["is_ordinary_field"]).lower()}')
        lines.append("")

    return "\n".join(lines) + "\n"


def write_if(path, text, check):
    if check:
        if not path.exists():
            fail(f"--check: {path} does not exist")
        if path.read_text() != text:
            fail(f"--check: {path} differs from deterministic regeneration")
        print(f"check passed: {path}")
    else:
        path.write_text(text)
        print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    ops = build_grammar()
    trainer_types = build_trainerbattle_types()
    non_opcode_grammars = build_non_opcode_grammars()
    operand_types = build_operand_type_table()
    flow_classes = build_flow_class_table()

    # Validate opcode coverage
    seen = set()
    for op in ops:
        if op["opcode"] in seen:
            fail(f"duplicate opcode {op['opcode']:02X}")
        seen.add(op["opcode"])
    for i in range(0xE3):
        if i not in seen:
            fail(f"missing opcode {i:02X}")
    if len(ops) != 227:
        fail(f"expected 227 opcodes, got {len(ops)}")

    # Validate trainer types
    seen_types = set()
    for tt in trainer_types:
        if tt["type_id"] in seen_types:
            fail(f"duplicate trainer type {tt['type_id']}")
        seen_types.add(tt["type_id"])
    if len(trainer_types) != 13:
        fail(f"expected 13 trainer types, got {len(trainer_types)}")

    outdir = Path(__file__).resolve().parent
    text = emit_toml(ops, trainer_types, non_opcode_grammars, operand_types,
                     flow_classes)
    write_if(outdir / "field_script_grammar.generated.toml", text, args.check)

    print(f"grammar: {len(ops)} opcodes, {len(trainer_types)} trainer types, "
          f"{len(non_opcode_grammars)} non-opcode grammars, "
          f"{len(operand_types)} operand types, {len(flow_classes)} flow classes")


if __name__ == "__main__":
    main()