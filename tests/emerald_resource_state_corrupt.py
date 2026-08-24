#!/usr/bin/env python3
"""R10-G TEST 8: mutate a v5 state's resource sidecar with CRC repair.

Mirrors the container layout (NST1, formatVersion 5, header + section
headers + payload, CRC32 = zlib.crc32 over each section / the payload) and
applies one targeted corruption to the FIRST sidecar record, recomputing the
section and payload CRCs so the loader's PARSER (not the checksum) is what
must reject the state. Each kind pins one validation rule from R10 §B.

Kinds:
  corrupt-key        flip key bytes (well-formed but unknown key -> resolve
                     failure, TEST 7b)
  bad-tag            sectionTag = 99
  oob-offset         fieldOffset = 0xFFFFFFFC
  bad-key            key = all zero
  bad-role           representationRole = 99
  bad-schema         schema = 0xFFFFFFFF (identity mismatch at resolve)
  oob-resource-offset rangeOffset = 0xFFFFFFF0
  oversized-count    record count = 5000 (inconsistent with section size)
  duplicate-fields   record[1].fieldOffset = record[0].fieldOffset
  bad-reserved       record[0].reserved = 1
  truncated-sidecar  section size field = fileSize + 1
  v4-version         header formatVersion = 4 (R10 §I policy; the version
                     field sits in the header, outside the payload CRC)
  unsupported-version header formatVersion = 6
"""
import struct
import sys
import zlib

MAGIC = 0x4E535431
SIDECAR_TAG = 14
RECORD_SIZE = 64


def load(path):
    with open(path, "rb") as f:
        return bytearray(f.read())


def le32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def put32(buf, off, value):
    struct.pack_into("<I", buf, off, value)


def main():
    if len(sys.argv) != 3:
        print("usage: emerald_resource_state_corrupt.py <state> <kind>")
        return 2
    path, kind = sys.argv[1], sys.argv[2]
    buf = load(path)
    if le32(buf, 0) != MAGIC:
        print("not a native state")
        return 1
    version = le32(buf, 4)
    if version != 5:
        print(f"state version {version} != 5")
        return 1
    header_size = le32(buf, 8)
    section_count = le32(buf, 16)

    sidecar_index = None
    for i in range(section_count):
        off = header_size + i * 12
        if le32(buf, off) == SIDECAR_TAG:
            sidecar_index = i
            break
    if sidecar_index is None:
        print("no sidecar section")
        return 1

    sidecar_header = header_size + sidecar_index * 12
    sidecar_size = le32(buf, sidecar_header + 4)
    # Payload offset of the sidecar section.
    payload_base = header_size + section_count * 12
    offset = 0
    for i in range(sidecar_index):
        offset += le32(buf, header_size + i * 12 + 4)
    sidecar_payload = payload_base + offset

    if kind == "truncated-sidecar":
        put32(buf, sidecar_header + 4, len(buf) + 1)
    elif kind == "v4-version":
        put32(buf, 4, 4)
    elif kind == "unsupported-version":
        put32(buf, 4, 6)
    elif kind == "oversized-count":
        put32(buf, sidecar_payload, 5000)
    elif kind == "bad-sidecar-size":
        # Size field not 4 + 64*k: the parser's structural check refuses
        # before any record is read.
        put32(buf, sidecar_header + 4, sidecar_size - 4)
    elif kind == "raw-crc":
        # Flip one payload byte WITHOUT repairing: the container CRC check
        # must refuse (the payload/section checksum path).
        buf[payload_base] ^= 0xFF
    else:
        if sidecar_size < 4 + RECORD_SIZE:
            print("sidecar too small to corrupt")
            return 1
        record = sidecar_payload + 4
        if kind == "corrupt-key":
            for j in range(8, 40):
                buf[record + j] ^= 0xA5
        elif kind == "bad-tag":
            put32(buf, record, 99)
        elif kind == "oob-offset":
            put32(buf, record + 4, 0xFFFFFFFC)
        elif kind == "bad-key":
            for j in range(8, 40):
                buf[record + j] = 0
        elif kind == "bad-role":
            put32(buf, record + 48, 99)
        elif kind == "bad-schema":
            put32(buf, record + 44, 0xFFFFFFFF)
        elif kind == "bad-type":
            put32(buf, record + 40, 99)
        elif kind == "oob-resource-offset":
            put32(buf, record + 52, 0xFFFFFFF0)
        elif kind == "duplicate-fields":
            put32(buf, record + RECORD_SIZE + 4, le32(buf, record + 4))
        elif kind == "bad-reserved":
            put32(buf, record + 56, 1)
        else:
            print(f"unknown kind {kind}")
            return 2

    # Repair the sidecar section CRC and the payload CRC so the container's
    # structural checks pass and the semantic validation must do the work.
    # raw-crc deliberately SKIPS the repair: the un-repaired checksum is the
    # mutation under test.
    if kind != "raw-crc":
        put32(buf, sidecar_header + 8,
              zlib.crc32(buf[sidecar_payload:sidecar_payload + sidecar_size]))
        put32(buf, 24, zlib.crc32(buf[header_size:]))
    with open(path, "wb") as f:
        f.write(buf)
    print(f"corrupted {kind}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
