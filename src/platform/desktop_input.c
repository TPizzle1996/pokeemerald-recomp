#ifdef PLATFORM_SDL2

#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "global.h"
#include "platform/desktop_audio.h"
#include "platform/desktop_config.h"
#include "platform/desktop_input.h"

HOST_DATA static bool32 sKeyboardHeld[PLATFORM_INPUT_KEY_COUNT];
HOST_DATA static u16 sKeyboardKeys;
HOST_DATA static u16 sControllerKeys;
HOST_DATA static u16 sControllerAxisKeys;
HOST_DATA static Sint16 sControllerAxisX;
HOST_DATA static Sint16 sControllerAxisY;
HOST_DATA static bool32 sFastForward;
HOST_DATA static bool32 sToggleFastForward;
HOST_DATA static SDL_GameController *sController;

#ifdef __ANDROID__
extern void Platform_HandleTouchEvent(const SDL_TouchFingerEvent *event);
#endif

static const char *const sActionNames[PLATFORM_INPUT_ACTION_COUNT] =
{
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "A", "B", "Start", "Select", "L", "R",
    "Fast-forward Hold", "Fast-forward Toggle", "Pause", "Reset",
    "Quick Save", "Save State Manager", "Manual Save to Slot", "Quick Load",
};

static const char *const sActionConfigNames[PLATFORM_INPUT_ACTION_COUNT] =
{
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "a", "b", "start", "select", "l", "r",
    "fast_forward_hold", "fast_forward_toggle", "pause", "reset",
    "quick_save", "state_manager", "manual_save", "quick_load",
};

static const char *const sKeyNames[PLATFORM_INPUT_KEY_COUNT] =
{
    [PLATFORM_INPUT_KEY_NONE] = "None",
    [PLATFORM_INPUT_KEY_A] = "A", [PLATFORM_INPUT_KEY_B] = "B",
    [PLATFORM_INPUT_KEY_C] = "C", [PLATFORM_INPUT_KEY_D] = "D",
    [PLATFORM_INPUT_KEY_E] = "E", [PLATFORM_INPUT_KEY_F] = "F",
    [PLATFORM_INPUT_KEY_G] = "G", [PLATFORM_INPUT_KEY_H] = "H",
    [PLATFORM_INPUT_KEY_I] = "I", [PLATFORM_INPUT_KEY_J] = "J",
    [PLATFORM_INPUT_KEY_K] = "K", [PLATFORM_INPUT_KEY_L] = "L",
    [PLATFORM_INPUT_KEY_M] = "M", [PLATFORM_INPUT_KEY_N] = "N",
    [PLATFORM_INPUT_KEY_O] = "O", [PLATFORM_INPUT_KEY_P] = "P",
    [PLATFORM_INPUT_KEY_Q] = "Q", [PLATFORM_INPUT_KEY_R] = "R",
    [PLATFORM_INPUT_KEY_S] = "S", [PLATFORM_INPUT_KEY_T] = "T",
    [PLATFORM_INPUT_KEY_U] = "U", [PLATFORM_INPUT_KEY_V] = "V",
    [PLATFORM_INPUT_KEY_W] = "W", [PLATFORM_INPUT_KEY_X] = "X",
    [PLATFORM_INPUT_KEY_Y] = "Y", [PLATFORM_INPUT_KEY_Z] = "Z",
    [PLATFORM_INPUT_KEY_0] = "0", [PLATFORM_INPUT_KEY_1] = "1",
    [PLATFORM_INPUT_KEY_2] = "2", [PLATFORM_INPUT_KEY_3] = "3",
    [PLATFORM_INPUT_KEY_4] = "4", [PLATFORM_INPUT_KEY_5] = "5",
    [PLATFORM_INPUT_KEY_6] = "6", [PLATFORM_INPUT_KEY_7] = "7",
    [PLATFORM_INPUT_KEY_8] = "8", [PLATFORM_INPUT_KEY_9] = "9",
    [PLATFORM_INPUT_KEY_UP] = "Up", [PLATFORM_INPUT_KEY_DOWN] = "Down",
    [PLATFORM_INPUT_KEY_LEFT] = "Left", [PLATFORM_INPUT_KEY_RIGHT] = "Right",
    [PLATFORM_INPUT_KEY_RETURN] = "Return", [PLATFORM_INPUT_KEY_ESCAPE] = "Escape",
    [PLATFORM_INPUT_KEY_SPACE] = "Space", [PLATFORM_INPUT_KEY_TAB] = "Tab",
    [PLATFORM_INPUT_KEY_BACKSPACE] = "Backspace", [PLATFORM_INPUT_KEY_INSERT] = "Insert",
    [PLATFORM_INPUT_KEY_DELETE] = "Delete", [PLATFORM_INPUT_KEY_HOME] = "Home",
    [PLATFORM_INPUT_KEY_END] = "End", [PLATFORM_INPUT_KEY_PAGEUP] = "PageUp",
    [PLATFORM_INPUT_KEY_PAGEDOWN] = "PageDown", [PLATFORM_INPUT_KEY_F1] = "F1",
    [PLATFORM_INPUT_KEY_F2] = "F2", [PLATFORM_INPUT_KEY_F3] = "F3",
    [PLATFORM_INPUT_KEY_F4] = "F4", [PLATFORM_INPUT_KEY_F5] = "F5",
    [PLATFORM_INPUT_KEY_F6] = "F6", [PLATFORM_INPUT_KEY_F7] = "F7",
    [PLATFORM_INPUT_KEY_F8] = "F8", [PLATFORM_INPUT_KEY_F9] = "F9",
    [PLATFORM_INPUT_KEY_F10] = "F10", [PLATFORM_INPUT_KEY_F11] = "F11",
    [PLATFORM_INPUT_KEY_F12] = "F12", [PLATFORM_INPUT_KEY_MINUS] = "Minus",
    [PLATFORM_INPUT_KEY_EQUALS] = "Equals", [PLATFORM_INPUT_KEY_LEFTBRACKET] = "LeftBracket",
    [PLATFORM_INPUT_KEY_RIGHTBRACKET] = "RightBracket", [PLATFORM_INPUT_KEY_BACKSLASH] = "Backslash",
    [PLATFORM_INPUT_KEY_SEMICOLON] = "Semicolon", [PLATFORM_INPUT_KEY_APOSTROPHE] = "Apostrophe",
    [PLATFORM_INPUT_KEY_GRAVE] = "Grave", [PLATFORM_INPUT_KEY_COMMA] = "Comma",
    [PLATFORM_INPUT_KEY_PERIOD] = "Period", [PLATFORM_INPUT_KEY_SLASH] = "Slash",
};

static u8 ModifiersFromSdl(SDL_Keymod modifiers)
{
    u8 result = 0;
    if (modifiers & KMOD_SHIFT) result |= PLATFORM_INPUT_MODIFIER_SHIFT;
    if (modifiers & KMOD_CTRL)  result |= PLATFORM_INPUT_MODIFIER_CTRL;
    if (modifiers & KMOD_ALT)   result |= PLATFORM_INPUT_MODIFIER_ALT;
    if (modifiers & KMOD_GUI)   result |= PLATFORM_INPUT_MODIFIER_GUI;
    return result;
}

static u16 ControllerButtonMask(Uint8 button)
{
    switch (button)
    {
    case SDL_CONTROLLER_BUTTON_A:             return A_BUTTON;
    case SDL_CONTROLLER_BUTTON_B:             return B_BUTTON;
    case SDL_CONTROLLER_BUTTON_BACK:          return SELECT_BUTTON;
    case SDL_CONTROLLER_BUTTON_START:         return START_BUTTON;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return L_BUTTON;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return R_BUTTON;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return DPAD_RIGHT;
    default:                                  return 0;
    }
}

static void OpenController(int deviceIndex)
{
    if (sController == NULL && SDL_IsGameController(deviceIndex))
        sController = SDL_GameControllerOpen(deviceIndex);
}

static void CloseController(SDL_JoystickID instanceId)
{
    if (sController != NULL
     && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(sController)) == instanceId)
    {
        SDL_GameControllerClose(sController);
        sController = NULL;
        sControllerKeys = 0;
        sControllerAxisKeys = 0;
        sControllerAxisX = 0;
        sControllerAxisY = 0;
    }
}

static bool32 GetControllerSpeedUp(void)
{
    return sController != NULL
        && SDL_GameControllerGetAxis(sController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
}

static void RefreshKeyboardKeys(void)
{
    static const u16 masks[10] =
    {
        DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,
        A_BUTTON, B_BUTTON, START_BUTTON, SELECT_BUTTON, L_BUTTON, R_BUTTON,
    };
    int i;
    u8 modifiers = ModifiersFromSdl(SDL_GetModState());

    sKeyboardKeys = 0;
    for (i = 0; i < 10; i++)
    {
        struct PlatformInputBinding binding;
        Platform_ConfigGetKeyboardBinding(i, &binding);
        if (sKeyboardHeld[binding.key]
         && Platform_InputBindingMatches(&binding, binding.key, modifiers))
            sKeyboardKeys |= masks[i];
    }
}

#if defined(LINUX64) && LINUX64
static bool32 HandleZoomShortcut(struct PlatformInputActions *actions,
                                 enum PlatformInputKey key, u8 modifiers,
                                 bool32 repeat)
{
    if (!(modifiers & PLATFORM_INPUT_MODIFIER_CTRL)
     || (modifiers & (PLATFORM_INPUT_MODIFIER_ALT | PLATFORM_INPUT_MODIFIER_GUI)))
        return FALSE;
    if (key == PLATFORM_INPUT_KEY_MINUS)
    {
        if (!repeat)
            actions->zoomOut = TRUE;
        return TRUE;
    }
    if (key == PLATFORM_INPUT_KEY_EQUALS)
    {
        if (!repeat)
            actions->zoomIn = TRUE;
        return TRUE;
    }
    if (key == PLATFORM_INPUT_KEY_0)
    {
        if (!repeat)
            actions->zoomReset = TRUE;
        return TRUE;
    }
    return FALSE;
}
#endif

struct PlatformInputBinding Platform_InputGetDefaultBinding(enum PlatformInputAction action)
{
    struct PlatformInputBinding binding = {PLATFORM_INPUT_KEY_NONE, 0};
    switch (action)
    {
    case PLATFORM_INPUT_ACTION_DPAD_UP:              binding.key = PLATFORM_INPUT_KEY_UP; break;
    case PLATFORM_INPUT_ACTION_DPAD_DOWN:            binding.key = PLATFORM_INPUT_KEY_DOWN; break;
    case PLATFORM_INPUT_ACTION_DPAD_LEFT:            binding.key = PLATFORM_INPUT_KEY_LEFT; break;
    case PLATFORM_INPUT_ACTION_DPAD_RIGHT:           binding.key = PLATFORM_INPUT_KEY_RIGHT; break;
    case PLATFORM_INPUT_ACTION_A:                    binding.key = PLATFORM_INPUT_KEY_Z; break;
    case PLATFORM_INPUT_ACTION_B:                    binding.key = PLATFORM_INPUT_KEY_X; break;
    case PLATFORM_INPUT_ACTION_START:                binding.key = PLATFORM_INPUT_KEY_RETURN; break;
    case PLATFORM_INPUT_ACTION_SELECT:               binding.key = PLATFORM_INPUT_KEY_BACKSLASH; break;
    case PLATFORM_INPUT_ACTION_L:                    binding.key = PLATFORM_INPUT_KEY_A; break;
    case PLATFORM_INPUT_ACTION_R:                    binding.key = PLATFORM_INPUT_KEY_S; break;
    case PLATFORM_INPUT_ACTION_FAST_FORWARD_HOLD:    binding.key = PLATFORM_INPUT_KEY_SPACE; break;
    case PLATFORM_INPUT_ACTION_FAST_FORWARD_TOGGLE:  binding.key = PLATFORM_INPUT_KEY_SPACE; binding.modifiers = PLATFORM_INPUT_MODIFIER_SHIFT; break;
    case PLATFORM_INPUT_ACTION_PAUSE:                binding.key = PLATFORM_INPUT_KEY_P; binding.modifiers = PLATFORM_INPUT_MODIFIER_CTRL; break;
    case PLATFORM_INPUT_ACTION_RESET:                binding.key = PLATFORM_INPUT_KEY_R; binding.modifiers = PLATFORM_INPUT_MODIFIER_CTRL; break;
    case PLATFORM_INPUT_ACTION_QUICK_SAVE:           binding.key = PLATFORM_INPUT_KEY_F5; break;
    case PLATFORM_INPUT_ACTION_OPEN_STATE_MANAGER:   binding.key = PLATFORM_INPUT_KEY_F6; break;
    case PLATFORM_INPUT_ACTION_MANUAL_SAVE:          binding.key = PLATFORM_INPUT_KEY_F7; break;
    case PLATFORM_INPUT_ACTION_QUICK_LOAD:           binding.key = PLATFORM_INPUT_KEY_F9; break;
    default: break;
    }
    return binding;
}

const char *Platform_InputActionName(enum PlatformInputAction action)
{
    return action < PLATFORM_INPUT_ACTION_COUNT ? sActionNames[action] : "Unknown";
}

const char *Platform_InputActionConfigName(enum PlatformInputAction action)
{
    return action < PLATFORM_INPUT_ACTION_COUNT ? sActionConfigNames[action] : "unknown";
}

const char *Platform_InputKeyName(enum PlatformInputKey key)
{
    return key < PLATFORM_INPUT_KEY_COUNT && sKeyNames[key] != NULL ? sKeyNames[key] : "None";
}

enum PlatformInputKey Platform_InputKeyFromName(const char *name)
{
    int i;
    if (name == NULL)
        return PLATFORM_INPUT_KEY_NONE;
    for (i = 1; i < PLATFORM_INPUT_KEY_COUNT; i++)
    {
        if (strcmp(name, sKeyNames[i]) == 0)
            return i;
    }
    return PLATFORM_INPUT_KEY_NONE;
}

enum PlatformInputKey Platform_InputKeyFromScancode(int scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return PLATFORM_INPUT_KEY_A + scancode - SDL_SCANCODE_A;
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return PLATFORM_INPUT_KEY_1 + scancode - SDL_SCANCODE_1;
    if (scancode == SDL_SCANCODE_0)
        return PLATFORM_INPUT_KEY_0;
    switch (scancode)
    {
    case SDL_SCANCODE_UP: return PLATFORM_INPUT_KEY_UP;
    case SDL_SCANCODE_DOWN: return PLATFORM_INPUT_KEY_DOWN;
    case SDL_SCANCODE_LEFT: return PLATFORM_INPUT_KEY_LEFT;
    case SDL_SCANCODE_RIGHT: return PLATFORM_INPUT_KEY_RIGHT;
    case SDL_SCANCODE_RETURN: return PLATFORM_INPUT_KEY_RETURN;
    case SDL_SCANCODE_ESCAPE: return PLATFORM_INPUT_KEY_ESCAPE;
    case SDL_SCANCODE_SPACE: return PLATFORM_INPUT_KEY_SPACE;
    case SDL_SCANCODE_TAB: return PLATFORM_INPUT_KEY_TAB;
    case SDL_SCANCODE_BACKSPACE: return PLATFORM_INPUT_KEY_BACKSPACE;
    case SDL_SCANCODE_INSERT: return PLATFORM_INPUT_KEY_INSERT;
    case SDL_SCANCODE_DELETE: return PLATFORM_INPUT_KEY_DELETE;
    case SDL_SCANCODE_HOME: return PLATFORM_INPUT_KEY_HOME;
    case SDL_SCANCODE_END: return PLATFORM_INPUT_KEY_END;
    case SDL_SCANCODE_PAGEUP: return PLATFORM_INPUT_KEY_PAGEUP;
    case SDL_SCANCODE_PAGEDOWN: return PLATFORM_INPUT_KEY_PAGEDOWN;
    case SDL_SCANCODE_F1: return PLATFORM_INPUT_KEY_F1;
    case SDL_SCANCODE_F2: return PLATFORM_INPUT_KEY_F2;
    case SDL_SCANCODE_F3: return PLATFORM_INPUT_KEY_F3;
    case SDL_SCANCODE_F4: return PLATFORM_INPUT_KEY_F4;
    case SDL_SCANCODE_F5: return PLATFORM_INPUT_KEY_F5;
    case SDL_SCANCODE_F6: return PLATFORM_INPUT_KEY_F6;
    case SDL_SCANCODE_F7: return PLATFORM_INPUT_KEY_F7;
    case SDL_SCANCODE_F8: return PLATFORM_INPUT_KEY_F8;
    case SDL_SCANCODE_F9: return PLATFORM_INPUT_KEY_F9;
    case SDL_SCANCODE_F10: return PLATFORM_INPUT_KEY_F10;
    case SDL_SCANCODE_F11: return PLATFORM_INPUT_KEY_F11;
    case SDL_SCANCODE_F12: return PLATFORM_INPUT_KEY_F12;
    case SDL_SCANCODE_MINUS: return PLATFORM_INPUT_KEY_MINUS;
    case SDL_SCANCODE_EQUALS: return PLATFORM_INPUT_KEY_EQUALS;
    case SDL_SCANCODE_LEFTBRACKET: return PLATFORM_INPUT_KEY_LEFTBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET: return PLATFORM_INPUT_KEY_RIGHTBRACKET;
    case SDL_SCANCODE_BACKSLASH: return PLATFORM_INPUT_KEY_BACKSLASH;
    case SDL_SCANCODE_SEMICOLON: return PLATFORM_INPUT_KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return PLATFORM_INPUT_KEY_APOSTROPHE;
    case SDL_SCANCODE_GRAVE: return PLATFORM_INPUT_KEY_GRAVE;
    case SDL_SCANCODE_COMMA: return PLATFORM_INPUT_KEY_COMMA;
    case SDL_SCANCODE_PERIOD: return PLATFORM_INPUT_KEY_PERIOD;
    case SDL_SCANCODE_SLASH: return PLATFORM_INPUT_KEY_SLASH;
    default: return PLATFORM_INPUT_KEY_NONE;
    }
}

void Platform_InputBindingToString(const struct PlatformInputBinding *binding, char *dest, u32 destSize)
{
    int offset = 0;
    if (dest == NULL || destSize == 0)
        return;
    dest[0] = '\0';
    if (binding == NULL || !Platform_InputBindingIsUsable(binding))
    {
        snprintf(dest, destSize, "Unbound");
        return;
    }
    if (binding->modifiers & PLATFORM_INPUT_MODIFIER_SHIFT)
        offset += snprintf(dest + offset, destSize - offset, "Shift+");
    if (binding->modifiers & PLATFORM_INPUT_MODIFIER_CTRL)
        offset += snprintf(dest + offset, destSize - offset, "Ctrl+");
    if (binding->modifiers & PLATFORM_INPUT_MODIFIER_ALT)
        offset += snprintf(dest + offset, destSize - offset, "Alt+");
    if (binding->modifiers & PLATFORM_INPUT_MODIFIER_GUI)
        offset += snprintf(dest + offset, destSize - offset, "Gui+");
    if (offset < 0 || (u32)offset >= destSize)
        return;
    snprintf(dest + offset, destSize - offset, "%s", Platform_InputKeyName(binding->key));
}

bool32 Platform_InputBindingIsUsable(const struct PlatformInputBinding *binding)
{
    return binding != NULL && binding->key > PLATFORM_INPUT_KEY_NONE
        && binding->key < PLATFORM_INPUT_KEY_COUNT;
}

bool32 Platform_InputBindingMatches(const struct PlatformInputBinding *binding,
                                    enum PlatformInputKey key, u8 modifiers)
{
    return Platform_InputBindingIsUsable(binding)
        && binding->key == key
        && binding->modifiers == modifiers;
}

void Platform_InputInit(void)
{
    int i;
    memset(sKeyboardHeld, 0, sizeof(sKeyboardHeld));
    sKeyboardKeys = 0;
    sControllerKeys = 0;
    sControllerAxisKeys = 0;
    sControllerAxisX = 0;
    sControllerAxisY = 0;
    sFastForward = FALSE;
    sToggleFastForward = FALSE;
    sController = NULL;
    for (i = 0; i < SDL_NumJoysticks() && sController == NULL; i++)
        OpenController(i);
}

void Platform_InputPoll(struct PlatformInputActions *actions)
{
    SDL_Event event;
    bool32 oldFastForward = sFastForward;

    memset(actions, 0, sizeof(*actions));
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            actions->quit = TRUE;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            OpenController(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            CloseController(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            sControllerKeys |= ControllerButtonMask(event.cbutton.button);
            break;
        case SDL_CONTROLLERBUTTONUP:
            sControllerKeys &= ~ControllerButtonMask(event.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                sControllerAxisX = event.caxis.value;
            else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                sControllerAxisY = event.caxis.value;
            sControllerAxisKeys = 0;
            if (sControllerAxisX < -16000) sControllerAxisKeys |= DPAD_LEFT;
            if (sControllerAxisX >  16000) sControllerAxisKeys |= DPAD_RIGHT;
            if (sControllerAxisY < -16000) sControllerAxisKeys |= DPAD_UP;
            if (sControllerAxisY >  16000) sControllerAxisKeys |= DPAD_DOWN;
            break;
        case SDL_KEYDOWN:
        {
            enum PlatformInputKey key = Platform_InputKeyFromScancode(event.key.keysym.scancode);
            u8 modifiers = ModifiersFromSdl(event.key.keysym.mod);
#if defined(LINUX64) && LINUX64
            if (modifiers & PLATFORM_INPUT_MODIFIER_CTRL)
            {
                fprintf(stderr,
                        "zoom input: scancode=%d keycode=%d ctrl=%d shift=%d repeat=%d\n",
                        event.key.keysym.scancode, event.key.keysym.sym,
                        !!(modifiers & PLATFORM_INPUT_MODIFIER_CTRL),
                        !!(modifiers & PLATFORM_INPUT_MODIFIER_SHIFT),
                        event.key.repeat);
                fflush(stderr);
            }
            // Native zoom shortcuts are consumed before remappable GBA input.
            // Repeated keydown events are consumed but do not cycle the level.
            if (HandleZoomShortcut(actions, key, modifiers, event.key.repeat))
            {
                fprintf(stderr, "zoom shortcut consumed\n");
                fflush(stderr);
                if (key != PLATFORM_INPUT_KEY_NONE)
                    sKeyboardHeld[key] = FALSE;
                break;
            }
#endif
            if (key != PLATFORM_INPUT_KEY_NONE)
                sKeyboardHeld[key] = TRUE;
            if (event.key.repeat)
                break;
            {
                struct PlatformInputBinding binding;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_RESET, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->reset = TRUE;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_PAUSE, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->togglePause = TRUE;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_FAST_FORWARD_TOGGLE, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) sToggleFastForward = !sToggleFastForward;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_QUICK_SAVE, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->quickSave = TRUE;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_OPEN_STATE_MANAGER, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->openStateManager = TRUE;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_MANUAL_SAVE, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->manualSave = TRUE;
                Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_QUICK_LOAD, &binding);
                if (Platform_InputBindingMatches(&binding, key, modifiers)) actions->quickLoad = TRUE;
#if defined(NATIVE_LINUX) || defined(WINDOWS64)
                /* Temporary native-only developer/testing shortcut. SDL's
                 * non-repeat keydown event makes this one grant per press. */
                if (key == PLATFORM_INPUT_KEY_F8 && modifiers == 0)
                    actions->debugAddRareCandies = TRUE;
#endif
            }
            if (event.key.keysym.scancode == SDL_SCANCODE_F10 && modifiers == 0)
                actions->openSettings = TRUE;
            break;
        }
        case SDL_KEYUP:
        {
            enum PlatformInputKey key = Platform_InputKeyFromScancode(event.key.keysym.scancode);
            if (key != PLATFORM_INPUT_KEY_NONE)
                sKeyboardHeld[key] = FALSE;
            break;
        }
#ifdef __ANDROID__
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP:
            Platform_HandleTouchEvent(&event.tfinger);
            break;
#endif
        default:
            break;
        }
    }

    {
        struct PlatformInputBinding binding;
        u8 modifiers = ModifiersFromSdl(SDL_GetModState());
        Platform_ConfigGetKeyboardBinding(PLATFORM_INPUT_ACTION_FAST_FORWARD_HOLD, &binding);
        sFastForward = sKeyboardHeld[binding.key]
                    && Platform_InputBindingMatches(&binding, binding.key, modifiers);
    }
    sFastForward = sFastForward || GetControllerSpeedUp();
    sFastForward = sFastForward || sToggleFastForward;
    RefreshKeyboardKeys();
    if (sFastForward != oldFastForward)
    {
        actions->speedUpChanged = TRUE;
        actions->speedUp = sFastForward;
        actions->speed = Platform_ConfigGetFastForwardSpeed();
    }
}

u16 Platform_InputGetKeys(void)
{
    return sKeyboardKeys | sControllerKeys | sControllerAxisKeys;
}

void Platform_InputClearState(void)
{
    memset(sKeyboardHeld, 0, sizeof(sKeyboardHeld));
    sKeyboardKeys = 0;
    sToggleFastForward = FALSE;
    sFastForward = FALSE;
}

void Platform_InputShutdown(void)
{
    if (sController != NULL)
    {
        SDL_GameControllerClose(sController);
        sController = NULL;
    }
}

#endif
