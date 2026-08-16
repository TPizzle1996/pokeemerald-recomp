#!/usr/bin/env python3
"""Independent reference for the R2 fixed synthetic-pack digest vectors.

This script is NOT part of the C test runner. It is the maintainer reference
used to derive the hardcoded EXPECTED_* vectors in resource_pack_test.c from an
independent implementation, so the C writer/reader is verified against the
documented format rather than against itself.

The fixed fixture is exactly the two-resource Brendan synthetic pack:

    emerald:trainer/brendan/battle/front/normal-palette   (entry 0, bytewise name order)
    emerald:trainer/brendan/battle/front/sheet            (entry 1)

Run:  python3 tests/gen3_resources/resource_pack_vectors.py
"""
import hashlib
import struct

# ---------------------------------------------------------------------------
# Format constants (must match include/gen3/resources/resource_pack.h)
# ---------------------------------------------------------------------------
HEADER_SIZE = 448
ENTRY_SIZE = 160
FORMAT_VERSION = 1
ENDIAN_TAG = 0x01020304
MAGIC = b"G3RPACK\x00"

DOMAIN_KEY = b"gen3-resource-id-v1\x00"
DOMAIN_LOGICAL = b"gen3-base-resource-pack-v1\x00"
DOMAIN_PROVIDER = b"gen3-provider-content-v1\x00"

TYPE_TILE_GRAPHICS = 2
TYPE_PALETTE = 3
REPRESENTATION_DECODED = 1
ENCODING_GBA_LZ77 = 1
FLAG_REQUIRED_FOR_BASE = 0x00000001


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def name_key(name: str) -> bytes:
    return sha256(DOMAIN_KEY + name.encode("ascii"))


# ---------------------------------------------------------------------------
# Fixed fixture logical records
# ---------------------------------------------------------------------------
SHEET_NAME = "emerald:trainer/brendan/battle/front/sheet"
PALETTE_NAME = "emerald:trainer/brendan/battle/front/normal-palette"

SHEET_PAYLOAD = bytes((i * 7 + (i >> 3)) & 0xFF for i in range(2048))
PALETTE_PAYLOAD = bytes((i * 3 + 1) & 0xFF for i in range(32))

# Real Brendan source-artifact SHA-256 (R1A manifest, provenance only).
SHEET_SOURCE_SHA256 = bytes.fromhex(
    "c5fca4e037ab22c2c24bce1f0cf0c2b35679c3a1cf64b6e64dedbd26b3cebfde")
PALETTE_SOURCE_SHA256 = bytes.fromhex(
    "7cb7654625eabaa37dbcc96f116ca3ac64b6a4424ec6f8bf1117e9ddcaabd18f")

# Fixture ROM identity from the R1A synthetic fixture.
ROM_SHA1 = bytes.fromhex("092ee293c6e4381074682375249561bbc41bec44")
ROM_SHA256 = bytes.fromhex("e4c8f3693d840fb233dfa31c1cd2b4d21aa2dde67359e4aad4b064a4b1b33bca")

CATALOG_SHA256 = sha256(b"gen3-synthetic-catalog-v1")
MANIFEST_SHA256 = sha256(b"gen3-synthetic-extraction-manifest-v1")

GAME_ID = b"emerald" + b"\x00" * (16 - len(b"emerald"))

records = [  # sorted bytewise by canonical name later; here explicit
    dict(name=PALETTE_NAME, key=name_key(PALETTE_NAME), type_code=TYPE_PALETTE,
         schema=1, flags=FLAG_REQUIRED_FOR_BASE, payload=PALETTE_PAYLOAD,
         rom_offset=0x310000, enc_size=40, source_sha256=PALETTE_SOURCE_SHA256),
    dict(name=SHEET_NAME, key=name_key(SHEET_NAME), type_code=TYPE_TILE_GRAPHICS,
         schema=1, flags=FLAG_REQUIRED_FOR_BASE, payload=SHEET_PAYLOAD,
         rom_offset=0x300000, enc_size=804, source_sha256=SHEET_SOURCE_SHA256),
]
records.sort(key=lambda r: r["name"].encode("ascii"))


def align16(v: int) -> int:
    return (v + 15) & ~15


# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
toc_offset = HEADER_SIZE
toc_size = ENTRY_SIZE * len(records)
names_offset = toc_offset + toc_size
names_blob = b"".join(r["name"].encode("ascii") for r in records)
names_size = len(names_blob)
payload_offset = align16(names_offset + names_size)
# Payload slots are 16-aligned in name order.
slot_sizes = [align16(len(r["payload"])) for r in records]
payload_size = sum(slot_sizes)
file_size = payload_offset + payload_size
assert file_size < 0x80000000

# Assign payload offsets in name order.
run = payload_offset
for r, slot in zip(records, slot_sizes):
    r["payload_offset"] = run
    run += slot
assert run == payload_offset + payload_size

# ---------------------------------------------------------------------------
# TOC
# ---------------------------------------------------------------------------
toc = bytearray()
for r in records:
    assert len(r["name"]) <= 0xFFFF and len(r["name"]) <= 255
    entry = bytearray(ENTRY_SIZE)
    entry[0:32] = r["key"]
    struct.pack_into("<I", entry, 32, names_blob.index(r["name"].encode("ascii")))
    struct.pack_into("<H", entry, 36, len(r["name"]))
    struct.pack_into("<H", entry, 38, r["type_code"])
    struct.pack_into("<I", entry, 40, r["schema"])
    struct.pack_into("<I", entry, 44, r["flags"])
    struct.pack_into("<I", entry, 48, REPRESENTATION_DECODED)
    struct.pack_into("<I", entry, 52, ENCODING_GBA_LZ77)
    struct.pack_into("<Q", entry, 56, r["payload_offset"])
    struct.pack_into("<Q", entry, 64, len(r["payload"]))
    struct.pack_into("<Q", entry, 72, r["rom_offset"])
    struct.pack_into("<Q", entry, 80, r["enc_size"])
    entry[88:120] = sha256(r["payload"])
    entry[120:152] = r["source_sha256"]
    toc += entry

toc_sha256 = sha256(bytes(toc))
names_sha256 = sha256(names_blob)
payload_blob = b"".join(r["payload"] for r in records)
payload_sha256 = sha256(payload_blob)

# ---------------------------------------------------------------------------
# Header
# ---------------------------------------------------------------------------
header = bytearray(HEADER_SIZE)
header[0:8] = MAGIC
struct.pack_into("<I", header, 8, HEADER_SIZE)
struct.pack_into("<I", header, 12, FORMAT_VERSION)
struct.pack_into("<I", header, 16, ENDIAN_TAG)
struct.pack_into("<I", header, 20, 0)            # flags
struct.pack_into("<I", header, 24, 1)            # base pack version
struct.pack_into("<I", header, 28, len(records)) # entry count
struct.pack_into("<I", header, 32, ENTRY_SIZE)
struct.pack_into("<I", header, 36, 1)            # API major
struct.pack_into("<I", header, 40, 0)            # API minor
struct.pack_into("<I", header, 44, 0)            # API patch
struct.pack_into("<I", header, 48, 1)            # catalog version
struct.pack_into("<I", header, 52, 1)            # extraction-manifest version
struct.pack_into("<I", header, 56, 1)            # canonical representation version
struct.pack_into("<I", header, 60, 0)            # reserved
struct.pack_into("<Q", header, 64, file_size)
struct.pack_into("<Q", header, 72, toc_offset)
struct.pack_into("<Q", header, 80, toc_size)
struct.pack_into("<Q", header, 88, names_offset)
struct.pack_into("<Q", header, 96, names_size)
struct.pack_into("<Q", header, 104, payload_offset)
struct.pack_into("<Q", header, 112, payload_size)
struct.pack_into("<Q", header, 120, 16 * 1024 * 1024)  # source ROM size
header[128:148] = ROM_SHA1
header[148:180] = ROM_SHA256
header[180:184] = b"BPEE"
header[184:186] = b"01"
header[186] = 0  # software revision
header[187] = 0  # reserved
header[188:204] = GAME_ID
header[204:236] = CATALOG_SHA256
header[236:268] = MANIFEST_SHA256
header[268:300] = toc_sha256
header[300:332] = names_sha256
header[332:364] = payload_sha256

# Logical pack-content digest preimage (documented in resource_content_digest.h):
logical_preimage = b"".join([
    DOMAIN_LOGICAL,
    struct.pack("<I", FORMAT_VERSION),
    struct.pack("<I", 1),   # API major
    struct.pack("<I", 0),   # API minor
    struct.pack("<I", 0),   # API patch
    struct.pack("<I", 1),   # base pack version
    struct.pack("<I", 1),   # catalog version
    struct.pack("<I", 1),   # extraction-manifest version
    struct.pack("<I", 1),   # canonical representation version
    struct.pack("<Q", 16 * 1024 * 1024),  # source ROM size
    b"BPEE", b"01",
    struct.pack("<BB", 0, 0),  # software revision, reserved
    ROM_SHA1, ROM_SHA256,
    CATALOG_SHA256, MANIFEST_SHA256,
    toc_sha256, names_sha256, payload_sha256,
])
logical_sha256 = sha256(logical_preimage)
header[364:396] = logical_sha256

# Header hash: over the full 448-byte header with the 32-byte header-hash field
# at offset 396 zeroed. The field is still zero here, so hashing the whole
# header (reserved bytes at 428+ are also zero) is exactly that preimage.
header_sha256 = sha256(bytes(header))
header[396:428] = header_sha256

# ---------------------------------------------------------------------------
# Provider-content digest (records sorted bytewise by 32-byte key)
# ---------------------------------------------------------------------------
provider_records = sorted(records, key=lambda r: r["key"])
provider_preimage = b"".join([
    DOMAIN_PROVIDER,
    struct.pack("<Q", len(provider_records)),
] + [
    r["key"]
    + struct.pack("<I", r["type_code"])
    + struct.pack("<I", r["schema"])
    + struct.pack("<Q", len(r["payload"]))
    + sha256(r["payload"])
    for r in provider_records
])
provider_sha256 = sha256(provider_preimage)

pack = bytes(header) + bytes(toc) + names_blob \
    + b"\x00" * (payload_offset - (names_offset + names_size)) \
    + b"".join(r["payload"] + b"\x00" * (align16(len(r["payload"])) - len(r["payload"]))
               for r in records)
assert len(pack) == file_size


def hx(b: bytes) -> str:
    return b.hex()


print("== fixed synthetic pack v1 vectors ==")
print(f"file_size                 {file_size}")
print(f"toc_offset                {toc_offset}")
print(f"names_offset              {names_offset}")
print(f"names_size                {names_size}")
print(f"payload_offset            {payload_offset}")
print(f"payload_size              {payload_size}")
print(f"palette_key               {hx(records[0]['key'])}")
print(f"sheet_key                 {hx(records[1]['key'])}")
print(f"palette_payload_sha256    {hx(sha256(PALETTE_PAYLOAD))}")
print(f"sheet_payload_sha256      {hx(sha256(SHEET_PAYLOAD))}")
print(f"payload_section_sha256    {hx(payload_sha256)}")
print(f"toc_sha256                {hx(toc_sha256)}")
print(f"names_sha256              {hx(names_sha256)}")
print(f"logical_pack_sha256       {hx(logical_sha256)}")
print(f"provider_content_sha256   {hx(provider_sha256)}")
print(f"header_sha256             {hx(header_sha256)}")
print(f"catalog_sha256            {hx(CATALOG_SHA256)}")
print(f"manifest_sha256           {hx(MANIFEST_SHA256)}")
print(f"palette_source_sha256     {hx(PALETTE_SOURCE_SHA256)}")
print(f"sheet_source_sha256       {hx(SHEET_SOURCE_SHA256)}")
print()
print("== one-byte payload-change sensitivity (flip palette payload byte 0) ==")
altered_palette = bytes([PALETTE_PAYLOAD[0] ^ 0x01]) + PALETTE_PAYLOAD[1:]
alt_payload_blob = altered_palette + SHEET_PAYLOAD
print(f"altered payload_section   {hx(sha256(alt_payload_blob))}")
