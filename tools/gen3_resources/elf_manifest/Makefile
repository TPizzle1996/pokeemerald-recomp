# gen3-elf-manifest - Stage R1A extraction-metadata generator.
#
# Build isolation: only -std=c99 -Wall -Wextra -Werror plus the two include
# roots for the shared Gen3 core (sha256.c, resource_id.c). No Emerald defines,
# no global.h/gba/SDL. The tool links the exact M0/M1 key-derivation code so the
# manifest keys are recomputed with the same algorithm (R1A guardrail 14).

CC ?= cc
ELF_MANIFEST_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT := $(ELF_MANIFEST_DIR)../../..
CFLAGS ?= -std=c99 -Wall -Wextra -Werror \
    -iquote $(ROOT)/include \
    -iquote $(ROOT)/src/gen3/resources

CORE := $(ROOT)/src/gen3/resources/sha256.c $(ROOT)/src/gen3/resources/resource_id.c \
        $(ROOT)/src/gen3/resources/util.c $(ROOT)/src/gen3/resources/lz77.c \
        $(ROOT)/src/gen3/resources/sha1.c $(ROOT)/src/gen3/resources/toml.c
TOOL := elf_reader.c manifest.c

all: gen3-elf-manifest fixture_build

gen3-elf-manifest: main.c $(TOOL) $(CORE)
	$(CC) $(CFLAGS) main.c $(TOOL) $(CORE) -o $@

fixture_build: fixture_build.c $(CORE)
	$(CC) $(CFLAGS) fixture_build.c $(CORE) -o $@

clean:
	rm -f gen3-elf-manifest fixture_build

.PHONY: all clean
