#ifdef PLATFORM_SDL2

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "global.h"
#include "platform/desktop_config.h"
#include "platform/desktop_filesystem.h"
#include "platform/desktop_storage.h"

#define PLATFORM_CONFIG_VERSION 2

HOST_DATA static u8 sPlatformSettings[PLATFORM_SETTING_COUNT] = {0, 4, 0, 1, 1, 10};
HOST_DATA static struct PlatformInputBinding sKeyboardBindings[PLATFORM_INPUT_ACTION_COUNT];
HOST_DATA static u8 sFastForwardSpeed;

static bool32 ParseValue(const char *line, const char *key, unsigned int *value)
{
    char *end;
    unsigned long parsed;
    size_t keyLength = strlen(key);

    if (strncmp(line, key, keyLength) != 0 || line[keyLength] != '=')
        return FALSE;
    parsed = strtoul(line + keyLength + 1, &end, 10);
    if (end == line + keyLength + 1 || parsed > UINT_MAX)
        return FALSE;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
        end++;
    if (*end != '\0')
        return FALSE;
    *value = (unsigned int)parsed;
    return TRUE;
}

static void ResetDefaults(void)
{
    static const u8 defaults[PLATFORM_SETTING_COUNT] = {0, 4, 0, 1, 1, 10};
    int i;

    memcpy(sPlatformSettings, defaults, sizeof(defaults));
    for (i = 0; i < PLATFORM_INPUT_ACTION_COUNT; i++)
        sKeyboardBindings[i] = Platform_InputGetDefaultBinding(i);
    sFastForwardSpeed = 5;
}

static bool32 ParseString(const char *line, const char *key, char *value, u32 valueSize)
{
    size_t keyLength = strlen(key);
    const char *start;
    const char *end;
    size_t length;

    if (strncmp(line, key, keyLength) != 0 || line[keyLength] != '=')
        return FALSE;
    start = line + keyLength + 1;
    end = start + strlen(start);
    while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '
                        || end[-1] == '\t'))
        end--;
    length = end - start;
    if (length + 1 > valueSize)
        return FALSE;
    memcpy(value, start, length);
    value[length] = '\0';
    return TRUE;
}

static bool32 ParseBinding(const char *text, struct PlatformInputBinding *binding)
{
    char copy[96];
    char *token;
    char *save;
    u8 modifiers = 0;
    enum PlatformInputKey key = PLATFORM_INPUT_KEY_NONE;

    if (strlen(text) >= sizeof(copy))
        return FALSE;
    strcpy(copy, text);
    token = strtok_r(copy, "+", &save);
    while (token != NULL)
    {
        if (strcmp(token, "Shift") == 0)
            modifiers |= PLATFORM_INPUT_MODIFIER_SHIFT;
        else if (strcmp(token, "Ctrl") == 0)
            modifiers |= PLATFORM_INPUT_MODIFIER_CTRL;
        else if (strcmp(token, "Alt") == 0)
            modifiers |= PLATFORM_INPUT_MODIFIER_ALT;
        else if (strcmp(token, "Gui") == 0)
            modifiers |= PLATFORM_INPUT_MODIFIER_GUI;
        else
            key = Platform_InputKeyFromName(token);
        token = strtok_r(NULL, "+", &save);
    }
    if (key == PLATFORM_INPUT_KEY_NONE)
        return FALSE;
    binding->key = key;
    binding->modifiers = modifiers;
    return Platform_InputBindingIsUsable(binding);
}

static void LoadPath(const char *path)
{
    FILE *file;
    char line[64];
    char bindingText[96];
    unsigned int value;
    int i;

    file = Platform_FileOpen(path, "r");
    if (file == NULL)
        return;
    while (fgets(line, sizeof(line), file) != NULL)
    {
        if (ParseValue(line, "configVersion", &value) && value == PLATFORM_CONFIG_VERSION)
        {
            /* Reserved for future schema migrations; unknown versions are ignored. */
        }
        else if (ParseValue(line, "fullscreen", &value) && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_FULLSCREEN] = value;
        else if (ParseValue(line, "windowScale", &value) && value >= 2 && value <= 5)
            sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE] = value;
        else if (ParseValue(line, "integerScale", &value) && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_INTEGER_SCALE] = value;
        else if (ParseValue(line, "vsync", &value) && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_VSYNC] = value;
        else if (ParseValue(line, "border", &value) && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_BORDER] = value;
        else if (ParseValue(line, "volume", &value) && value <= 10)
            sPlatformSettings[PLATFORM_SETTING_VOLUME] = value;
        else if (ParseValue(line, "fastForwardSpeed", &value)
              && (value == 0 || (value >= 2 && value <= 5)))
            sFastForwardSpeed = value;
        else
        {
            for (i = 0; i < PLATFORM_INPUT_ACTION_COUNT; i++)
            {
                char key[64];
                snprintf(key, sizeof(key), "input.%s", Platform_InputActionConfigName(i));
                if (ParseString(line, key, bindingText, sizeof(bindingText)))
                {
                    struct PlatformInputBinding binding;
                    if (ParseBinding(bindingText, &binding))
                        sKeyboardBindings[i] = binding;
                    break;
                }
            }
        }
    }
    fclose(file);
}

void Platform_ConfigLoad(void)
{
    const char *preferredPath = Platform_StorageGetConfigPath();
    const char *legacyPath = Platform_StorageGetLegacyConfigPath();
    bool32 preferredExists;

    ResetDefaults();
    {
        FILE *preferredFile = Platform_FileOpen(preferredPath, "r");
        preferredExists = preferredFile != NULL;
        if (preferredFile != NULL)
            fclose(preferredFile);
    }
    if (preferredExists)
    {
        LoadPath(preferredPath);
    }
    else
    {
        LoadPath(legacyPath);
        if (Platform_StorageUsesPreferredRoot())
            Platform_ConfigStore();
    }
}

void Platform_ConfigStore(void)
{
    char contents[4096];
    char bindingText[96];
    int length;
    int offset;
    int i;
    const char *preferredPath = Platform_StorageGetConfigPath();

    offset = snprintf(contents, sizeof(contents),
                      "configVersion=%u\n"
                      "fullscreen=%u\n"
                      "windowScale=%u\n"
                      "integerScale=%u\n"
                      "vsync=%u\n"
                      "border=%u\n"
                      "volume=%u\n",
                      PLATFORM_CONFIG_VERSION,
                      sPlatformSettings[PLATFORM_SETTING_FULLSCREEN],
                      sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE],
                      sPlatformSettings[PLATFORM_SETTING_INTEGER_SCALE],
                      sPlatformSettings[PLATFORM_SETTING_VSYNC],
                      sPlatformSettings[PLATFORM_SETTING_BORDER],
                      sPlatformSettings[PLATFORM_SETTING_VOLUME]);
    if (offset < 0 || (size_t)offset >= sizeof(contents))
        return;
    offset += snprintf(contents + offset, sizeof(contents) - offset,
                       "fastForwardSpeed=%u\n", sFastForwardSpeed);
    for (i = 0; i < PLATFORM_INPUT_ACTION_COUNT; i++)
    {
        Platform_InputBindingToString(&sKeyboardBindings[i], bindingText, sizeof(bindingText));
        if (offset < 0 || (size_t)offset >= sizeof(contents))
            return;
        offset += snprintf(contents + offset, sizeof(contents) - offset,
                           "input.%s=%s\n", Platform_InputActionConfigName(i), bindingText);
    }
    length = offset;
    if (length < 0 || (size_t)length >= sizeof(contents))
        return;
    if (!Platform_StorageWriteAtomic(preferredPath, contents, (u32)length)
     && Platform_StorageUsesPreferredRoot())
        Platform_StorageWriteAtomic(Platform_StorageGetLegacyConfigPath(), contents, (u32)length);
}

void Platform_ConfigGetKeyboardBinding(enum PlatformInputAction action, struct PlatformInputBinding *binding)
{
    if (binding == NULL)
        return;
    if (action < PLATFORM_INPUT_ACTION_COUNT)
        *binding = sKeyboardBindings[action];
    else
        binding->key = PLATFORM_INPUT_KEY_NONE, binding->modifiers = 0;
}

void Platform_ConfigSetKeyboardBinding(enum PlatformInputAction action, const struct PlatformInputBinding *binding)
{
    if (action < PLATFORM_INPUT_ACTION_COUNT && binding != NULL
     && Platform_InputBindingIsUsable(binding))
        sKeyboardBindings[action] = *binding;
}

void Platform_ConfigResetInputDefaults(void)
{
    int i;
    for (i = 0; i < PLATFORM_INPUT_ACTION_COUNT; i++)
        sKeyboardBindings[i] = Platform_InputGetDefaultBinding(i);
    sFastForwardSpeed = 5;
}

u8 Platform_ConfigGetFastForwardSpeed(void)
{
    return sFastForwardSpeed;
}

void Platform_ConfigSetFastForwardSpeed(u8 speed)
{
    if (speed == 0 || (speed >= 2 && speed <= 5))
        sFastForwardSpeed = speed;
}

u8 Platform_GetSetting(enum PlatformSetting setting)
{
    if (setting >= PLATFORM_SETTING_COUNT)
        return 0;
    return sPlatformSettings[setting];
}

void Platform_ConfigSetSetting(enum PlatformSetting setting, u8 value)
{
    if (setting < PLATFORM_SETTING_COUNT)
        sPlatformSettings[setting] = value;
}

#endif
