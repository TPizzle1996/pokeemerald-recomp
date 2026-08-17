#ifndef GUARD_PLATFORM_DESKTOP_INPUT_H
#define GUARD_PLATFORM_DESKTOP_INPUT_H

#include "gba/types.h"

/* Stable host-facing identifiers. SDL scancodes are translated at the event
 * boundary and are never written to the configuration file. */
enum PlatformInputKey
{
    PLATFORM_INPUT_KEY_NONE = 0,
    PLATFORM_INPUT_KEY_A, PLATFORM_INPUT_KEY_B, PLATFORM_INPUT_KEY_C,
    PLATFORM_INPUT_KEY_D, PLATFORM_INPUT_KEY_E, PLATFORM_INPUT_KEY_F,
    PLATFORM_INPUT_KEY_G, PLATFORM_INPUT_KEY_H, PLATFORM_INPUT_KEY_I,
    PLATFORM_INPUT_KEY_J, PLATFORM_INPUT_KEY_K, PLATFORM_INPUT_KEY_L,
    PLATFORM_INPUT_KEY_M, PLATFORM_INPUT_KEY_N, PLATFORM_INPUT_KEY_O,
    PLATFORM_INPUT_KEY_P, PLATFORM_INPUT_KEY_Q, PLATFORM_INPUT_KEY_R,
    PLATFORM_INPUT_KEY_S, PLATFORM_INPUT_KEY_T, PLATFORM_INPUT_KEY_U,
    PLATFORM_INPUT_KEY_V, PLATFORM_INPUT_KEY_W, PLATFORM_INPUT_KEY_X,
    PLATFORM_INPUT_KEY_Y, PLATFORM_INPUT_KEY_Z,
    PLATFORM_INPUT_KEY_0, PLATFORM_INPUT_KEY_1, PLATFORM_INPUT_KEY_2,
    PLATFORM_INPUT_KEY_3, PLATFORM_INPUT_KEY_4, PLATFORM_INPUT_KEY_5,
    PLATFORM_INPUT_KEY_6, PLATFORM_INPUT_KEY_7, PLATFORM_INPUT_KEY_8,
    PLATFORM_INPUT_KEY_9,
    PLATFORM_INPUT_KEY_UP, PLATFORM_INPUT_KEY_DOWN, PLATFORM_INPUT_KEY_LEFT,
    PLATFORM_INPUT_KEY_RIGHT, PLATFORM_INPUT_KEY_RETURN,
    PLATFORM_INPUT_KEY_ESCAPE, PLATFORM_INPUT_KEY_SPACE, PLATFORM_INPUT_KEY_TAB,
    PLATFORM_INPUT_KEY_BACKSPACE, PLATFORM_INPUT_KEY_INSERT,
    PLATFORM_INPUT_KEY_DELETE, PLATFORM_INPUT_KEY_HOME, PLATFORM_INPUT_KEY_END,
    PLATFORM_INPUT_KEY_PAGEUP, PLATFORM_INPUT_KEY_PAGEDOWN,
    PLATFORM_INPUT_KEY_F1, PLATFORM_INPUT_KEY_F2, PLATFORM_INPUT_KEY_F3,
    PLATFORM_INPUT_KEY_F4, PLATFORM_INPUT_KEY_F5, PLATFORM_INPUT_KEY_F6,
    PLATFORM_INPUT_KEY_F7, PLATFORM_INPUT_KEY_F8, PLATFORM_INPUT_KEY_F9,
    PLATFORM_INPUT_KEY_F10, PLATFORM_INPUT_KEY_F11, PLATFORM_INPUT_KEY_F12,
    PLATFORM_INPUT_KEY_MINUS, PLATFORM_INPUT_KEY_EQUALS,
    PLATFORM_INPUT_KEY_LEFTBRACKET, PLATFORM_INPUT_KEY_RIGHTBRACKET,
    PLATFORM_INPUT_KEY_BACKSLASH, PLATFORM_INPUT_KEY_SEMICOLON,
    PLATFORM_INPUT_KEY_APOSTROPHE, PLATFORM_INPUT_KEY_GRAVE,
    PLATFORM_INPUT_KEY_COMMA, PLATFORM_INPUT_KEY_PERIOD,
    PLATFORM_INPUT_KEY_SLASH, PLATFORM_INPUT_KEY_COUNT
};

enum PlatformInputModifier
{
    PLATFORM_INPUT_MODIFIER_SHIFT = 1 << 0,
    PLATFORM_INPUT_MODIFIER_CTRL  = 1 << 1,
    PLATFORM_INPUT_MODIFIER_ALT   = 1 << 2,
    PLATFORM_INPUT_MODIFIER_GUI   = 1 << 3,
};

enum PlatformInputAction
{
    PLATFORM_INPUT_ACTION_DPAD_UP = 0,
    PLATFORM_INPUT_ACTION_DPAD_DOWN,
    PLATFORM_INPUT_ACTION_DPAD_LEFT,
    PLATFORM_INPUT_ACTION_DPAD_RIGHT,
    PLATFORM_INPUT_ACTION_A,
    PLATFORM_INPUT_ACTION_B,
    PLATFORM_INPUT_ACTION_START,
    PLATFORM_INPUT_ACTION_SELECT,
    PLATFORM_INPUT_ACTION_L,
    PLATFORM_INPUT_ACTION_R,
    PLATFORM_INPUT_ACTION_FAST_FORWARD_HOLD,
    PLATFORM_INPUT_ACTION_FAST_FORWARD_TOGGLE,
    PLATFORM_INPUT_ACTION_PAUSE,
    PLATFORM_INPUT_ACTION_RESET,
    PLATFORM_INPUT_ACTION_QUICK_SAVE,
    PLATFORM_INPUT_ACTION_OPEN_STATE_MANAGER,
    PLATFORM_INPUT_ACTION_MANUAL_SAVE,
    PLATFORM_INPUT_ACTION_QUICK_LOAD,
    PLATFORM_INPUT_ACTION_COUNT
};

struct PlatformInputBinding
{
    enum PlatformInputKey key;
    u8 modifiers;
};

struct PlatformInputActions
{
    bool32 quit;
    bool32 reset;
    bool32 togglePause;
    bool32 openSettings;
    bool32 quickSave;
    bool32 openStateManager;
    bool32 manualSave;
    bool32 quickLoad;
#if defined(NATIVE_LINUX) || defined(WINDOWS64)
    /* Temporary native-only developer/testing shortcut. */
    bool32 debugAddRareCandies;
#endif
#if defined(LINUX64) && LINUX64
    bool32 zoomIn;
    bool32 zoomOut;
    bool32 zoomReset;
#endif
    bool32 speedUpChanged;
    bool32 speedUp;
    u8 speed;
};

void Platform_InputInit(void);
void Platform_InputPoll(struct PlatformInputActions *actions);
u16 Platform_InputGetKeys(void);
struct PlatformInputBinding Platform_InputGetDefaultBinding(enum PlatformInputAction action);
const char *Platform_InputActionName(enum PlatformInputAction action);
const char *Platform_InputActionConfigName(enum PlatformInputAction action);
const char *Platform_InputKeyName(enum PlatformInputKey key);
enum PlatformInputKey Platform_InputKeyFromName(const char *name);
enum PlatformInputKey Platform_InputKeyFromScancode(int scancode);
void Platform_InputBindingToString(const struct PlatformInputBinding *binding, char *dest, u32 destSize);
bool32 Platform_InputBindingIsUsable(const struct PlatformInputBinding *binding);
bool32 Platform_InputBindingMatches(const struct PlatformInputBinding *binding,
                                    enum PlatformInputKey key, u8 modifiers);
void Platform_InputClearState(void);
void Platform_InputShutdown(void);

#endif
