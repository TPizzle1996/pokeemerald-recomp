#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Wandering[] = _("WANDERING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Rickety[] = _("RICKETY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_RockSolid[] = _("ROCK-SOLID");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Hungry[] = _("HUNGRY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Tight[] = _("TIGHT");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Ticklish[] = _("TICKLISH");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Twirling[] = _("TWIRLING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Spiraling[] = _("SPIRALING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Thirsty[] = _("THIRSTY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Lolling[] = _("LOLLING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Silky[] = _("SILKY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Sadly[] = _("SADLY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Hopeless[] = _("HOPELESS");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Useless[] = _("USELESS");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Drooling[] = _("DROOLING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Exciting[] = _("EXCITING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Thick[] = _("THICK");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Smooth[] = _("SMOOTH");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Slimy[] = _("SLIMY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Thin[] = _("THIN");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Break[] = _("BREAK");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Voracious[] = _("VORACIOUS");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Scatter[] = _("SCATTER");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Awesome[] = _("AWESOME");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Wimpy[] = _("WIMPY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Wobbly[] = _("WOBBLY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Shaky[] = _("SHAKY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Ripped[] = _("RIPPED");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Shredded[] = _("SHREDDED");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Increasing[] = _("INCREASING");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Yet[] = _("YET");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Destroyed[] = _("DESTROYED");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Fiery[] = _("FIERY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_LoveyDovey[] = _("LOVEY-DOVEY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Happily[] = _("HAPPILY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Anticipation[] = _("ANTICIPATION");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT

#if defined(NATIVE_LINUX)
struct EasyChatWordInfo gEasyChatGroup_Adjectives[36];
#else
const struct EasyChatWordInfo gEasyChatGroup_Adjectives[] = {
    [EC_INDEX(EC_WORD_WANDERING)] =
    {
        .text = gEasyChatWord_Wandering,
        .alphabeticalOrder = EC_INDEX(EC_WORD_ANTICIPATION),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_RICKETY)] =
    {
        .text = gEasyChatWord_Rickety,
        .alphabeticalOrder = EC_INDEX(EC_WORD_AWESOME),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_ROCK_SOLID)] =
    {
        .text = gEasyChatWord_RockSolid,
        .alphabeticalOrder = EC_INDEX(EC_WORD_BREAK),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_HUNGRY)] =
    {
        .text = gEasyChatWord_Hungry,
        .alphabeticalOrder = EC_INDEX(EC_WORD_DESTROYED),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_TIGHT)] =
    {
        .text = gEasyChatWord_Tight,
        .alphabeticalOrder = EC_INDEX(EC_WORD_DROOLING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_TICKLISH)] =
    {
        .text = gEasyChatWord_Ticklish,
        .alphabeticalOrder = EC_INDEX(EC_WORD_EXCITING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_TWIRLING)] =
    {
        .text = gEasyChatWord_Twirling,
        .alphabeticalOrder = EC_INDEX(EC_WORD_FIERY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SPIRALING)] =
    {
        .text = gEasyChatWord_Spiraling,
        .alphabeticalOrder = EC_INDEX(EC_WORD_HAPPILY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_THIRSTY)] =
    {
        .text = gEasyChatWord_Thirsty,
        .alphabeticalOrder = EC_INDEX(EC_WORD_HOPELESS),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_LOLLING)] =
    {
        .text = gEasyChatWord_Lolling,
        .alphabeticalOrder = EC_INDEX(EC_WORD_HUNGRY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SILKY)] =
    {
        .text = gEasyChatWord_Silky,
        .alphabeticalOrder = EC_INDEX(EC_WORD_INCREASING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SADLY)] =
    {
        .text = gEasyChatWord_Sadly,
        .alphabeticalOrder = EC_INDEX(EC_WORD_LOLLING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_HOPELESS)] =
    {
        .text = gEasyChatWord_Hopeless,
        .alphabeticalOrder = EC_INDEX(EC_WORD_LOVEY_DOVEY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_USELESS)] =
    {
        .text = gEasyChatWord_Useless,
        .alphabeticalOrder = EC_INDEX(EC_WORD_RICKETY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_DROOLING)] =
    {
        .text = gEasyChatWord_Drooling,
        .alphabeticalOrder = EC_INDEX(EC_WORD_RIPPED),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_EXCITING)] =
    {
        .text = gEasyChatWord_Exciting,
        .alphabeticalOrder = EC_INDEX(EC_WORD_ROCK_SOLID),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_THICK)] =
    {
        .text = gEasyChatWord_Thick,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SADLY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SMOOTH)] =
    {
        .text = gEasyChatWord_Smooth,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SCATTER),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SLIMY)] =
    {
        .text = gEasyChatWord_Slimy,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SHAKY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_THIN)] =
    {
        .text = gEasyChatWord_Thin,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SHREDDED),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_BREAK)] =
    {
        .text = gEasyChatWord_Break,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SILKY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_VORACIOUS)] =
    {
        .text = gEasyChatWord_Voracious,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SLIMY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SCATTER)] =
    {
        .text = gEasyChatWord_Scatter,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SMOOTH),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_AWESOME)] =
    {
        .text = gEasyChatWord_Awesome,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SPIRALING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_WIMPY)] =
    {
        .text = gEasyChatWord_Wimpy,
        .alphabeticalOrder = EC_INDEX(EC_WORD_THICK),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_WOBBLY)] =
    {
        .text = gEasyChatWord_Wobbly,
        .alphabeticalOrder = EC_INDEX(EC_WORD_THIN),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SHAKY)] =
    {
        .text = gEasyChatWord_Shaky,
        .alphabeticalOrder = EC_INDEX(EC_WORD_THIRSTY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_RIPPED)] =
    {
        .text = gEasyChatWord_Ripped,
        .alphabeticalOrder = EC_INDEX(EC_WORD_TICKLISH),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SHREDDED)] =
    {
        .text = gEasyChatWord_Shredded,
        .alphabeticalOrder = EC_INDEX(EC_WORD_TIGHT),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_INCREASING)] =
    {
        .text = gEasyChatWord_Increasing,
        .alphabeticalOrder = EC_INDEX(EC_WORD_TWIRLING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_YET)] =
    {
        .text = gEasyChatWord_Yet,
        .alphabeticalOrder = EC_INDEX(EC_WORD_USELESS),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_DESTROYED)] =
    {
        .text = gEasyChatWord_Destroyed,
        .alphabeticalOrder = EC_INDEX(EC_WORD_VORACIOUS),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_FIERY)] =
    {
        .text = gEasyChatWord_Fiery,
        .alphabeticalOrder = EC_INDEX(EC_WORD_WANDERING),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_LOVEY_DOVEY)] =
    {
        .text = gEasyChatWord_LoveyDovey,
        .alphabeticalOrder = EC_INDEX(EC_WORD_WIMPY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_HAPPILY)] =
    {
        .text = gEasyChatWord_Happily,
        .alphabeticalOrder = EC_INDEX(EC_WORD_WOBBLY),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_ANTICIPATION)] =
    {
        .text = gEasyChatWord_Anticipation,
        .alphabeticalOrder = EC_INDEX(EC_WORD_YET),
        .enabled = TRUE,
    },
};
#endif
