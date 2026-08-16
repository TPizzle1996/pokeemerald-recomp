#ifndef GEN3_RESOURCES_UTIL_H
#define GEN3_RESOURCES_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Growable byte/string buffer used to build the deterministic TOML manifest
 * and error messages. Bounds are caller-visible so generators never write
 * past the buffer. */
struct Gen3Buffer
{
    char *data;
    size_t length;
    size_t capacity;
};

bool Gen3Buffer_Init(struct Gen3Buffer *buffer, size_t initialCapacity);
void Gen3Buffer_Destroy(struct Gen3Buffer *buffer);

/* Appends exactly `size` bytes; returns false on allocation failure. */
bool Gen3Buffer_Append(struct Gen3Buffer *buffer, const void *bytes, size_t size);
bool Gen3Buffer_AppendCStr(struct Gen3Buffer *buffer, const char *text);
bool Gen3Buffer_AppendFormat(struct Gen3Buffer *buffer, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Reads an entire file into a freshly allocated buffer. On success the buffer
 * is NUL-terminated (length excludes the terminator) so text files can be
 * passed straight to the TOML parser. Returns false and sets *outError on
 * failure. */
bool Gen3Util_ReadFile(const char *path, struct Gen3Buffer *outBuffer,
                       char *errbuf, size_t errbufSize);

/* Writes a buffer to a file, creating parent directories. Returns false and
 * sets *outError on failure. */
bool Gen3Util_WriteFile(const char *path, const char *data, size_t size,
                        char *errbuf, size_t errbufSize);

/* Lowercase hex formatting for fixed-size digests. */
void Gen3Util_FormatHex(const uint8_t *digest, size_t digestSize,
                        char *outHex /* digestSize*2+1 bytes */);

#endif
