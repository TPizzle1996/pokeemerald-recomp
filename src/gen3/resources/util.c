#include "gen3/resources/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <io.h>
#endif

/* MinGW's io.h declares mkdir() with one argument; POSIX takes a mode. */
static int MkdirPath(const char *path)
{
#if defined(_WIN32)
    return mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

static bool BufferReserve(struct Gen3Buffer *buffer, size_t required)
{
    size_t next;
    char *resized;
    if (buffer->capacity >= required)
        return true;
    next = buffer->capacity == 0 ? 256u : buffer->capacity;
    while (next < required)
    {
        if (next > SIZE_MAX / 2u)
            return false;
        next *= 2u;
    }
    if (next > SIZE_MAX / sizeof(*buffer->data))
        return false;
    resized = realloc(buffer->data, next * sizeof(*resized));
    if (resized == NULL)
        return false;
    buffer->data = resized;
    buffer->capacity = next;
    return true;
}

bool Gen3Buffer_Init(struct Gen3Buffer *buffer, size_t initialCapacity)
{
    if (buffer == NULL)
        return false;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    if (initialCapacity != 0 && !BufferReserve(buffer, initialCapacity + 1u))
        return false;
    return true;
}

void Gen3Buffer_Destroy(struct Gen3Buffer *buffer)
{
    if (buffer == NULL)
        return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

bool Gen3Buffer_Append(struct Gen3Buffer *buffer, const void *bytes, size_t size)
{
    if (buffer == NULL || (size != 0 && bytes == NULL))
        return false;
    if (!BufferReserve(buffer, buffer->length + size + 1u))
        return false;
    if (size != 0)
        memcpy(buffer->data + buffer->length, bytes, size);
    buffer->length += size;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool Gen3Buffer_AppendCStr(struct Gen3Buffer *buffer, const char *text)
{
    return text != NULL && Gen3Buffer_Append(buffer, text, strlen(text));
}

bool Gen3Buffer_AppendFormat(struct Gen3Buffer *buffer, const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;
    bool ok;
    if (buffer == NULL || format == NULL)
        return false;
    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0)
    {
        va_end(args);
        return false;
    }
    ok = BufferReserve(buffer, buffer->length + (size_t)needed + 1u);
    if (!ok)
    {
        va_end(args);
        return false;
    }
    vsnprintf(buffer->data + buffer->length, (size_t)needed + 1u, format, args);
    va_end(args);
    buffer->length += (size_t)needed;
    return true;
}

static bool SetError(char *errbuf, size_t errbufSize, const char *format, ...)
{
    va_list args;
    int needed;
    if (errbuf == NULL || errbufSize == 0)
        return false;
    va_start(args, format);
    needed = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (needed < 0)
    {
        errbuf[0] = '\0';
        return false;
    }
    if ((size_t)needed >= errbufSize)
        needed = (int)errbufSize - 1;
    va_start(args, format);
    vsnprintf(errbuf, (size_t)needed + 1u, format, args);
    va_end(args);
    return false;
}

bool Gen3Util_ReadFile(const char *path, struct Gen3Buffer *outBuffer,
                       char *errbuf, size_t errbufSize)
{
    FILE *file;
    size_t chunk;
    uint8_t scratch[8192];
    bool ok;
    if (path == NULL || outBuffer == NULL)
        return SetError(errbuf, errbufSize, "internal: null read-file argument");
    ok = Gen3Buffer_Init(outBuffer, 4096);
    if (!ok)
        return SetError(errbuf, errbufSize, "out of memory reading '%s'", path);
    file = fopen(path, "rb");
    if (file == NULL)
    {
        Gen3Buffer_Destroy(outBuffer);
        return SetError(errbuf, errbufSize, "cannot open '%s': %s", path, strerror(errno));
    }
    while ((chunk = fread(scratch, 1, sizeof(scratch), file)) != 0)
    {
        if (!Gen3Buffer_Append(outBuffer, scratch, chunk))
        {
            fclose(file);
            Gen3Buffer_Destroy(outBuffer);
            return SetError(errbuf, errbufSize, "out of memory reading '%s'", path);
        }
    }
    if (ferror(file))
    {
        int saved = errno;
        fclose(file);
        Gen3Buffer_Destroy(outBuffer);
        return SetError(errbuf, errbufSize, "error reading '%s': %s", path, strerror(saved));
    }
    fclose(file);
    return true;
}

static char *DuplicateString(const char *text)
{
    size_t length;
    char *copy;
    if (text == NULL)
        return NULL;
    length = strlen(text);
    copy = malloc(length + 1u);
    if (copy == NULL)
        return NULL;
    memcpy(copy, text, length + 1u);
    return copy;
}

static bool EnsureParentDirectories(const char *path, char *errbuf, size_t errbufSize)
{
    char *copy;
    char *cursor;
    bool ok = true;
    copy = DuplicateString(path);
    if (copy == NULL)
        return SetError(errbuf, errbufSize, "out of memory creating output path");
    for (cursor = copy + 1; *cursor != '\0'; cursor++)
    {
        if (*cursor == '/')
        {
            *cursor = '\0';
            if (MkdirPath(copy) != 0 && errno != EEXIST)
            {
                ok = SetError(errbuf, errbufSize, "cannot create directory '%s': %s",
                              copy, strerror(errno));
                break;
            }
            *cursor = '/';
        }
    }
    free(copy);
    return ok;
}

bool Gen3Util_WriteFile(const char *path, const char *data, size_t size,
                        char *errbuf, size_t errbufSize)
{
    FILE *file;
    size_t written;
    if (path == NULL)
        return SetError(errbuf, errbufSize, "internal: null write-file path");
    if (!EnsureParentDirectories(path, errbuf, errbufSize))
        return false;
    file = fopen(path, "wb");
    if (file == NULL)
        return SetError(errbuf, errbufSize, "cannot open '%s' for writing: %s",
                        path, strerror(errno));
    written = fwrite(data, 1, size, file);
    if (written != size || fflush(file) != 0 || fclose(file) != 0)
        return SetError(errbuf, errbufSize, "error writing '%s': %s", path, strerror(errno));
    return true;
}

void Gen3Util_FormatHex(const uint8_t *digest, size_t digestSize, char *outHex)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < digestSize; i++)
    {
        outHex[i * 2u] = digits[digest[i] >> 4];
        outHex[i * 2u + 1u] = digits[digest[i] & 15u];
    }
    outHex[digestSize * 2u] = '\0';
}
