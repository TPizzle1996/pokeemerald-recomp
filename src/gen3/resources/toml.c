#include "gen3/resources/toml.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- error reporting ---------------------------------------------------- */

typedef struct
{
    const char *text;
    size_t length;
    size_t pos;
    size_t line;
    struct Gen3TomlDocument *document;
    struct Gen3TomlMap *currentMap; /* innermost open header */
    char *errbuf;
    size_t errbufSize;
    bool failed;
} Parser;

static void Fail(Parser *parser, size_t line, const char *message, const char *detail)
{
    int needed;
    if (parser->failed || parser->errbuf == NULL || parser->errbufSize == 0)
        return;
    parser->failed = true;
    if (detail != NULL)
        needed = snprintf(parser->errbuf, parser->errbufSize, "%zu: %s: %s",
                          line, message, detail);
    else
        needed = snprintf(parser->errbuf, parser->errbufSize, "%zu: %s", line, message);
    if (needed < 0 || (size_t)needed >= parser->errbufSize)
        parser->errbuf[parser->errbufSize - 1u] = '\0';
}

/* ---- memory helpers ----------------------------------------------------- */

static struct Gen3TomlValue *ValueCreate(void)
{
    return calloc(1, sizeof(struct Gen3TomlValue));
}

static struct Gen3TomlEntry *MapAppend(Parser *parser, struct Gen3TomlMap *map,
                                       const char *key, size_t keyLength,
                                       struct Gen3TomlValue *value, size_t line)
{
    struct Gen3TomlEntry *resized;
    struct Gen3TomlEntry *entry;
    size_t i;
    for (i = 0; i < map->count; i++)
    {
        if (strlen(map->entries[i].key) == keyLength
         && memcmp(map->entries[i].key, key, keyLength) == 0)
        {
            Fail(parser, line, "duplicate key", key);
            return NULL;
        }
    }
    resized = realloc(map->entries, (map->count + 1u) * sizeof(*map->entries));
    if (resized == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return NULL;
    }
    map->entries = resized;
    entry = &map->entries[map->count++];
    entry->key = malloc(keyLength + 1u);
    if (entry->key == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return NULL;
    }
    memcpy(entry->key, key, keyLength);
    entry->key[keyLength] = '\0';
    entry->value = value;
    return entry;
}

static bool ArrayAppend(Parser *parser, struct Gen3TomlValue *array,
                        struct Gen3TomlValue *item, size_t line)
{
    struct Gen3TomlValue **resized;
    resized = realloc(array->items, (array->itemCount + 1u) * sizeof(*array->items));
    if (resized == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return false;
    }
    array->items = resized;
    array->items[array->itemCount++] = item;
    return true;
}

static struct Gen3TomlValue *MapFindValue(const struct Gen3TomlMap *map, const char *key)
{
    size_t i;
    for (i = 0; i < map->count; i++)
    {
        if (strcmp(map->entries[i].key, key) == 0)
            return map->entries[i].value;
    }
    return NULL;
}

/* ---- token scanning ----------------------------------------------------- */

static bool IsBareKeyChar(char character)
{
    return isalnum((unsigned char)character)
        || character == '_' || character == '-';
}

static const char *SkipWhitespace(const char *cursor, const char *end)
{
    while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
        cursor++;
    return cursor;
}

/* Parses a double-quoted basic string starting at *cursor (which points at the
 * opening quote). Advances *cursor past the closing quote and stores the
 * unescaped content in *outString. */
static bool ParseBasicString(Parser *parser, const char **cursor, const char *end,
                             size_t line, char **outString)
{
    const char *cursorPos = *cursor + 1u;
    struct Gen3Buffer value;
    if (!Gen3Buffer_Init(&value, 16))
    {
        Fail(parser, line, "out of memory", NULL);
        return false;
    }
    while (cursorPos < end)
    {
        char character = *cursorPos++;
        if (character == '"')
        {
            *outString = value.data;
            *cursor = cursorPos;
            return true;
        }
        if (character == '\\')
        {
            char escaped;
            if (cursorPos >= end)
            {
                Fail(parser, line, "unterminated escape sequence", NULL);
                Gen3Buffer_Destroy(&value);
                return false;
            }
            escaped = *cursorPos++;
            switch (escaped)
            {
                case '"': Gen3Buffer_Append(&value, "\"", 1); break;
                case '\\': Gen3Buffer_Append(&value, "\\", 1); break;
                case 'n': Gen3Buffer_Append(&value, "\n", 1); break;
                case 't': Gen3Buffer_Append(&value, "\t", 1); break;
                case 'r': Gen3Buffer_Append(&value, "\r", 1); break;
                default:
                {
                    char detail[32];
                    snprintf(detail, sizeof(detail), "\\%c", escaped);
                    Fail(parser, line, "unsupported escape", detail);
                    Gen3Buffer_Destroy(&value);
                    return false;
                }
            }
        }
        else
        {
            Gen3Buffer_Append(&value, &character, 1);
        }
    }
    Fail(parser, line, "unterminated string", NULL);
    Gen3Buffer_Destroy(&value);
    return false;
}

/* Parses a single-quoted literal string (no escapes) starting at *cursor. */
static bool ParseLiteralString(Parser *parser, const char **cursor, const char *end,
                               size_t line, char **outString)
{
    const char *start = *cursor + 1u;
    const char *cursorPos = start;
    char *copy;
    size_t length;
    while (cursorPos < end && *cursorPos != '\'')
        cursorPos++;
    if (cursorPos >= end)
    {
        Fail(parser, line, "unterminated literal string", NULL);
        return false;
    }
    length = (size_t)(cursorPos - start);
    copy = malloc(length + 1u);
    if (copy == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return false;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    *outString = copy;
    *cursor = cursorPos + 1u;
    return true;
}

/* Parses a value token (the right-hand side of `key = ...`). */
static bool ParseValue(Parser *parser, const char **cursor, const char *end,
                       size_t line, struct Gen3TomlValue **outValue)
{
    const char *cursorPos = SkipWhitespace(*cursor, end);
    struct Gen3TomlValue *value = ValueCreate();
    if (value == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return false;
    }
    if (cursorPos < end && *cursorPos == '"')
    {
        char *stringValue;
        if (!ParseBasicString(parser, &cursorPos, end, line, &stringValue))
        {
            free(value);
            return false;
        }
        value->kind = GEN3_TOML_STRING;
        value->string = stringValue;
    }
    else if (cursorPos < end && *cursorPos == '\'')
    {
        char *stringValue;
        if (!ParseLiteralString(parser, &cursorPos, end, line, &stringValue))
        {
            free(value);
            return false;
        }
        value->kind = GEN3_TOML_STRING;
        value->string = stringValue;
    }
    else if (cursorPos < end && (*cursorPos == '-' || *cursorPos == '+'
             || isdigit((unsigned char)*cursorPos)))
    {
        const char *numberStart = cursorPos;
        char *endNumber;
        long long parsed;
        if (*cursorPos == '-' || *cursorPos == '+')
            cursorPos++;
        if (cursorPos >= end || !isdigit((unsigned char)*cursorPos))
        {
            Fail(parser, line, "malformed integer", NULL);
            free(value);
            return false;
        }
        while (cursorPos < end && isdigit((unsigned char)*cursorPos))
            cursorPos++;
        value->kind = GEN3_TOML_INTEGER;
        errno = 0;
        parsed = strtoll(numberStart, &endNumber, 10);
        if (endNumber != cursorPos || errno == ERANGE)
        {
            Fail(parser, line, "integer out of range", NULL);
            free(value);
            return false;
        }
        value->integer = parsed;
    }
    else if (cursorPos + 4u <= end && memcmp(cursorPos, "true", 4u) == 0)
    {
        value->kind = GEN3_TOML_BOOLEAN;
        value->boolean = true;
        cursorPos += 4u;
    }
    else if (cursorPos + 5u <= end && memcmp(cursorPos, "false", 5u) == 0)
    {
        value->kind = GEN3_TOML_BOOLEAN;
        value->boolean = false;
        cursorPos += 5u;
    }
    else
    {
        Fail(parser, line, "unsupported value (strings, integers, booleans only)", NULL);
        free(value);
        return false;
    }
    *cursor = cursorPos;
    *outValue = value;
    return true;
}

/* Parses a key token (bare or quoted); advances *cursor past it. */
static bool ParseKey(Parser *parser, const char **cursor, const char *end,
                     size_t line, char **outKey)
{
    const char *cursorPos = *cursor;
    const char *keyStart;
    size_t length;
    char *copy;
    if (cursorPos < end && (*cursorPos == '"' || *cursorPos == '\''))
    {
        bool ok = (*cursorPos == '"')
            ? ParseBasicString(parser, &cursorPos, end, line, outKey)
            : ParseLiteralString(parser, &cursorPos, end, line, outKey);
        *cursor = cursorPos;
        return ok;
    }
    keyStart = cursorPos;
    while (cursorPos < end && IsBareKeyChar(*cursorPos))
        cursorPos++;
    if (cursorPos == keyStart)
    {
        Fail(parser, line, "expected a key", NULL);
        return false;
    }
    length = (size_t)(cursorPos - keyStart);
    copy = malloc(length + 1u);
    if (copy == NULL)
    {
        Fail(parser, line, "out of memory", NULL);
        return false;
    }
    memcpy(copy, keyStart, length);
    copy[length] = '\0';
    *outKey = copy;
    *cursor = cursorPos;
    return true;
}

/* ---- line dispatch ------------------------------------------------------ */

static void HandleAssignment(Parser *parser, const char *line, size_t lineLength,
                             size_t lineNo, struct Gen3TomlMap *map)
{
    const char *cursor = line;
    const char *end = line + lineLength;
    char *key = NULL;
    struct Gen3TomlValue *value = NULL;

    if (!ParseKey(parser, &cursor, end, lineNo, &key))
        return;
    cursor = SkipWhitespace(cursor, end);
    if (cursor >= end || *cursor != '=')
    {
        Fail(parser, lineNo, "expected '=' after key", key);
        free(key);
        return;
    }
    cursor++;
    if (!ParseValue(parser, &cursor, end, lineNo, &value))
    {
        free(key);
        return;
    }
    cursor = SkipWhitespace(cursor, end);
    if (cursor < end && *cursor != '#')
    {
        Fail(parser, lineNo, "unexpected content after value", key);
        free(key);
        free(value->string);
        free(value);
        return;
    }
    if (MapAppend(parser, map, key, strlen(key), value, lineNo) == NULL)
    {
        free(value->string);
        free(value);
    }
    free(key);
}

/* Opens a `[name]` or `[[name]]` header. For `[[name]]`, appends a fresh item
 * and returns its map; for `[name]`, returns that table's map. */
static bool OpenTableHeader(Parser *parser, const char *line, size_t lineLength,
                            size_t lineNo, struct Gen3TomlMap **outMap)
{
    const char *cursor = line;
    const char *end = line + lineLength;
    bool isArray = false;
    char *key = NULL;
    struct Gen3TomlValue *existing;
    struct Gen3TomlValue *created;

    if (cursor < end && *cursor == '[')
    {
        if (cursor + 1u < end && cursor[1] == '[')
        {
            isArray = true;
            cursor += 2;
        }
        else
        {
            cursor += 1;
        }
    }
    cursor = SkipWhitespace(cursor, end);
    if (!ParseKey(parser, &cursor, end, lineNo, &key))
        return false;
    cursor = SkipWhitespace(cursor, end);
    if (cursor >= end || *cursor != ']')
    {
        Fail(parser, lineNo, "expected ']' in table header", key);
        free(key);
        return false;
    }
    if (isArray)
    {
        if (cursor + 1u >= end || cursor[1] != ']')
        {
            Fail(parser, lineNo, "expected ']]' in array header", key);
            free(key);
            return false;
        }
        cursor += 2;
    }
    else
    {
        cursor += 1;
    }
    cursor = SkipWhitespace(cursor, end);
    if (cursor < end && *cursor != '#')
    {
        Fail(parser, lineNo, "unexpected content after table header", key);
        free(key);
        return false;
    }

    existing = MapFindValue(&parser->document->root, key);
    if (isArray)
    {
        struct Gen3TomlValue *item;
        if (existing == NULL)
        {
            created = ValueCreate();
            if (created == NULL)
            {
                Fail(parser, lineNo, "out of memory", NULL);
                free(key);
                return false;
            }
            created->kind = GEN3_TOML_ARRAY;
            if (MapAppend(parser, &parser->document->root, key, strlen(key), created, lineNo) == NULL)
            {
                free(created);
                free(key);
                return false;
            }
            existing = created;
        }
        if (existing->kind != GEN3_TOML_ARRAY)
        {
            Fail(parser, lineNo, "key redefined with a different kind", key);
            free(key);
            return false;
        }
        item = ValueCreate();
        if (item == NULL)
        {
            Fail(parser, lineNo, "out of memory", NULL);
            free(key);
            return false;
        }
        item->kind = GEN3_TOML_ARRAY_ITEM;
        if (!ArrayAppend(parser, existing, item, lineNo))
        {
            free(item);
            free(key);
            return false;
        }
        *outMap = &item->map;
    }
    else
    {
        if (existing != NULL)
        {
            Fail(parser, lineNo, "duplicate table header", key);
            free(key);
            return false;
        }
        created = ValueCreate();
        if (created == NULL)
        {
            Fail(parser, lineNo, "out of memory", NULL);
            free(key);
            return false;
        }
        created->kind = GEN3_TOML_TABLE;
        if (MapAppend(parser, &parser->document->root, key, strlen(key), created, lineNo) == NULL)
        {
            free(created);
            free(key);
            return false;
        }
        *outMap = &created->map;
    }
    free(key);
    return true;
}

/* ---- public parser ------------------------------------------------------ */

bool Gen3Toml_Parse(const char *text, size_t textLength,
                    struct Gen3TomlDocument *outDocument,
                    char *errbuf, size_t errbufSize)
{
    Parser parser;
    const char *end;
    if (text == NULL || outDocument == NULL)
    {
        if (errbuf != NULL && errbufSize != 0)
            snprintf(errbuf, errbufSize, "internal: null TOML argument");
        return false;
    }
    memset(outDocument, 0, sizeof(*outDocument));
    parser.text = text;
    parser.length = textLength;
    parser.pos = 0;
    parser.line = 0;
    parser.document = outDocument;
    parser.currentMap = &outDocument->root;
    parser.errbuf = errbuf;
    parser.errbufSize = errbufSize;
    parser.failed = false;
    end = text + textLength;

    while (!parser.failed && parser.pos < parser.length)
    {
        const char *lineStart;
        const char *lineEnd;
        const char *trimmed;
        size_t trimmedLength;
        char *lineCopy;
        char *scan;

        parser.line++;
        lineStart = text + parser.pos;
        lineEnd = lineStart;
        while (lineEnd < end && *lineEnd != '\n')
            lineEnd++;
        if (lineEnd < end)
            parser.pos = (size_t)(lineEnd - text) + 1u;
        else
            parser.pos = parser.length;
        if (lineEnd > lineStart && lineEnd[-1] == '\r')
            lineEnd--;

        lineCopy = malloc((size_t)(lineEnd - lineStart) + 1u);
        if (lineCopy == NULL)
        {
            Fail(&parser, parser.line, "out of memory", NULL);
            break;
        }
        memcpy(lineCopy, lineStart, (size_t)(lineEnd - lineStart));
        lineCopy[lineEnd - lineStart] = '\0';

        /* cut the line at the first '#' that is outside a string */
        for (scan = lineCopy; *scan != '\0'; scan++)
        {
            if (*scan == '#')
            {
                *scan = '\0';
                break;
            }
            if (*scan == '"' || *scan == '\'')
            {
                char quote = *scan++;
                while (*scan != '\0' && *scan != quote)
                    scan++;
            }
        }

        trimmed = lineCopy;
        while (*trimmed == ' ' || *trimmed == '\t')
            trimmed++;
        trimmedLength = strlen(trimmed);
        while (trimmedLength != 0
               && (trimmed[trimmedLength - 1u] == ' ' || trimmed[trimmedLength - 1u] == '\t'))
            trimmedLength--;

        if (trimmedLength != 0)
        {
            if (trimmed[0] == '[')
            {
                struct Gen3TomlMap *target = NULL;
                if (OpenTableHeader(&parser, trimmed, trimmedLength, parser.line, &target))
                    parser.currentMap = target;
            }
            else
            {
                HandleAssignment(&parser, trimmed, trimmedLength, parser.line, parser.currentMap);
            }
        }
        free(lineCopy);
    }

    return !parser.failed;
}

/* ---- destruction -------------------------------------------------------- */

static void ValueDestroy(struct Gen3TomlValue *value)
{
    size_t i;
    if (value == NULL)
        return;
    free(value->string);
    for (i = 0; i < value->map.count; i++)
    {
        free(value->map.entries[i].key);
        ValueDestroy(value->map.entries[i].value);
    }
    free(value->map.entries);
    for (i = 0; i < value->itemCount; i++)
        ValueDestroy(value->items[i]);
    free(value->items);
    free(value);
}

void Gen3Toml_Destroy(struct Gen3TomlDocument *document)
{
    size_t i;
    if (document == NULL)
        return;
    for (i = 0; i < document->root.count; i++)
    {
        free(document->root.entries[i].key);
        ValueDestroy(document->root.entries[i].value);
    }
    free(document->root.entries);
    memset(document, 0, sizeof(*document));
}

/* ---- lookups ------------------------------------------------------------ */

const struct Gen3TomlEntry *Gen3Toml_FindEntry(const struct Gen3TomlMap *map, const char *key)
{
    size_t i;
    if (map == NULL || key == NULL)
        return NULL;
    for (i = 0; i < map->count; i++)
    {
        if (strcmp(map->entries[i].key, key) == 0)
            return &map->entries[i];
    }
    return NULL;
}

bool Gen3Toml_GetString(const struct Gen3TomlMap *map, const char *key, const char **out)
{
    const struct Gen3TomlEntry *entry = Gen3Toml_FindEntry(map, key);
    if (entry == NULL || entry->value->kind != GEN3_TOML_STRING)
        return false;
    if (out != NULL)
        *out = entry->value->string;
    return true;
}

bool Gen3Toml_GetInteger(const struct Gen3TomlMap *map, const char *key, long long *out)
{
    const struct Gen3TomlEntry *entry = Gen3Toml_FindEntry(map, key);
    if (entry == NULL || entry->value->kind != GEN3_TOML_INTEGER)
        return false;
    if (out != NULL)
        *out = entry->value->integer;
    return true;
}

bool Gen3Toml_GetBool(const struct Gen3TomlMap *map, const char *key, bool *out)
{
    const struct Gen3TomlEntry *entry = Gen3Toml_FindEntry(map, key);
    if (entry == NULL || entry->value->kind != GEN3_TOML_BOOLEAN)
        return false;
    if (out != NULL)
        *out = entry->value->boolean;
    return true;
}

size_t Gen3Toml_GetArrayCount(const struct Gen3TomlMap *map, const char *key)
{
    const struct Gen3TomlEntry *entry = Gen3Toml_FindEntry(map, key);
    if (entry == NULL || entry->value->kind != GEN3_TOML_ARRAY)
        return 0;
    return entry->value->itemCount;
}

const struct Gen3TomlMap *Gen3Toml_GetArrayItem(const struct Gen3TomlMap *map,
                                                const char *key, size_t index)
{
    const struct Gen3TomlEntry *entry = Gen3Toml_FindEntry(map, key);
    if (entry == NULL || entry->value->kind != GEN3_TOML_ARRAY
     || index >= entry->value->itemCount)
        return NULL;
    return &entry->value->items[index]->map;
}

/* ---- deterministic emitter ---------------------------------------------- */

static bool EmitEscaped(struct Gen3Buffer *out, const char *text)
{
    const char *cursor;
    for (cursor = text; *cursor != '\0'; cursor++)
    {
        switch (*cursor)
        {
            case '"': Gen3Buffer_Append(out, "\\\"", 2); break;
            case '\\': Gen3Buffer_Append(out, "\\\\", 2); break;
            case '\n': Gen3Buffer_Append(out, "\\n", 2); break;
            case '\t': Gen3Buffer_Append(out, "\\t", 2); break;
            case '\r': Gen3Buffer_Append(out, "\\r", 2); break;
            default: Gen3Buffer_Append(out, cursor, 1); break;
        }
    }
    return true;
}

bool Gen3TomlWrite_String(struct Gen3Buffer *out, const char *key, const char *value)
{
    if (out == NULL || key == NULL || value == NULL)
        return false;
    return Gen3Buffer_AppendCStr(out, key)
        && Gen3Buffer_AppendCStr(out, " = \"")
        && EmitEscaped(out, value)
        && Gen3Buffer_AppendCStr(out, "\"\n");
}

bool Gen3TomlWrite_Integer(struct Gen3Buffer *out, const char *key, long long value)
{
    if (out == NULL || key == NULL)
        return false;
    return Gen3Buffer_AppendCStr(out, key)
        && Gen3Buffer_AppendCStr(out, " = ")
        && Gen3Buffer_AppendFormat(out, "%lld\n", value);
}

bool Gen3TomlWrite_OpenArrayTable(struct Gen3Buffer *out, const char *key)
{
    if (out == NULL || key == NULL)
        return false;
    return Gen3Buffer_AppendCStr(out, "[[")
        && Gen3Buffer_AppendCStr(out, key)
        && Gen3Buffer_AppendCStr(out, "]]\n");
}
