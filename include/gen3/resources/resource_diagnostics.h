#ifndef GEN3_RESOURCES_RESOURCE_DIAGNOSTICS_H
#define GEN3_RESOURCES_RESOURCE_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_types.h"

#define GEN3_PROVIDER_ID_MAX 127u
#define GEN3_PROVIDER_VERSION_MAX 63u

enum Gen3ResourceDiagnosticSeverity
{
    GEN3_DIAGNOSTIC_INFO = 0,
    GEN3_DIAGNOSTIC_WARNING,
    GEN3_DIAGNOSTIC_ERROR,
};

enum Gen3ResourceReason
{
    GEN3_RESOURCE_REASON_NONE = 0,
    GEN3_RESOURCE_REASON_INVALID_ARGUMENT,
    GEN3_RESOURCE_REASON_OUT_OF_MEMORY,
    GEN3_RESOURCE_REASON_INVALID_CANONICAL_NAME,
    GEN3_RESOURCE_REASON_DUPLICATE_CATALOG_NAME,
    GEN3_RESOURCE_REASON_RESOURCE_KEY_COLLISION,
    GEN3_RESOURCE_REASON_CATALOG_NOT_FINALIZED,
    GEN3_RESOURCE_REASON_PROVIDER_NOT_FINALIZED,
    GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_ID,
    GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_PRECEDENCE,
    GEN3_RESOURCE_REASON_DUPLICATE_PROVIDER_ENTRY,
    GEN3_RESOURCE_REASON_UNKNOWN_RESOURCE,
    GEN3_RESOURCE_REASON_TYPE_MISMATCH,
    GEN3_RESOURCE_REASON_SCHEMA_MISMATCH,
    GEN3_RESOURCE_REASON_INVALID_PAYLOAD,
    GEN3_RESOURCE_REASON_MISSING_REQUIRED_BASE_RESOURCE,
    GEN3_RESOURCE_REASON_RESOURCE_NOT_RESOLVED,
};

struct Gen3ResourceDiagnostic
{
    enum Gen3ResourceDiagnosticSeverity severity;
    enum Gen3ResourceReason reason;
    char resourceName[256];
    char providerId[GEN3_PROVIDER_ID_MAX + 1u];
    enum Gen3ResourceType expectedType;
    enum Gen3ResourceType actualType;
    uint32_t expectedSchema;
    uint32_t actualSchema;
    bool providerEntryRequired;
    bool catalogRequiredForBase;
};

struct Gen3ResourceDiagnosticList
{
    struct Gen3ResourceDiagnostic *items;
    size_t count;
    size_t capacity;
};

void Gen3ResourceDiagnostics_Init(struct Gen3ResourceDiagnosticList *list);
void Gen3ResourceDiagnostics_Destroy(struct Gen3ResourceDiagnosticList *list);
bool Gen3ResourceDiagnostics_Append(
    struct Gen3ResourceDiagnosticList *list,
    enum Gen3ResourceDiagnosticSeverity severity,
    enum Gen3ResourceReason reason,
    const char *resourceName,
    const char *providerId,
    enum Gen3ResourceType expectedType,
    enum Gen3ResourceType actualType,
    uint32_t expectedSchema,
    uint32_t actualSchema,
    bool providerEntryRequired,
    bool catalogRequiredForBase);
const char *Gen3ResourceReason_Describe(enum Gen3ResourceReason reason);

#endif
