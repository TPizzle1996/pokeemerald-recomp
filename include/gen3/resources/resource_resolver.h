#ifndef GEN3_RESOURCES_RESOURCE_RESOLVER_H
#define GEN3_RESOURCES_RESOURCE_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_catalog.h"
#include "gen3/resources/resource_provider.h"

struct Gen3ResourceCandidate;
struct Gen3ResourceSnapshot;

enum Gen3ResourceResult
{
    GEN3_RESOURCE_OK = 0,
    GEN3_RESOURCE_NOT_FOUND,
    GEN3_RESOURCE_WRONG_TYPE,
    GEN3_RESOURCE_WRONG_SCHEMA,
    GEN3_RESOURCE_INVALID_HANDLE,
};

struct Gen3ResourceView
{
    Gen3ResourceHandle handle;
    Gen3ResourceKey key;
    const char *canonicalName;
    enum Gen3ResourceType type;
    uint32_t schema;
    const uint8_t *payload;
    size_t payloadSize;
    const char *winningProviderId;
    const char *winningProviderVersion;
    uint32_t winningProviderPrecedence;
};

enum Gen3ResourceTraceDisposition
{
    GEN3_TRACE_REJECTED = 0,
    GEN3_TRACE_WINNER,
    GEN3_TRACE_FALLBACK,
};

struct Gen3ResourceTraceRecord
{
    char providerId[GEN3_PROVIDER_ID_MAX + 1u];
    char providerVersion[GEN3_PROVIDER_VERSION_MAX + 1u];
    uint32_t precedence;
    enum Gen3ResourceTraceDisposition disposition;
    enum Gen3ResourceReason reason;
};

struct Gen3ResourceTrace
{
    struct Gen3ResourceTraceRecord *items;
    size_t count;
    size_t capacity;
};

struct Gen3ResourceRegistry
{
    struct Gen3ResourceSnapshot *active;
};

struct Gen3ResourceCandidate *Gen3ResourceCandidate_Create(
    const struct Gen3ResourceCatalog *catalog,
    struct Gen3ResourceDiagnosticList *diagnostics);
void Gen3ResourceCandidate_Destroy(struct Gen3ResourceCandidate *candidate);
bool Gen3ResourceCandidate_AddProvider(
    struct Gen3ResourceCandidate *candidate,
    const struct Gen3ResourceProvider *provider,
    struct Gen3ResourceDiagnosticList *diagnostics);
bool Gen3ResourceCandidate_Build(
    const struct Gen3ResourceCandidate *candidate,
    struct Gen3ResourceSnapshot **outSnapshot,
    struct Gen3ResourceDiagnosticList *diagnostics);

void Gen3ResourceSnapshot_Destroy(struct Gen3ResourceSnapshot *snapshot);
size_t Gen3ResourceSnapshot_Count(const struct Gen3ResourceSnapshot *snapshot);
enum Gen3ResourceResult Gen3ResourceSnapshot_FindHandle(
    const struct Gen3ResourceSnapshot *snapshot,
    const char *canonicalName,
    Gen3ResourceHandle *outHandle);
enum Gen3ResourceResult Gen3ResourceSnapshot_Resolve(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    enum Gen3ResourceType expectedType,
    uint32_t expectedSchema,
    struct Gen3ResourceView *outView);
enum Gen3ResourceResult Gen3ResourceSnapshot_GetTileGraphics(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    uint32_t expectedSchema,
    struct Gen3TileGraphicsView *outView);
enum Gen3ResourceResult Gen3ResourceSnapshot_GetPalette(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    uint32_t expectedSchema,
    struct Gen3PaletteView *outView);

void Gen3ResourceTrace_Init(struct Gen3ResourceTrace *trace);
void Gen3ResourceTrace_Destroy(struct Gen3ResourceTrace *trace);
bool Gen3ResourceSnapshot_Trace(
    const struct Gen3ResourceSnapshot *snapshot,
    Gen3ResourceHandle handle,
    struct Gen3ResourceTrace *trace);

void Gen3ResourceRegistry_Init(struct Gen3ResourceRegistry *registry);
void Gen3ResourceRegistry_Destroy(struct Gen3ResourceRegistry *registry);
void Gen3ResourceRegistry_Publish(struct Gen3ResourceRegistry *registry,
                                  struct Gen3ResourceSnapshot *snapshot);
const struct Gen3ResourceSnapshot *Gen3ResourceRegistry_Active(
    const struct Gen3ResourceRegistry *registry);

#endif
