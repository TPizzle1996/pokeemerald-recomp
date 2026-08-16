# gen-trainer-family - Stage R7A trainer-front family generator.
#
# Build isolation (R7A §24): only -std=c99 -Wall -Wextra -Werror plus the two
# include roots for the shared Gen3 core. No Emerald defines, no
# global.h/gba/SDL. The tool links the exact M0/M1 key-derivation code so the
# ownership keys are recomputed with the same algorithm, and the strict GBA
# LZ77 decoder so every committed artifact's decoded size/hash is verified at
# generation time.

CC ?= cc
FAMILY_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ROOT := $(FAMILY_DIR)../../..
CFLAGS ?= -std=c99 -Wall -Wextra -Werror \
    -iquote $(ROOT)/include \
    -iquote $(ROOT)/src/gen3/resources

CORE := $(ROOT)/src/gen3/resources/sha256.c $(ROOT)/src/gen3/resources/resource_id.c \
        $(ROOT)/src/gen3/resources/util.c $(ROOT)/src/gen3/resources/lz77.c \
        $(ROOT)/src/gen3/resources/toml.c

all: gen-trainer-family

gen-trainer-family: gen_trainer_family.c $(CORE)
	$(CC) $(CFLAGS) gen_trainer_family.c $(CORE) -o $@

clean:
	rm -f gen-trainer-family

.PHONY: all clean
