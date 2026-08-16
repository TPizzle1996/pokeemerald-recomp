#ifndef GEN3_RESOURCES_TOML_H
#define GEN3_RESOURCES_TOML_H

#include <stdbool.h>
#include <stddef.h>

#include "gen3/resources/util.h"

/* Minimal deterministic TOML subset covering the two inputs this tool reads:
 *   - top-level scalars (string / integer / boolean), and
 *   - arrays of tables (`[[name]]`).
 *
 * Everything else in the TOML spec (inline tables, arrays of scalars, dotted
 * keys, multi-line strings, hex/octal integers) is rejected with a diagnostic,
 * so a binding file that drifts outside the supported surface fails loudly
 * instead of being silently misread. */
enum Gen3TomlKind
{
    GEN3_TOML_STRING,
    GEN3_TOML_INTEGER,
    GEN3_TOML_BOOLEAN,
    GEN3_TOML_TABLE,      /* [name] — a nested map */
    GEN3_TOML_ARRAY,      /* [[name]] — ordered list of tables */
    GEN3_TOML_ARRAY_ITEM, /* one element inside a [[name]] */
};

struct Gen3TomlValue;
struct Gen3TomlEntry
{
    char *key;
    struct Gen3TomlValue *value;
};
struct Gen3TomlMap
{
    struct Gen3TomlEntry *entries;
    size_t count;
};
struct Gen3TomlValue
{
    enum Gen3TomlKind kind;
    char *string;                       /* GEN3_TOML_STRING (owned) */
    long long integer;                  /* GEN3_TOML_INTEGER */
    bool boolean;                       /* GEN3_TOML_BOOLEAN */
    struct Gen3TomlMap map;             /* GEN3_TOML_TABLE / ARRAY_ITEM */
    struct Gen3TomlValue **items;       /* GEN3_TOML_ARRAY (owned) */
    size_t itemCount;
};
struct Gen3TomlDocument
{
    struct Gen3TomlMap root;
};

bool Gen3Toml_Parse(const char *text, size_t textLength,
                    struct Gen3TomlDocument *outDocument,
                    char *errbuf, size_t errbufSize);
void Gen3Toml_Destroy(struct Gen3TomlDocument *document);

/* Lookup helpers. */
const struct Gen3TomlEntry *Gen3Toml_FindEntry(const struct Gen3TomlMap *map, const char *key);
bool Gen3Toml_GetString(const struct Gen3TomlMap *map, const char *key, const char **out);
bool Gen3Toml_GetInteger(const struct Gen3TomlMap *map, const char *key, long long *out);
bool Gen3Toml_GetBool(const struct Gen3TomlMap *map, const char *key, bool *out);
size_t Gen3Toml_GetArrayCount(const struct Gen3TomlMap *map, const char *key);
const struct Gen3TomlMap *Gen3Toml_GetArrayItem(const struct Gen3TomlMap *map,
                                                const char *key, size_t index);

/* Deterministic TOML emitter used for the extraction manifest. Keys and string
 * values are escaped; ordering is entirely caller-controlled (the manifest
 * core emits records sorted bytewise by canonical id). */
bool Gen3TomlWrite_String(struct Gen3Buffer *out, const char *key, const char *value);
bool Gen3TomlWrite_Integer(struct Gen3Buffer *out, const char *key, long long value);
bool Gen3TomlWrite_OpenArrayTable(struct Gen3Buffer *out, const char *key);

#endif
