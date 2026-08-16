#ifndef GEN3_RESOURCES_RESOURCE_CATALOG_H
#define GEN3_RESOURCES_RESOURCE_CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gen3/resources/resource_diagnostics.h"
#include "gen3/resources/resource_id.h"
#include "gen3/resources/resource_types.h"

struct Gen3ResourceCatalog;

struct Gen3ResourceContract
{
    Gen3ResourceHandle handle;
    Gen3ResourceKey key;
    const char *canonicalName;
    enum Gen3ResourceType type;
    uint32_t schema;
    bool requiredForBase;
};

struct Gen3ResourceCatalog *Gen3ResourceCatalog_Create(void);
void Gen3ResourceCatalog_Destroy(struct Gen3ResourceCatalog *catalog);
bool Gen3ResourceCatalog_Add(struct Gen3ResourceCatalog *catalog,
                             const char *canonicalName,
                             enum Gen3ResourceType type,
                             uint32_t schema,
                             bool requiredForBase,
                             struct Gen3ResourceDiagnosticList *diagnostics);
bool Gen3ResourceCatalog_Finalize(struct Gen3ResourceCatalog *catalog,
                                  struct Gen3ResourceDiagnosticList *diagnostics);
size_t Gen3ResourceCatalog_Count(const struct Gen3ResourceCatalog *catalog);
const struct Gen3ResourceContract *Gen3ResourceCatalog_At(
    const struct Gen3ResourceCatalog *catalog, size_t index);
const struct Gen3ResourceContract *Gen3ResourceCatalog_Find(
    const struct Gen3ResourceCatalog *catalog, const char *canonicalName);

#endif
