# gen3-pack-build - deterministic production .rpack builder (R11-L).
#
# Thin driver over the existing multi-manifest importer; links the exact
# importer + pack codec chain (no new import/write logic).

CC ?= cc
FAMILY_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT := $(FAMILY_DIR)../../..
CFLAGS ?= -std=c99 -Wall -Wextra -Werror \
    -iquote $(ROOT)/include \
    -iquote $(ROOT)/src/gen3/resources \
    -iquote $(ROOT)/src/emerald/resources

CORE := $(ROOT)/src/gen3/resources/sha256.c $(ROOT)/src/gen3/resources/sha1.c \
        $(ROOT)/src/gen3/resources/resource_lz.c \
        $(ROOT)/src/gen3/resources/resource_id.c \
        $(ROOT)/src/gen3/resources/resource_core.c \
        $(ROOT)/src/gen3/resources/resource_resolver.c \
        $(ROOT)/src/gen3/resources/resource_content_digest.c \
        $(ROOT)/src/gen3/resources/resource_pack.c \
        $(ROOT)/src/gen3/resources/resource_pack_writer.c \
        $(ROOT)/src/gen3/resources/resource_pack_provider.c \
        $(ROOT)/src/gen3/resources/toml.c \
        $(ROOT)/src/gen3/resources/util.c \
        $(ROOT)/src/gen3/resources/lz77.c \
        $(ROOT)/src/emerald/resources/emerald_rom_profile.c \
        $(ROOT)/src/emerald/resources/emerald_resource_import.c

all: gen3-pack-build

gen3-pack-build: gen3_pack_build.c $(CORE)
	$(CC) $(CFLAGS) gen3_pack_build.c $(CORE) -o $@

clean:
	rm -f gen3-pack-build

.PHONY: all clean
