#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_IChooseYou[] = _("I CHOOSE YOU");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Gotcha[] = _("GOTCHA");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Trade[] = _("TRADE");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Sapphire[] = _("SAPPHIRE");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Evolve[] = _("EVOLVE");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Encyclopedia[] = _("ENCYCLOPEDIA");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Nature[] = _("NATURE");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Center[] = _("CENTER");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Egg[] = _("EGG");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Link[] = _("LINK");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_SpAbility[] = _("SP. ABILITY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Trainer[] = _("TRAINER");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Version[] = _("VERSION");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Pokenav[] = _("POKéNAV");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Pokemon[] = _("POKéMON");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Get[] = _("GET");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Pokedex[] = _("POKéDEX");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Ruby[] = _("RUBY");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Level[] = _("LEVEL");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Red[] = _("RED");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Green[] = _("GREEN");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Bag[] = _("BAG");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Flame[] = _("FLAME");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Gold[] = _("GOLD");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Leaf[] = _("LEAF");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Silver[] = _("SILVER");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT
#ifndef DESKTOP_EXTERNAL_GAME_CONTENT
const u8 gEasyChatWord_Emerald[] = _("EMERALD");
#endif // DESKTOP_EXTERNAL_GAME_CONTENT

#if defined(NATIVE_LINUX)
struct EasyChatWordInfo gEasyChatGroup_Trainer[27];
#else
const struct EasyChatWordInfo gEasyChatGroup_Trainer[] = {
    [EC_INDEX(EC_WORD_I_CHOOSE_YOU)] =
    {
        .text = gEasyChatWord_IChooseYou,
        .alphabeticalOrder = EC_INDEX(EC_WORD_BAG),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_GOTCHA)] =
    {
        .text = gEasyChatWord_Gotcha,
        .alphabeticalOrder = EC_INDEX(EC_WORD_CENTER),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_TRADE)] =
    {
        .text = gEasyChatWord_Trade,
        .alphabeticalOrder = EC_INDEX(EC_WORD_EGG),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SAPPHIRE)] =
    {
        .text = gEasyChatWord_Sapphire,
        .alphabeticalOrder = EC_INDEX(EC_WORD_EMERALD),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_EVOLVE)] =
    {
        .text = gEasyChatWord_Evolve,
        .alphabeticalOrder = EC_INDEX(EC_WORD_ENCYCLOPEDIA),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_ENCYCLOPEDIA)] =
    {
        .text = gEasyChatWord_Encyclopedia,
        .alphabeticalOrder = EC_INDEX(EC_WORD_EVOLVE),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_NATURE)] =
    {
        .text = gEasyChatWord_Nature,
        .alphabeticalOrder = EC_INDEX(EC_WORD_FLAME),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_CENTER)] =
    {
        .text = gEasyChatWord_Center,
        .alphabeticalOrder = EC_INDEX(EC_WORD_GET),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_EGG)] =
    {
        .text = gEasyChatWord_Egg,
        .alphabeticalOrder = EC_INDEX(EC_WORD_GOLD),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_LINK)] =
    {
        .text = gEasyChatWord_Link,
        .alphabeticalOrder = EC_INDEX(EC_WORD_GOTCHA),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_SP_ABILITY)] =
    {
        .text = gEasyChatWord_SpAbility,
        .alphabeticalOrder = EC_INDEX(EC_WORD_GREEN),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_TRAINER)] =
    {
        .text = gEasyChatWord_Trainer,
        .alphabeticalOrder = EC_INDEX(EC_WORD_I_CHOOSE_YOU),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_VERSION)] =
    {
        .text = gEasyChatWord_Version,
        .alphabeticalOrder = EC_INDEX(EC_WORD_LEAF),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_POKENAV)] =
    {
        .text = gEasyChatWord_Pokenav,
        .alphabeticalOrder = EC_INDEX(EC_WORD_LEVEL),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_POKEMON)] =
    {
        .text = gEasyChatWord_Pokemon,
        .alphabeticalOrder = EC_INDEX(EC_WORD_LINK),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_GET)] =
    {
        .text = gEasyChatWord_Get,
        .alphabeticalOrder = EC_INDEX(EC_WORD_NATURE),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_POKEDEX)] =
    {
        .text = gEasyChatWord_Pokedex,
        .alphabeticalOrder = EC_INDEX(EC_WORD_POKEDEX),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_RUBY)] =
    {
        .text = gEasyChatWord_Ruby,
        .alphabeticalOrder = EC_INDEX(EC_WORD_POKEMON),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_LEVEL)] =
    {
        .text = gEasyChatWord_Level,
        .alphabeticalOrder = EC_INDEX(EC_WORD_POKENAV),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_RED)] =
    {
        .text = gEasyChatWord_Red,
        .alphabeticalOrder = EC_INDEX(EC_WORD_RED),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_GREEN)] =
    {
        .text = gEasyChatWord_Green,
        .alphabeticalOrder = EC_INDEX(EC_WORD_RUBY),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_BAG)] =
    {
        .text = gEasyChatWord_Bag,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SAPPHIRE),
        .enabled = TRUE,
    },
    [EC_INDEX(EC_WORD_FLAME)] =
    {
        .text = gEasyChatWord_Flame,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SILVER),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_GOLD)] =
    {
        .text = gEasyChatWord_Gold,
        .alphabeticalOrder = EC_INDEX(EC_WORD_SP_ABILITY),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_LEAF)] =
    {
        .text = gEasyChatWord_Leaf,
        .alphabeticalOrder = EC_INDEX(EC_WORD_TRADE),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_SILVER)] =
    {
        .text = gEasyChatWord_Silver,
        .alphabeticalOrder = EC_INDEX(EC_WORD_TRAINER),
        .enabled = FALSE,
    },
    [EC_INDEX(EC_WORD_EMERALD)] =
    {
        .text = gEasyChatWord_Emerald,
        .alphabeticalOrder = EC_INDEX(EC_WORD_VERSION),
        .enabled = TRUE,
    },
};
#endif
