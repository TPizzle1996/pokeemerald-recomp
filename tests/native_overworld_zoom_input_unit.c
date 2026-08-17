#include <assert.h>
#include <string.h>

#include "../src/platform/desktop_input.c"

static void ResetActions(struct PlatformInputActions *actions)
{
    memset(actions, 0, sizeof(*actions));
}

int main(void)
{
    struct PlatformInputActions actions;

    // SDL places 0 after 1-9 in its scancode enum; every host digit must still
    // map to the corresponding stable input key.
    assert(Platform_InputKeyFromScancode(SDL_SCANCODE_0) == PLATFORM_INPUT_KEY_0);
    assert(Platform_InputKeyFromScancode(SDL_SCANCODE_1) == PLATFORM_INPUT_KEY_1);
    assert(Platform_InputKeyFromScancode(SDL_SCANCODE_9) == PLATFORM_INPUT_KEY_9);

    ResetActions(&actions);
    assert(HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_MINUS,
                              PLATFORM_INPUT_MODIFIER_CTRL, FALSE));
    assert(actions.zoomOut && !actions.zoomIn && !actions.zoomReset);

    ResetActions(&actions);
    assert(HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_EQUALS,
                              PLATFORM_INPUT_MODIFIER_CTRL, FALSE));
    assert(actions.zoomIn && !actions.zoomOut && !actions.zoomReset);

    // Ctrl+Plus is the same physical key with Shift held.
    ResetActions(&actions);
    assert(HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_EQUALS,
                              PLATFORM_INPUT_MODIFIER_CTRL
                            | PLATFORM_INPUT_MODIFIER_SHIFT, FALSE));
    assert(actions.zoomIn);

    ResetActions(&actions);
    assert(HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_0,
                              PLATFORM_INPUT_MODIFIER_CTRL, FALSE));
    assert(actions.zoomReset && !actions.zoomIn && !actions.zoomOut);

    // SDL repeats are consumed so they cannot leak into GBA input, but do not
    // issue another zoom action until a fresh physical key press.
    ResetActions(&actions);
    assert(HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_MINUS,
                              PLATFORM_INPUT_MODIFIER_CTRL, TRUE));
    assert(!actions.zoomOut && !actions.zoomIn && !actions.zoomReset);

    ResetActions(&actions);
    assert(!HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_MINUS, 0, FALSE));
    assert(!HandleZoomShortcut(&actions, PLATFORM_INPUT_KEY_MINUS,
                               PLATFORM_INPUT_MODIFIER_CTRL
                             | PLATFORM_INPUT_MODIFIER_ALT, FALSE));
    return 0;
}
