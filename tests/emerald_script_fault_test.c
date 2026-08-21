/* R13-G3 fault-injection driver (test-only).
 *
 * Compiled once per mutated generated-table variant (the runner links
 * the seam TU against the mutated copy of script_native_table.generated.c).
 * Drives the real harness setup (production pack -> R6 loader -> sibling
 * seams), then EmeraldScriptCompat_TryInitialize must refuse with exactly
 * the expected status named on the command line (plan sec 17: no
 * fallback, hard refusal).
 *
 * usage: emerald_script_fault_test <expected-status-name>
 *                                 <temp-dir> <production-pack.rpack>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "emerald/resources/emerald_script_compat.h"

#include "emerald_script_compat_harness.h"

static int ExpectedStatusFromName(const char *name)
{
    if (strcmp(name, "EMERALD_SCRIPT_ERR_SEGMENT_INVALID") == 0)
        return EMERALD_SCRIPT_ERR_SEGMENT_INVALID;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_EXPORT_INVALID") == 0)
        return EMERALD_SCRIPT_ERR_EXPORT_INVALID;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_RELOC_INVALID") == 0)
        return EMERALD_SCRIPT_ERR_RELOC_INVALID;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED") == 0)
        return EMERALD_SCRIPT_ERR_TARGET_UNRESOLVED;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_BOUNDARY_INVALID") == 0)
        return EMERALD_SCRIPT_ERR_BOUNDARY_INVALID;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_TABLE_MISMATCH") == 0)
        return EMERALD_SCRIPT_ERR_TABLE_MISMATCH;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT") == 0)
        return EMERALD_SCRIPT_ERR_UNEXPECTED_COUNT;
    if (strcmp(name, "EMERALD_SCRIPT_ERR_OUT_OF_MEMORY") == 0)
        return EMERALD_SCRIPT_ERR_OUT_OF_MEMORY;
    return -1;
}

int main(int argc, char **argv)
{
    const char *expectedName;
    const char *tempDir;
    const char *prodPack;
    int expected;
    uint64_t sGenerationBeforeFault = 0u;
    struct EmeraldScriptCompatDiagnostics diag;
    enum EmeraldScriptCompatStatus status;

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <expected-status> <temp-dir> "
                        "<production-pack.rpack>\n", argv[0]);
        return 2;
    }
    expectedName = argv[1];
    tempDir = argv[2];
    prodPack = argv[3];
    expected = ExpectedStatusFromName(expectedName);
    if (expected < 0)
    {
        fprintf(stderr, "unknown expected status '%s'\n", expectedName);
        return 2;
    }
    gScriptHarnessTempDir = tempDir;
    gScriptHarnessPackPath = prodPack;

    if (!SetupScriptCompatSessionWithoutScript(prodPack))
    {
        fprintf(stderr, "fault driver: setup failed\n");
        TeardownScriptCompatSession();
        return 1;
    }
#ifdef EMERALD_SCRIPT_COMPAT_TEST_HOOKS
    if (expected == EMERALD_SCRIPT_ERR_OUT_OF_MEMORY)
    {
        /* The stage allocation order is generation, arena, spans,
         * sources, F plan: failing call 3 (the source index) leaves
         * the arena + span index allocated but refuses the stage -
         * the deterministic partial-allocation fault. */
        EmeraldScriptCompat_TestSetAllocFail(3u);
    }
#endif
    sGenerationBeforeFault = EmeraldScriptCompat_GetGenerationId();
    memset(&diag, 0, sizeof(diag));
    status = EmeraldScriptCompat_TryInitialize(
        gScriptHarnessSnapshot, gScriptHarnessPack, &diag);
    if (status != expected)
    {
        fprintf(stderr, "fault driver: expected %s (%d), got %s (%d) "
                        "stage '%s' name '%s'\n",
                expectedName, expected,
                EmeraldScriptCompatStatus_Describe(status), (int)status,
                diag.stage, diag.canonicalName);
        EmeraldScriptCompat_ClearMigratedEntries();
        TeardownScriptCompatSession();
        return 1;
    }
    /* A refused stage must publish nothing: the live generation
     * (published by the R6 loader) stays untouched. */
    if (EmeraldScriptCompat_GetGenerationId() != sGenerationBeforeFault)
    {
        fprintf(stderr, "fault driver: refused stage left a generation\n");
        EmeraldScriptCompat_ClearMigratedEntries();
        TeardownScriptCompatSession();
        return 1;
    }
    printf("fault refused as expected: %s\n", expectedName);
    EmeraldScriptCompat_ClearMigratedEntries();
    TeardownScriptCompatSession();
    return 0;
}
