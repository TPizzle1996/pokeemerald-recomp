#ifdef NATIVE_LINUX
#include "emerald/resources/text_slots.generated.h"
#include "emerald/resources/text_skeleton_arrays.generated.h"
#endif
#ifndef NATIVE_LINUX
static const u8 sHardyNatureName[] = _("HARDY");
#endif
#ifndef NATIVE_LINUX
static const u8 sLonelyNatureName[] = _("LONELY");
#endif
#ifndef NATIVE_LINUX
static const u8 sBraveNatureName[] = _("BRAVE");
#endif
#ifndef NATIVE_LINUX
static const u8 sAdamantNatureName[] = _("ADAMANT");
#endif
#ifndef NATIVE_LINUX
static const u8 sNaughtyNatureName[] = _("NAUGHTY");
#endif
#ifndef NATIVE_LINUX
static const u8 sBoldNatureName[] = _("BOLD");
#endif
#ifndef NATIVE_LINUX
static const u8 sDocileNatureName[] = _("DOCILE");
#endif
#ifndef NATIVE_LINUX
static const u8 sRelaxedNatureName[] = _("RELAXED");
#endif
#ifndef NATIVE_LINUX
static const u8 sImpishNatureName[] = _("IMPISH");
#endif
#ifndef NATIVE_LINUX
static const u8 sLaxNatureName[] = _("LAX");
#endif
#ifndef NATIVE_LINUX
static const u8 sTimidNatureName[] = _("TIMID");
#endif
#ifndef NATIVE_LINUX
static const u8 sHastyNatureName[] = _("HASTY");
#endif
#ifndef NATIVE_LINUX
static const u8 sSeriousNatureName[] = _("SERIOUS");
#endif
#ifndef NATIVE_LINUX
static const u8 sJollyNatureName[] = _("JOLLY");
#endif
#ifndef NATIVE_LINUX
static const u8 sNaiveNatureName[] = _("NAIVE");
#endif
#ifndef NATIVE_LINUX
static const u8 sModestNatureName[] = _("MODEST");
#endif
#ifndef NATIVE_LINUX
static const u8 sMildNatureName[] = _("MILD");
#endif
#ifndef NATIVE_LINUX
static const u8 sQuietNatureName[] = _("QUIET");
#endif
#ifndef NATIVE_LINUX
static const u8 sBashfulNatureName[] = _("BASHFUL");
#endif
#ifndef NATIVE_LINUX
static const u8 sRashNatureName[] = _("RASH");
#endif
#ifndef NATIVE_LINUX
static const u8 sCalmNatureName[] = _("CALM");
#endif
#ifndef NATIVE_LINUX
static const u8 sGentleNatureName[] = _("GENTLE");
#endif
#ifndef NATIVE_LINUX
static const u8 sSassyNatureName[] = _("SASSY");
#endif
#ifndef NATIVE_LINUX
static const u8 sCarefulNatureName[] = _("CAREFUL");
#endif
#ifndef NATIVE_LINUX
static const u8 sQuirkyNatureName[] = _("QUIRKY");
#endif

/* R13-C: skeleton-migrated table (rows filled at publish). */
#ifndef NATIVE_LINUX
const u8 *const gNatureNamePointers[NUM_NATURES] =
{
    [NATURE_HARDY] = sHardyNatureName,
    [NATURE_LONELY] = sLonelyNatureName,
    [NATURE_BRAVE] = sBraveNatureName,
    [NATURE_ADAMANT] = sAdamantNatureName,
    [NATURE_NAUGHTY] = sNaughtyNatureName,
    [NATURE_BOLD] = sBoldNatureName,
    [NATURE_DOCILE] = sDocileNatureName,
    [NATURE_RELAXED] = sRelaxedNatureName,
    [NATURE_IMPISH] = sImpishNatureName,
    [NATURE_LAX] = sLaxNatureName,
    [NATURE_TIMID] = sTimidNatureName,
    [NATURE_HASTY] = sHastyNatureName,
    [NATURE_SERIOUS] = sSeriousNatureName,
    [NATURE_JOLLY] = sJollyNatureName,
    [NATURE_NAIVE] = sNaiveNatureName,
    [NATURE_MODEST] = sModestNatureName,
    [NATURE_MILD] = sMildNatureName,
    [NATURE_QUIET] = sQuietNatureName,
    [NATURE_BASHFUL] = sBashfulNatureName,
    [NATURE_RASH] = sRashNatureName,
    [NATURE_CALM] = sCalmNatureName,
    [NATURE_GENTLE] = sGentleNatureName,
    [NATURE_SASSY] = sSassyNatureName,
    [NATURE_CAREFUL] = sCarefulNatureName,
    [NATURE_QUIRKY] = sQuirkyNatureName,
};
#endif
