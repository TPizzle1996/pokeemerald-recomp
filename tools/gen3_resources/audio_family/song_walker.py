#!/usr/bin/env python3
"""R12-D: engine-faithful MP2K song-graph stream walker.

Parses each song's track streams exactly as the native MP2K engine
consumes them (src/music_player.c and src/m4a.c are the authority;
the GBA asm it ports matches byte-for-byte). Two passes per track:

  * sweep_track  - LINEAR parse of the track's stream region with the
                   engine's running-status state machine, counting every
                   command position and validating every address operand.
                   This is the pinned-count authority: the stage pins
                   (GOTO 1,438 / PATT 6,942 / REPT 0 / xwave 0 = 8,380
                   4-byte operands, XCMD 848, notes 162,040, ...) are
                   static stream statistics, cross-checked against the
                   .s sources over all 530 songs.
  * walk_track   - CONTROL-FLOW walk of the same stream the way the GBA
                   player executes it (pattern stack, PEND returns, loop
                   detection at a jump back to an already-executed
                   (position, stack) state). Ends at FINE or a loop
                   "within the slice", exactly like the player. Every
                   reachable target must be interior; the walk must
                   terminate.

Address classification for every 4-byte operand:
  * GOTO/PATT/REPT/memacc-jump targets must lie inside the OWNING song's
    stream region [slice start, header start) - a dangling target fails.
  * XCMD xwave operands must be programmable-wave start addresses (0 in
    song graphs today; the gate stays ready).
Header validation: trackCount byte (0..10) with 8+4*N == slice size,
`tone` must hit the 197-label voicegroup/cry-label set, part pointers
must be interior and strictly increasing with part[0] == slice start.

Width model (verified against the engine source, NOT the R12-D plan
text - the plan's §6 list has two typos the engine corrects):
  * 0xCC PORT consumes 2 bytes on native (music_player.c:327-336)
  * 0xC2 LFOS consumes 1 byte (music_player.c:727-732), not 2
  * 0xCE ENDTIE consumes an optional 1-byte key when the next byte is
    < 0x80 (music_player.c:708-715)
"""
import struct

# --- engine width model ----------------------------------------------------

GEN3_GBA_ROM_BASE = 0x08000000

# XCMD (0xCD) sub-command operand widths (gXcmdTable, m4a_tables.c;
# handlers in m4a.c). Sub 0x00/0x03 (ply_xxx) jump to gMPlayJumpTable[0]
# (fine) and END the track. Sub 0x01 (ply_xwave) is a 4-byte address.
XCMD_SUB_WIDTH = {0x00: 0, 0x01: 4, 0x02: 1, 0x03: 0,
                  0x04: 1, 0x05: 1, 0x06: 1, 0x07: 1, 0x08: 1, 0x09: 1,
                  0x0A: 1, 0x0B: 1, 0x0C: 2, 0x0D: 4}
XCMD_FINE_SUBS = frozenset((0x00, 0x03))
XCMD_WAVE_SUB = 0x01
MEMACC_JUMP_OPS = frozenset(range(6, 18))
# Single-byte-operand events: 0xBA prio, 0xBB tempo, 0xBC keysh, 0xBD voice,
# 0xBE vol, 0xBF pan, 0xC0 bend, 0xC1 bendr, 0xC3 lfodl, 0xC4 mod,
# 0xC5 modt, 0xC8 tune. 0xC2 lfos is also 1 byte (see module docstring);
# 0xCC port is 2 bytes.
ONE_BYTE_CMDS = frozenset((0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1,
                           0xC2, 0xC3, 0xC4, 0xC5, 0xC8))
# 0-operand events that END the track (all aliases of FINE).
FINE_CMDS = frozenset((0xB1, 0xB6, 0xB7, 0xB8, 0xC6, 0xC7, 0xC9, 0xCA,
                       0xCB))

# R12-D stage pins. Proven two ways: (a) engine-faithful sweep over the
# retail-qualified ROM (ELF == ROM == manifest three-way), and (b) a
# per-song line-by-line cross-check of every .s source (0 diffs over all
# 530 songs for GOTO/PATT/PEND/XCMD-sub/note categories). The values
# REPLACE the R12-D plan's §6 prototype counts (GOTO 1,412 / PATT 6,873 /
# XCMD 293 / notes 158,911 / operands 8,285 / targets 3,906): the
# prototype under-counted running-status continuations and the plan text
# did not reflect it (see docs/R12D_SONG_GRAPH_MIGRATION_REPORT.md).
PIN_TOTAL_BYTES = 684052          # 0xA7014, mus 651184 + se 31304 + ph 1564
PIN_GOTO = 1438
PIN_PATT = 6942
PIN_REPT = 0
PIN_MEMACC_JUMP = 0
PIN_XWAVE = 0
PIN_OPERANDS = 8380               # goto + patt + rept + memacc-jump + xwave
PIN_MEMACC = 1                    # total memacc events (the 1 is non-jump)
PIN_XCMD = 848
PIN_XCMD_SUBS = {0x08: 687, 0x09: 161}   # xIECV / xIECL only
PIN_NOTES = 162040
PIN_PARTS = 2082
PIN_DISTINCT_TONES = 176
PIN_DISTINCT_TARGETS = 3981
PIN_LOCAL_LABELS = 7740


class WalkError(Exception):
    pass


def _rd8(rom, pos):
    if pos < GEN3_GBA_ROM_BASE:
        raise WalkError(f"read below ROM base at {pos:#x}")
    return rom[pos - GEN3_GBA_ROM_BASE]


def _rd32(rom, pos):
    if pos < GEN3_GBA_ROM_BASE:
        raise WalkError(f"read below ROM base at {pos:#x}")
    return struct.unpack_from("<I", rom, pos - GEN3_GBA_ROM_BASE)[0]


def _new_stats():
    return {"wait": 0, "note": 0, "fine": 0, "goto": 0, "patt": 0,
            "pend": 0, "rept": 0, "memacc": 0, "memacc_jump": 0,
            "xcmd": 0, "xcmd_sub": {}, "xwave": 0, "onebyte": 0,
            "port": 0, "endtie": 0, "other": 0}


def sweep_track(rom, region_start, region_end, key, tno, stats, targets,
                wave_addrs):
    """Linear parse of one track's stream region [region_start,
    region_end) with the engine's running-status semantics. Counts every
    command position; validates every 4-byte address operand (targets
    must be inside the owning song's stream region; xwave operands must
    be programmable-wave starts - checked by the caller's wave set).
    Stops at FINE (allowing <= 3 bytes of `.align 2` padding after it)
    or at the exact region end. A command whose operands cross the region
    boundary, a running-status byte with no running command, or a gap
    larger than the align bound are hard failures."""
    pos = region_start
    rs = 0
    while True:
        if pos >= region_end:
            return  # stream consumed exactly at the region end
        b = _rd8(rom, pos)
        if b < 0x80:
            if rs == 0:
                raise WalkError(f"{key} track {tno}: running-status byte "
                                f"0x{b:02x} at {pos:#x} with no running command")
            cmd = rs
        else:
            cmd = b
            pos += 1
            if cmd >= 0xBD:
                rs = cmd
        if cmd <= 0xB0:                       # wait: 0 operands
            stats["wait"] += 1
            continue
        if cmd >= 0xCF:                       # note: 0-3 operands, each < 0x80
            stats["note"] += 1
            n = 0
            while n < 3 and pos < region_end and _rd8(rom, pos) < 0x80:
                pos += 1
                n += 1
            continue
        # 0xB1..0xCE command events
        if cmd in FINE_CMDS:
            stats["fine"] += 1
            slack = region_end - pos
            if slack > 3:
                raise WalkError(f"{key} track {tno}: {slack} bytes after FINE "
                                f"exceed the .align 2 pad bound")
            return
        if cmd == 0xB2:                       # GOTO: 4-byte target
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: GOTO target {target:#x} "
                                f"outside stream region "
                                f"[{region_start:#x}, {region_end:#x})")
            stats["goto"] += 1
            targets.add(target)
            continue
        if cmd == 0xB3:                       # PATT: 4-byte target
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: PATT target {target:#x} "
                                f"outside stream region")
            stats["patt"] += 1
            targets.add(target)
            continue
        if cmd == 0xB4:                       # PEND: 0 operands
            stats["pend"] += 1
            continue
        if cmd == 0xB5:                       # REPT: count + 4-byte target
            count = _rd8(rom, pos)
            pos += 1
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: REPT target {target:#x} "
                                f"outside stream region (count 0x{count:02x})")
            stats["rept"] += 1
            targets.add(target)
            continue
        if cmd == 0xB9:                       # MEMACC: op + index + data
            op = _rd8(rom, pos)
            pos += 1
            if pos >= region_end:
                raise WalkError(f"{key} track {tno}: MEMACC index past region end")
            pos += 1
            if pos >= region_end:
                raise WalkError(f"{key} track {tno}: MEMACC data past region end")
            pos += 1
            stats["memacc"] += 1
            if op in MEMACC_JUMP_OPS:         # + 4-byte target
                target = _rd32(rom, pos)
                pos += 4
                if not (region_start <= target < region_end):
                    raise WalkError(f"{key} track {tno}: MEMACC jump target "
                                    f"{target:#x} outside stream region (op "
                                    f"{op})")
                stats["memacc_jump"] += 1
                targets.add(target)
            continue
        if cmd == 0xCD:                       # XCMD: sub + per-sub operands
            sub = _rd8(rom, pos)
            pos += 1
            if sub not in XCMD_SUB_WIDTH:
                raise WalkError(f"{key} track {tno}: XCMD sub 0x{sub:02x} out "
                                f"of the 14-entry gXcmdTable bounds")
            stats["xcmd"] += 1
            stats["xcmd_sub"][sub] = stats["xcmd_sub"].get(sub, 0) + 1
            if sub == XCMD_WAVE_SUB:          # xwave: 4-byte wave address
                wave = _rd32(rom, pos)
                if wave not in wave_addrs:
                    raise WalkError(f"{key} track {tno}: xwave target {wave:#x} "
                                    f"is not a programmable-wave start")
                stats["xwave"] += 1
                targets.add(wave)
            pos += XCMD_SUB_WIDTH[sub]
            continue
        if cmd == 0xCE:                       # ENDTIE: optional key byte
            stats["endtie"] += 1
            if pos < region_end and _rd8(rom, pos) < 0x80:
                pos += 1
            continue
        if cmd in ONE_BYTE_CMDS:
            pos += 1
            stats["onebyte"] += 1
            continue
        if cmd == 0xCC:                       # PORT: 2 bytes on native
            pos += 2
            stats["port"] += 1
            continue
        raise WalkError(f"{key} track {tno}: unhandled event 0x{cmd:02x} "
                        f"at {pos - 1:#x}")


def walk_track(rom, region_start, region_end, key, tno, stats, targets,
               wave_addrs):
    """Control-flow walk of one track, mirroring the GBA player: running
    status, pattern stack (PATT pushes the return address, PEND pops),
    FINE ends the track, and a jump whose (target, pattern stack) state
    was already executed ends the walk - the player loops forever, the
    walker stops exactly as the engine would. Every reachable target
    must be interior; the walk must never run off the stream region."""
    pos = region_start
    rs = 0
    stack = []
    seen = {(region_start, ())}
    while True:
        if pos < region_start or pos >= region_end:
            raise WalkError(f"{key} track {tno}: walk ran off the stream "
                            f"region at {pos:#x}")
        b = _rd8(rom, pos)
        if b < 0x80:
            if rs == 0:
                raise WalkError(f"{key} track {tno}: running-status byte "
                                f"0x{b:02x} at {pos:#x} with no running command")
            cmd = rs
        else:
            cmd = b
            pos += 1
            if cmd >= 0xBD:
                rs = cmd
        if cmd <= 0xB0:
            continue
        if cmd >= 0xCF:
            n = 0
            while n < 3 and pos < region_end and _rd8(rom, pos) < 0x80:
                pos += 1
                n += 1
            continue
        if cmd in FINE_CMDS:
            return
        if cmd == 0xB2:
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: GOTO target {target:#x} "
                                f"outside stream region")
            state = (target, tuple(stack))
            if state in seen:
                return  # loop within the slice, like the GBA player
            seen.add(state)
            pos = target
            continue
        if cmd == 0xB3:
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: PATT target {target:#x} "
                                f"outside stream region")
            if len(stack) >= 3:
                return  # patternLevel >= 3: the player FINEs the track
            stack.append(pos)
            state = (target, tuple(stack))
            if state in seen:
                return
            seen.add(state)
            pos = target
            continue
        if cmd == 0xB4:
            if stack:
                pos = stack.pop()
            continue
        if cmd == 0xB5:
            count = _rd8(rom, pos)
            pos += 1
            target = _rd32(rom, pos)
            pos += 4
            if not (region_start <= target < region_end):
                raise WalkError(f"{key} track {tno}: REPT target {target:#x} "
                                f"outside stream region")
            if count == 0:
                # "repeat 0 times" == loop forever
                state = (target, tuple(stack))
                if state in seen:
                    return
                seen.add(state)
                pos = target
            # A counted REPT falls through after the count is exhausted;
            # the walk follows the target (the repeating path). No REPT
            # exists in today's graphs (pinned 0); any occurrence re-runs
            # this gate.
            continue
        if cmd == 0xB9:
            op = _rd8(rom, pos)
            pos += 3
            if op in MEMACC_JUMP_OPS:
                target = _rd32(rom, pos)
                pos += 4
                if not (region_start <= target < region_end):
                    raise WalkError(f"{key} track {tno}: MEMACC jump target "
                                    f"{target:#x} outside stream region (op "
                                    f"{op})")
                state = (target, tuple(stack))
                if state in seen:
                    return
                seen.add(state)
                pos = target
            continue
        if cmd == 0xCD:
            sub = _rd8(rom, pos)
            pos += 1
            if sub not in XCMD_SUB_WIDTH:
                raise WalkError(f"{key} track {tno}: XCMD sub 0x{sub:02x} out "
                                f"of the 14-entry gXcmdTable bounds")
            if sub == XCMD_WAVE_SUB:
                wave = _rd32(rom, pos)
                if wave not in wave_addrs:
                    raise WalkError(f"{key} track {tno}: xwave target "
                                    f"{wave:#x} is not a programmable-wave start")
            pos += XCMD_SUB_WIDTH[sub]
            if sub in XCMD_FINE_SUBS:
                return  # ply_xxx -> gMPlayJumpTable[0] (fine): track ends
            continue
        if cmd == 0xCE:
            if pos < region_end and _rd8(rom, pos) < 0x80:
                pos += 1
            continue
        if cmd in ONE_BYTE_CMDS:
            pos += 1
            continue
        if cmd == 0xCC:
            pos += 2
            continue
        raise WalkError(f"{key} track {tno}: unhandled event 0x{cmd:02x} "
                        f"at {pos - 1:#x}")


def validate_song(rom, key, slice_start, slice_end, header_addr, parts,
                  tone, vg_addrs, wave_addrs):
    """Header validation + sweep and control-flow walk of every track.
    Returns (sweep_stats, walk_stats, targets) for the song. Any
    violation raises WalkError naming the song."""
    track_count = len(parts)
    if not (slice_start <= header_addr < slice_end):
        raise WalkError(f"{key}: header {header_addr:#x} outside slice "
                        f"[{slice_start:#x}, {slice_end:#x})")
    if 8 + 4 * track_count != slice_end - header_addr:
        raise WalkError(f"{key}: header size 8+4*{track_count} != "
                        f"{slice_end - header_addr} (slice "
                        f"[{slice_start:#x}, {slice_end:#x}))")
    if not (0 <= track_count <= 10):
        raise WalkError(f"{key}: track count {track_count} outside 0..10")
    if tone not in vg_addrs:
        raise WalkError(f"{key}: tone {tone:#x} is not a voicegroup/cry label")
    last = -1
    for i, part in enumerate(parts):
        if not (slice_start <= part < header_addr):
            raise WalkError(f"{key}: part {i} {part:#x} outside stream region "
                            f"[{slice_start:#x}, {header_addr})")
        if part <= last:
            raise WalkError(f"{key}: parts not strictly increasing "
                            f"({last:#x} -> {part:#x})")
        last = part
    if track_count > 0 and parts[0] != slice_start:
        raise WalkError(f"{key}: first part {parts[0]:#x} != slice start "
                        f"{slice_start:#x}")

    sweep = _new_stats()
    walk = _new_stats()
    targets = set()
    for i, part in enumerate(parts):
        region_end = header_addr if i == len(parts) - 1 else parts[i + 1]
        sweep_track(rom, part, region_end, key, i, sweep, targets,
                    wave_addrs)
        walk_track(rom, part, region_end, key, i, walk, targets,
                   wave_addrs)
    return sweep, walk, targets
