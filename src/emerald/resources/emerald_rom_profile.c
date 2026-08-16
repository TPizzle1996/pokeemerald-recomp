/* Stage R3 Emerald ROM identity profiles (see emerald_rom_profile.h). */
#include <string.h>

#include "emerald/resources/emerald_rom_profile.h"

const uint8_t kEmeraldProfileBpee01Rev0Sha1[20] = {
    0xf3, 0xae, 0x08, 0x81, 0x81, 0xbf, 0x58, 0x3e, 0x55, 0xda,
    0xf9, 0x62, 0xa9, 0x2b, 0xb4, 0x6f, 0x4f, 0x1d, 0x07, 0xb7,
};

const uint8_t kEmeraldProfileBpee01Rev0Sha256[32] = {
    0xa9, 0xde, 0xc8, 0x4d, 0xfe, 0x7f, 0x62, 0xab,
    0x22, 0x20, 0xba, 0xfa, 0xef, 0x74, 0x79, 0xda,
    0x09, 0x29, 0xd0, 0x66, 0xec, 0xe1, 0x6a, 0x68,
    0x85, 0xf6, 0x22, 0x6d, 0xb1, 0x90, 0x85, 0xaf,
};

const uint8_t kEmeraldProfileSyntheticSha1[20] = {
    0x09, 0x2e, 0xe2, 0x93, 0xc6, 0xe4, 0x38, 0x10, 0x74, 0x68,
    0x23, 0x75, 0x24, 0x95, 0x61, 0xbb, 0xc4, 0x1b, 0xec, 0x44,
};

const uint8_t kEmeraldProfileSyntheticSha256[32] = {
    0xe4, 0xc8, 0xf3, 0x69, 0x3d, 0x84, 0x0f, 0xb2,
    0x33, 0xdf, 0xa3, 0x1c, 0x1c, 0xd2, 0xb4, 0xd2,
    0x1a, 0xa2, 0xdd, 0xe6, 0x73, 0x59, 0xe4, 0xaa,
    0xd4, 0xb0, 0x64, 0xa4, 0xb1, 0xb3, 0x3b, 0xca,
};

static const struct EmeraldRomProfile kProductionProfile = {
    .name = EMERALD_PROFILE_NAME,
    .game = EMERALD_PROFILE_GAME,
    .gameCode = EMERALD_PROFILE_GAME_CODE,
    .makerCode = EMERALD_PROFILE_MAKER_CODE,
    .softwareRevision = EMERALD_PROFILE_SOFTWARE_REV,
    .romSize = EMERALD_ROM_SIZE,
    .romSha1 = kEmeraldProfileBpee01Rev0Sha1,
    .romSha256 = kEmeraldProfileBpee01Rev0Sha256,
    .synthetic = false,
};

static const struct EmeraldRomProfile kSyntheticProfile = {
    .name = EMERALD_PROFILE_NAME,
    .game = EMERALD_PROFILE_GAME,
    .gameCode = EMERALD_PROFILE_GAME_CODE,
    .makerCode = EMERALD_PROFILE_MAKER_CODE,
    .softwareRevision = EMERALD_PROFILE_SOFTWARE_REV,
    .romSize = EMERALD_ROM_SIZE,
    .romSha1 = kEmeraldProfileSyntheticSha1,
    .romSha256 = kEmeraldProfileSyntheticSha256,
    .synthetic = true,
};

const struct EmeraldRomProfile *EmeraldRomProfile_Bpee01Rev0(void)
{
    return &kProductionProfile;
}

const struct EmeraldRomProfile *EmeraldRomProfile_SyntheticFixture(void)
{
    return &kSyntheticProfile;
}

const struct EmeraldRomProfile *EmeraldRomProfile_Find(const char *name)
{
    if (name != NULL && strcmp(name, EMERALD_PROFILE_NAME) == 0)
        return &kProductionProfile;
    return NULL;
}
