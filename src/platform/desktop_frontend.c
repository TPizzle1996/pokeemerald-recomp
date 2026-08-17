#ifdef PLATFORM_SDL2

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
#include <SDL2/SDL_image.h>
#endif

#include "global.h"
#include "platform.h"
#include "platform/desktop_config.h"
#include "platform/desktop_assets.h"
#include "platform/desktop_frontend.h"
#include "platform/desktop_file_dialog.h"
#include "platform/desktop_game_content.h"
#include "platform/desktop_input.h"
#include "platform/desktop_profiles.h"
#include "platform/desktop_video.h"

#define UI_WIDTH 960
#define UI_HEIGHT 540
#define PROFILE_ROWS_VISIBLE 5
#define SETTINGS_ROWS_VISIBLE 18

enum LauncherFocus
{
    LAUNCHER_FOCUS_PROFILES,
    LAUNCHER_FOCUS_ACTIONS,
};

enum LauncherAction
{
    LAUNCHER_ACTION_PLAY,
    LAUNCHER_ACTION_NEW_PROFILE,
    LAUNCHER_ACTION_DELETE_PROFILE,
    LAUNCHER_ACTION_SETTINGS,
    LAUNCHER_ACTION_QUIT,
    LAUNCHER_ACTION_COUNT,
};

enum SettingsPage
{
    SETTINGS_PAGE_CONTROLS,
    SETTINGS_PAGE_DESKTOP,
    SETTINGS_PAGE_GAME_DATA,
    SETTINGS_PAGE_COUNT,
};

static SDL_Texture *sRayquazaTexture;

static const u8 sLetters[26][7] =
{
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30}, {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14}, {17,17,17,31,17,17,17}, {14,4,4,4,4,4,14},
    {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16}, {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4}, {17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};

static const u8 sDigits[10][7] =
{
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
};

static u8 GlyphRow(char character, int row)
{
    if (character >= 'a' && character <= 'z') character = (char)toupper((unsigned char)character);
    if (character >= 'A' && character <= 'Z') return sLetters[character - 'A'][row];
    if (character >= '0' && character <= '9') return sDigits[character - '0'][row];
    switch (character)
    {
    case ' ': return 0;
    case '-': return row == 3 ? 14 : 0;
    case '+': return row == 3 ? 14 : (row == 1 || row == 2 || row == 4 || row == 5 ? 4 : 0);
    case ':': return row == 2 || row == 5 ? 4 : 0;
    case '.': return row == 6 ? 4 : 0;
    case ',': return row == 6 ? 8 : row == 5 ? 4 : 0;
    case '\'': return row == 0 || row == 1 ? 4 : 0;
    case '/': return row == 0 || row == 6 ? 1 : (1 << (6 - row));
    case '_': return row == 6 ? 31 : 0;
    case '>': return row == 3 ? 16 : (row == 2 || row == 4 ? 8 : (row == 1 || row == 5 ? 4 : 0));
    case '<': return row == 3 ? 1 : (row == 2 || row == 4 ? 2 : (row == 1 || row == 5 ? 4 : 0));
    case '?': return row == 0 ? 14 : row == 1 ? 17 : row == 2 ? 1 : row == 3 ? 2 : row == 6 ? 4 : 0;
    case '!': return row < 5 ? 4 : row == 6 ? 4 : 0;
    case '[': return row == 0 || row == 6 ? 31 : 16;
    case ']': return row == 0 || row == 6 ? 31 : 1;
    case '(': return row == 0 || row == 6 ? 2 : 4;
    case ')': return row == 0 || row == 6 ? 8 : 4;
    case '=': return row == 2 || row == 4 ? 31 : 0;
    default: return 0;
    }
}

static void SetColor(SDL_Color color)
{
    SDL_SetRenderDrawColor(sdlRenderer, color.r, color.g, color.b, color.a);
}

static void FillRect(int x, int y, int w, int h, SDL_Color color)
{
    SDL_Rect rect = {x, y, w, h};
    SetColor(color);
    SDL_RenderFillRect(sdlRenderer, &rect);
}

static void StrokeRect(int x, int y, int w, int h, SDL_Color color)
{
    SDL_Rect rect = {x, y, w, h};
    SetColor(color);
    SDL_RenderDrawRect(sdlRenderer, &rect);
}

static int TextWidth(const char *text, int scale)
{
    int length = text != NULL ? strlen(text) : 0;
    return length == 0 ? 0 : length * 6 * scale - scale;
}

static void DrawTextSized(int x, int y, const char *text, int scale, SDL_Color color)
{
    int i;
    SetColor(color);
    for (i = 0; text != NULL && text[i] != '\0'; i++)
    {
        int row;
        for (row = 0; row < 7; row++)
        {
            int column;
            u8 bits = GlyphRow(text[i], row);
            for (column = 0; column < 5; column++)
            {
                if (bits & (1 << (4 - column)))
                {
                    SDL_Rect pixel = {x + (i * 6 + column) * scale, y + row * scale,
                                      scale, scale};
                    SDL_RenderFillRect(sdlRenderer, &pixel);
                }
            }
        }
    }
}

static void DrawTextOutlined(int x, int y, const char *text, int scale,
                             SDL_Color color, SDL_Color outline, int weight)
{
    DrawTextSized(x - weight, y, text, scale, outline);
    DrawTextSized(x + weight, y, text, scale, outline);
    DrawTextSized(x, y - weight, text, scale, outline);
    DrawTextSized(x, y + weight, text, scale, outline);
    DrawTextSized(x - weight, y - weight, text, scale, outline);
    DrawTextSized(x + weight, y + weight, text, scale, outline);
    DrawTextSized(x - weight, y + weight, text, scale, outline);
    DrawTextSized(x + weight, y - weight, text, scale, outline);
    DrawTextSized(x, y, text, scale, color);
}

static void DrawCenteredText(int x, int y, int width, const char *text, int scale,
                             SDL_Color color)
{
    DrawTextSized(x + (width - TextWidth(text, scale)) / 2, y, text, scale, color);
}

static void DrawHexagon(int centerX, int centerY, int radius, SDL_Color color)
{
    SDL_Point points[7] =
    {
        {centerX - radius / 2, centerY - radius},
        {centerX + radius / 2, centerY - radius},
        {centerX + radius, centerY},
        {centerX + radius / 2, centerY + radius},
        {centerX - radius / 2, centerY + radius},
        {centerX - radius, centerY},
        {centerX - radius / 2, centerY - radius},
    };
    SetColor(color);
    SDL_RenderDrawLines(sdlRenderer, points, 7);
}

static void DrawBackground(void)
{
    int i;
    for (i = 0; i < 18; i++)
    {
        SDL_Color band = {(u8)(5 + i / 3), (u8)(20 + i * 2), (u8)(29 + i * 2), 255};
        FillRect(0, i * 30, UI_WIDTH, 30, band);
    }

    for (i = -40; i < UI_WIDTH; i += 96)
    {
        SetColor((SDL_Color){13, 72, 63, 255});
        SDL_RenderDrawLine(sdlRenderer, i, 0, i + 260, UI_HEIGHT);
        SDL_RenderDrawLine(sdlRenderer, i + 26, 0, i + 286, UI_HEIGHT);
    }
    for (i = 0; i < 6; i++)
        DrawHexagon(92 + i * 174, 100 + (i % 2) * 238, 32, (SDL_Color){16, 78, 68, 255});

    FillRect(0, 0, UI_WIDTH, 8, (SDL_Color){2, 10, 19, 255});
    FillRect(0, UI_HEIGHT - 8, UI_WIDTH, 8, (SDL_Color){2, 10, 19, 255});
    FillRect(0, 0, 8, UI_HEIGHT, (SDL_Color){2, 10, 19, 255});
    FillRect(UI_WIDTH - 8, 0, 8, UI_HEIGHT, (SDL_Color){2, 10, 19, 255});
    StrokeRect(9, 9, UI_WIDTH - 19, UI_HEIGHT - 19, (SDL_Color){50, 193, 137, 255});
    StrokeRect(12, 12, UI_WIDTH - 25, UI_HEIGHT - 25, (SDL_Color){197, 158, 49, 255});
}

static void DrawPanel(int x, int y, int width, int height)
{
    FillRect(x, y, width, height, (SDL_Color){2, 9, 17, 255});
    FillRect(x + 3, y + 3, width - 6, height - 6, (SDL_Color){192, 147, 39, 255});
    FillRect(x + 5, y + 5, width - 10, height - 10, (SDL_Color){35, 181, 126, 255});
    FillRect(x + 8, y + 8, width - 16, height - 16, (SDL_Color){8, 38, 43, 255});
    StrokeRect(x + 11, y + 11, width - 22, height - 22, (SDL_Color){94, 226, 177, 255});
}

static void DrawLogo(int x, int y, bool32 compact)
{
    int pokemonScale = compact ? 2 : 3;
    int emeraldScale = compact ? 3 : 6;
    int emeraldY = y + (compact ? 22 : 34);
    int recompY = emeraldY + (compact ? 29 : 52);

    DrawTextOutlined(x + 3, y + 4, "POKEMON", pokemonScale,
                     (SDL_Color){252, 214, 72, 255}, (SDL_Color){5, 18, 39, 255}, compact ? 1 : 2);
    DrawTextOutlined(x, emeraldY, "EMERALD", emeraldScale,
                     (SDL_Color){72, 235, 164, 255}, (SDL_Color){3, 18, 28, 255}, compact ? 1 : 2);
    DrawTextSized(x + (compact ? 3 : 8), recompY, "[Recomp]", compact ? 1 : 2,
                  (SDL_Color){245, 242, 204, 255});
    FillRect(x, recompY + (compact ? 11 : 20), compact ? 122 : 250, compact ? 2 : 3,
             (SDL_Color){214, 166, 45, 255});
}

static void DrawRayquaza(int areaX, int areaY, int maxWidth, int maxHeight)
{
    int sourceWidth;
    int sourceHeight;
    int width;
    int height;
    SDL_Rect dest;

    DrawHexagon(areaX + maxWidth / 2, areaY + maxHeight / 2, maxHeight / 2 - 12,
                (SDL_Color){23, 103, 78, 255});
    DrawHexagon(areaX + maxWidth / 2, areaY + maxHeight / 2, maxHeight / 2 - 22,
                (SDL_Color){180, 139, 37, 255});
    if (sRayquazaTexture == NULL
     || SDL_QueryTexture(sRayquazaTexture, NULL, NULL, &sourceWidth, &sourceHeight) != 0)
        return;

    width = maxWidth;
    height = sourceHeight * width / sourceWidth;
    if (height > maxHeight)
    {
        height = maxHeight;
        width = sourceWidth * height / sourceHeight;
    }
    dest.x = areaX + (maxWidth - width) / 2;
    dest.y = areaY + (maxHeight - height) / 2;
    dest.w = width;
    dest.h = height;
    SDL_RenderCopy(sdlRenderer, sRayquazaTexture, NULL, &dest);
}

static void DrawLauncherChrome(void)
{
    DrawBackground();
    DrawLogo(42, 27, FALSE);
    FillRect(521, 27, 3, 486, (SDL_Color){5, 19, 26, 255});
    FillRect(525, 27, 2, 486, (SDL_Color){51, 191, 136, 255});
    DrawRayquaza(42, 139, 453, 376);
    DrawPanel(545, 22, 389, 496);
}

static void DrawButton(int x, int y, int width, int height, const char *label, bool32 active)
{
    SDL_Color fill = active ? (SDL_Color){26, 127, 82, 255} : (SDL_Color){10, 54, 53, 255};
    SDL_Color inner = active ? (SDL_Color){81, 225, 151, 255} : (SDL_Color){34, 111, 89, 255};
    SDL_Color text = active ? (SDL_Color){255, 235, 119, 255} : (SDL_Color){239, 244, 218, 255};

    FillRect(x, y, width, height, (SDL_Color){2, 12, 19, 255});
    FillRect(x + 2, y + 2, width - 4, height - 4, active ? (SDL_Color){212, 163, 44, 255} : inner);
    FillRect(x + 4, y + 4, width - 8, height - 8, fill);
    if (active)
    {
        FillRect(x + 8, y + 7, 5, height - 14, (SDL_Color){247, 101, 126, 255});
        DrawTextSized(x + 18, y + (height - 14) / 2, ">", 2, (SDL_Color){255, 235, 119, 255});
    }
    DrawCenteredText(x, y + (height - 14) / 2, width, label, 2, text);
}

static void DrawProfiles(u32 cursor, enum LauncherFocus focus, int actionCursor, const char *message)
{
    u32 count = Platform_ProfileCount();
    u32 first = 0;
    u32 i;
    const struct PlatformProfileMetadata *profile;
    static const char *const actions[LAUNCHER_ACTION_COUNT] =
    {
        "PLAY", "NEW PROFILE", "DELETE PROFILE", "SETTINGS", "QUIT"
    };

    DrawLauncherChrome();
    DrawTextOutlined(574, 48, "SELECT PROFILE", 3,
                     (SDL_Color){244, 242, 207, 255}, (SDL_Color){2, 13, 19, 255}, 1);
    FillRect(570, 77, 338, 3, (SDL_Color){214, 166, 45, 255});
    FillRect(570, 84, 338, 177, (SDL_Color){3, 22, 29, 255});
    StrokeRect(570, 84, 338, 177, (SDL_Color){41, 132, 104, 255});

    if (count > PROFILE_ROWS_VISIBLE && cursor + 1 > PROFILE_ROWS_VISIBLE)
        first = cursor + 1 - PROFILE_ROWS_VISIBLE;
    for (i = first; i < count && i < first + PROFILE_ROWS_VISIBLE; i++)
    {
        int rowY = 92 + (i - first) * 32;
        bool32 active = i == cursor;
        SDL_Rect clip = {603, rowY + 2, 292, 25};
        profile = Platform_ProfileGet(i);
        if (active)
        {
            FillRect(578, rowY, 322, 28,
                     focus == LAUNCHER_FOCUS_PROFILES ? (SDL_Color){28, 130, 84, 255}
                                                       : (SDL_Color){17, 83, 68, 255});
            StrokeRect(578, rowY, 322, 28, (SDL_Color){220, 174, 49, 255});
            DrawTextSized(586, rowY + 7, ">", 2, (SDL_Color){255, 230, 105, 255});
        }
        SDL_RenderSetClipRect(sdlRenderer, &clip);
        DrawTextSized(607, rowY + 7, profile != NULL ? profile->displayName : "UNKNOWN", 2,
                      active ? (SDL_Color){255, 245, 192, 255} : (SDL_Color){210, 231, 213, 255});
        SDL_RenderSetClipRect(sdlRenderer, NULL);
    }

    profile = Platform_ProfileGet(cursor);
    FillRect(570, 269, 338, 43, (SDL_Color){10, 49, 48, 255});
    DrawTextSized(580, 276, "LAST PLAYED", 1, (SDL_Color){112, 211, 169, 255});
    if (profile != NULL && profile->lastPlayedTime[0] != '\0')
    {
        char date[24];
        snprintf(date, sizeof(date), "%.10s", profile->lastPlayedTime);
        DrawTextSized(580, 292, date, 2, (SDL_Color){240, 240, 209, 255});
    }
    else
        DrawTextSized(580, 292, "NOT YET PLAYED", 2, (SDL_Color){240, 240, 209, 255});

    for (i = 0; i < LAUNCHER_ACTION_COUNT; i++)
        DrawButton(570, 321 + i * 37, 338, 32, actions[i],
                   focus == LAUNCHER_FOCUS_ACTIONS && actionCursor == (int)i);

    if (message != NULL && message[0] != '\0')
        DrawTextSized(44, 510, message, 1, (SDL_Color){255, 167, 160, 255});
    else
        DrawTextSized(44, 510, "ARROWS MOVE   ENTER/A SELECT   ESC/B BACK", 1,
                      (SDL_Color){178, 222, 197, 255});
}

static bool32 NextEvent(SDL_Event *event)
{
    while (!SDL_PollEvent(event))
        SDL_Delay(8);
    return event->type != SDL_QUIT;
}

static u8 ModifiersFromEvent(SDL_Keymod modifiers)
{
    u8 result = 0;
    if (modifiers & KMOD_SHIFT) result |= PLATFORM_INPUT_MODIFIER_SHIFT;
    if (modifiers & KMOD_CTRL) result |= PLATFORM_INPUT_MODIFIER_CTRL;
    if (modifiers & KMOD_ALT) result |= PLATFORM_INPUT_MODIFIER_ALT;
    if (modifiers & KMOD_GUI) result |= PLATFORM_INPUT_MODIFIER_GUI;
    return result;
}

static bool32 KeyMatchesAction(const SDL_KeyboardEvent *event, enum PlatformInputAction action)
{
    struct PlatformInputBinding binding;
    enum PlatformInputKey key;
    Platform_ConfigGetKeyboardBinding(action, &binding);
    key = Platform_InputKeyFromScancode(event->keysym.scancode);
    return Platform_InputBindingMatches(&binding, key, ModifiersFromEvent(event->keysym.mod));
}

static bool32 IsAcceptEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_A;
    return event->type == SDL_KEYDOWN && !event->key.repeat
        && (event->key.keysym.scancode == SDL_SCANCODE_RETURN
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_A));
}

static bool32 IsCancelEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_B;
    return event->type == SDL_KEYDOWN && !event->key.repeat
        && (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_B));
}

static bool32 IsUpEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP;
    return event->type == SDL_KEYDOWN
        && (event->key.keysym.scancode == SDL_SCANCODE_UP
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_DPAD_UP));
}

static bool32 IsDownEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    return event->type == SDL_KEYDOWN
        && (event->key.keysym.scancode == SDL_SCANCODE_DOWN
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_DPAD_DOWN));
}

static bool32 IsLeftEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    return event->type == SDL_KEYDOWN
        && (event->key.keysym.scancode == SDL_SCANCODE_LEFT
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_DPAD_LEFT));
}

static bool32 IsRightEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    return event->type == SDL_KEYDOWN
        && (event->key.keysym.scancode == SDL_SCANCODE_RIGHT
         || KeyMatchesAction(&event->key, PLATFORM_INPUT_ACTION_DPAD_RIGHT));
}

static bool32 IsPageEvent(const SDL_Event *event)
{
    if (event->type == SDL_CONTROLLERBUTTONDOWN)
        return event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER
            || event->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    return event->type == SDL_KEYDOWN && !event->key.repeat
        && event->key.keysym.scancode == SDL_SCANCODE_TAB;
}

static void PresentUi(void)
{
    SDL_RenderPresent(sdlRenderer);
}

static void DrawModal(const char *title, const char *line1, const char *line2,
                      const char *entry)
{
    DrawLauncherChrome();
    FillRect(566, 120, 346, 278, (SDL_Color){2, 11, 18, 255});
    FillRect(569, 123, 340, 272, (SDL_Color){210, 161, 42, 255});
    FillRect(573, 127, 332, 264, (SDL_Color){8, 43, 45, 255});
    StrokeRect(579, 133, 320, 252, (SDL_Color){79, 215, 156, 255});
    DrawCenteredText(580, 151, 318, title, 2, (SDL_Color){255, 235, 119, 255});
    FillRect(594, 179, 290, 2, (SDL_Color){209, 161, 43, 255});
    if (entry != NULL)
    {
        SDL_Rect clip = {596, 210, 286, 32};
        FillRect(591, 202, 300, 45, (SDL_Color){3, 23, 30, 255});
        StrokeRect(591, 202, 300, 45, (SDL_Color){44, 154, 116, 255});
        SDL_RenderSetClipRect(sdlRenderer, &clip);
        DrawTextSized(600, 215, entry, 2, (SDL_Color){248, 246, 215, 255});
        SDL_RenderSetClipRect(sdlRenderer, NULL);
    }
    if (line1 != NULL)
        DrawCenteredText(580, 276, 318, line1, 1, (SDL_Color){236, 240, 211, 255});
    if (line2 != NULL)
        DrawCenteredText(580, 306, 318, line2, 1, (SDL_Color){255, 166, 160, 255});
}

static bool32 TextEntry(const char *title, const char *initial, char *result, u32 resultSize)
{
    SDL_Event event;
    u32 length = 0;
    if (initial != NULL)
    {
        length = strlen(initial);
        if (length >= resultSize) length = resultSize - 1;
        memcpy(result, initial, length);
    }
    result[length] = '\0';
    SDL_StartTextInput();
    for (;;)
    {
        DrawModal(title, "TYPE A NAME, THEN PRESS ENTER", "ESCAPE CANCELS", result);
        PresentUi();
        if (!NextEvent(&event))
        {
            SDL_StopTextInput();
            return FALSE;
        }
        if (event.type == SDL_TEXTINPUT)
        {
            u32 added = strlen(event.text.text);
            if (added > 0 && length + added < resultSize)
            {
                u32 i;
                for (i = 0; i < added; i++)
                    if ((unsigned char)event.text.text[i] >= 32)
                        result[length++] = event.text.text[i];
                result[length] = '\0';
            }
        }
        else if (event.type == SDL_KEYDOWN)
        {
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            {
                SDL_StopTextInput();
                return FALSE;
            }
            if (event.key.keysym.scancode == SDL_SCANCODE_BACKSPACE && length != 0)
                result[--length] = '\0';
            else if (event.key.keysym.scancode == SDL_SCANCODE_RETURN && length != 0)
            {
                SDL_StopTextInput();
                return TRUE;
            }
        }
        else if (event.type == SDL_CONTROLLERBUTTONDOWN)
        {
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B)
            {
                SDL_StopTextInput();
                return FALSE;
            }
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A && length != 0)
            {
                SDL_StopTextInput();
                return TRUE;
            }
        }
    }
}

static bool32 ConfirmDelete(const char *name)
{
    SDL_Event event;
    char prompt[128];
    snprintf(prompt, sizeof(prompt), "DELETE %s AND ALL OF ITS SAVES?", name);
    for (;;)
    {
        DrawModal("DELETE PROFILE?", prompt, "ENTER/A/Y CONFIRM   ESC/B/N CANCEL", NULL);
        PresentUi();
        if (!NextEvent(&event)) return FALSE;
        if (IsAcceptEvent(&event)
         || (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_Y))
            return TRUE;
        if (IsCancelEvent(&event)
         || (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_N))
            return FALSE;
    }
}

static bool32 BindingConflicts(enum PlatformInputAction action,
                               const struct PlatformInputBinding *candidate)
{
    int i;
    for (i = 0; i < PLATFORM_INPUT_ACTION_COUNT; i++)
    {
        struct PlatformInputBinding current;
        if (i == action) continue;
        Platform_ConfigGetKeyboardBinding(i, &current);
        if (current.key == candidate->key && current.modifiers == candidate->modifiers)
            return TRUE;
    }
    return FALSE;
}

static void DrawSettingsChrome(enum SettingsPage page)
{
    static const char *const pageNames[] = {"CONTROLS", "DESKTOP", "GAME DATA"};
    static const int tabX[] = {216, 442, 668};
    static const int tabWidth[] = {220, 220, 244};
    int i;

    DrawBackground();
    DrawLogo(24, 34, TRUE);
    DrawTextSized(27, 125, "SETTINGS", 2, (SDL_Color){244, 242, 207, 255});
    FillRect(24, 148, 150, 3, (SDL_Color){214, 166, 45, 255});
    DrawHexagon(97, 315, 61, (SDL_Color){24, 104, 80, 255});
    DrawHexagon(97, 315, 50, (SDL_Color){182, 140, 38, 255});
    DrawTextSized(37, 293, pageNames[page], 2,
                  (SDL_Color){92, 224, 169, 255});
    DrawTextSized(30, 471, "TAB / L / R", 1, (SDL_Color){178, 222, 197, 255});
    DrawTextSized(30, 487, "SWITCH PAGE", 1, (SDL_Color){178, 222, 197, 255});

    DrawPanel(194, 24, 740, 492);
    for (i = 0; i < SETTINGS_PAGE_COUNT; i++)
    {
        FillRect(tabX[i], 47, tabWidth[i], 38,
                 page == i ? (SDL_Color){29, 128, 84, 255}
                           : (SDL_Color){8, 43, 45, 255});
        if (page == i)
            StrokeRect(tabX[i], 47, tabWidth[i], 38, (SDL_Color){218, 171, 48, 255});
        DrawCenteredText(tabX[i], 59, tabWidth[i], pageNames[i], 2,
                         page == i ? (SDL_Color){255, 237, 126, 255}
                                   : (SDL_Color){190, 221, 198, 255});
    }
}

static void DrawSettingRow(int y, const char *label, const char *value, bool32 active)
{
    if (active)
    {
        FillRect(218, y - 4, 690, 34, (SDL_Color){24, 113, 77, 255});
        StrokeRect(218, y - 4, 690, 34, (SDL_Color){207, 162, 45, 255});
        FillRect(226, y + 3, 5, 20, (SDL_Color){245, 102, 127, 255});
    }
    DrawTextSized(241, y + 4, label, 2,
                  active ? (SDL_Color){255, 241, 159, 255} : (SDL_Color){222, 235, 213, 255});
    if (value != NULL)
        DrawTextSized(720, y + 4, value, 2,
                      active ? (SDL_Color){117, 245, 183, 255} : (SDL_Color){143, 211, 178, 255});
}

static void DrawControlSettings(int cursor, bool32 waiting, const char *message)
{
    int count = PLATFORM_INPUT_ACTION_COUNT + 3;
    int first = cursor >= SETTINGS_ROWS_VISIBLE ? cursor - SETTINGS_ROWS_VISIBLE + 1 : 0;
    int i;
    char value[96];
    char position[32];

    DrawSettingsChrome(SETTINGS_PAGE_CONTROLS);
    for (i = first; i < count && i < first + SETTINGS_ROWS_VISIBLE; i++)
    {
        int y = 98 + (i - first) * 20;
        const char *label;
        if (i < PLATFORM_INPUT_ACTION_COUNT)
        {
            struct PlatformInputBinding binding;
            Platform_ConfigGetKeyboardBinding(i, &binding);
            Platform_InputBindingToString(&binding, value, sizeof(value));
            label = Platform_InputActionName(i);
        }
        else if (i == PLATFORM_INPUT_ACTION_COUNT)
        {
            u8 speed = Platform_ConfigGetFastForwardSpeed();
            label = "Fast-forward Multiplier";
            if (speed == 0) snprintf(value, sizeof(value), "Unlimited");
            else snprintf(value, sizeof(value), "%ux", speed);
        }
        else if (i == PLATFORM_INPUT_ACTION_COUNT + 1)
        {
            label = "Reset Controls to Defaults";
            value[0] = '\0';
        }
        else
        {
            label = "Back";
            value[0] = '\0';
        }
        if (i == cursor)
        {
            FillRect(218, y - 3, 690, 19, (SDL_Color){24, 113, 77, 255});
            StrokeRect(218, y - 3, 690, 19, (SDL_Color){207, 162, 45, 255});
            FillRect(225, y, 4, 12, (SDL_Color){245, 102, 127, 255});
        }
        DrawTextSized(238, y, label, 1,
                      i == cursor ? (SDL_Color){255, 241, 159, 255}
                                  : (SDL_Color){222, 235, 213, 255});
        if (value[0] != '\0')
            DrawTextSized(724, y, value, 1,
                          i == cursor ? (SDL_Color){117, 245, 183, 255}
                                      : (SDL_Color){143, 211, 178, 255});
    }
    snprintf(position, sizeof(position), "%d / %d", cursor + 1, count);
    DrawTextSized(836, 469, position, 1, (SDL_Color){112, 197, 161, 255});
    if (waiting)
        DrawTextSized(225, 469, "PRESS A NEW KEY   ESCAPE CANCELS", 1,
                      (SDL_Color){255, 166, 160, 255});
    else if (message != NULL && message[0] != '\0')
        DrawTextSized(225, 469, message, 1, (SDL_Color){255, 166, 160, 255});
    else
        DrawTextSized(225, 469, "UP/DOWN MOVE   ENTER CHANGE   TAB PAGE", 1,
                      (SDL_Color){178, 222, 197, 255});
}

static void DesktopSettingValue(int cursor, char *value, u32 valueSize)
{
    static const enum PlatformSetting settingIds[] =
    {
        PLATFORM_SETTING_FULLSCREEN,
        PLATFORM_SETTING_WINDOW_SCALE,
        PLATFORM_SETTING_INTEGER_SCALE,
        PLATFORM_SETTING_VSYNC,
        PLATFORM_SETTING_VOLUME,
    };
    u8 current;
    if (cursor < 0 || cursor >= 5)
    {
        value[0] = '\0';
        return;
    }
    current = Platform_GetSetting(settingIds[cursor]);
    if (cursor == 1)
        snprintf(value, valueSize, "%ux", current);
    else if (cursor == 4)
        snprintf(value, valueSize, "%u / 10", current);
    else
        snprintf(value, valueSize, "%s", current ? "On" : "Off");
}

static void DrawDesktopSettings(int cursor, const char *message)
{
    static const char *const labels[] =
    {
        "Fullscreen", "Window Scale", "Integer Scaling", "VSync",
        "Audio Volume", "Back"
    };
    int i;
    char value[32];

    DrawSettingsChrome(SETTINGS_PAGE_DESKTOP);
    for (i = 0; i < 6; i++)
    {
        DesktopSettingValue(i, value, sizeof(value));
        DrawSettingRow(112 + i * 47, labels[i], value[0] != '\0' ? value : NULL, i == cursor);
    }
    if (message != NULL && message[0] != '\0')
        DrawTextSized(225, 469, message, 1, (SDL_Color){255, 166, 160, 255});
    else
        DrawTextSized(225, 469, "LEFT/RIGHT CHANGE   ENTER TOGGLE   ESC BACK", 1,
                      (SDL_Color){178, 222, 197, 255});
}

static void DrawGameDataSettings(int cursor, const char *message)
{
    bool32 installed = Platform_GameContentVerifyInstalled(FALSE);

    DrawSettingsChrome(SETTINGS_PAGE_GAME_DATA);
    DrawTextSized(240, 117, "POKEMON EMERALD", 2, (SDL_Color){222, 235, 213, 255});
    DrawTextSized(720, 117, installed ? "INSTALLED" : "MISSING", 2,
                  installed ? (SDL_Color){117, 245, 183, 255}
                            : (SDL_Color){255, 166, 160, 255});
    DrawTextSized(240, 151, "FORMAT VERSION", 1, (SDL_Color){143, 211, 178, 255});
    DrawTextSized(720, 151, "1", 1, (SDL_Color){143, 211, 178, 255});
    DrawSettingRow(205, "Verify Game Data", NULL, cursor == 0);
    DrawSettingRow(260, "Reimport / Repair", NULL, cursor == 1);
    DrawSettingRow(315, "Back", NULL, cursor == 2);
    if (message != NULL && message[0] != '\0')
        DrawTextSized(225, 469, message, 1, (SDL_Color){255, 166, 160, 255});
    else
        DrawTextSized(225, 469, "ENTER SELECT   TAB PAGE   ESC BACK", 1,
                      (SDL_Color){178, 222, 197, 255});
}

static void CycleFastForward(int direction)
{
    static const u8 speeds[] = {2, 3, 4, 5, 0};
    int i;
    u8 current = Platform_ConfigGetFastForwardSpeed();
    for (i = 0; i < 5 && speeds[i] != current; i++);
    if (i == 5) i = 0;
    i = (i + (direction < 0 ? 4 : 1)) % 5;
    Platform_ConfigSetFastForwardSpeed(speeds[i]);
    Platform_ConfigStore();
}

static void AdjustDesktopSetting(int cursor, int direction)
{
    static const enum PlatformSetting settingIds[] =
    {
        PLATFORM_SETTING_FULLSCREEN,
        PLATFORM_SETTING_WINDOW_SCALE,
        PLATFORM_SETTING_INTEGER_SCALE,
        PLATFORM_SETTING_VSYNC,
        PLATFORM_SETTING_VOLUME,
    };
    u8 current;
    u8 next;

    if (cursor < 0 || cursor >= 5)
        return;
    current = Platform_GetSetting(settingIds[cursor]);
    if (cursor == 1)
    {
        if (current < 2 || current > 5) current = 4;
        next = direction < 0 ? (current == 2 ? 5 : current - 1)
                             : (current == 5 ? 2 : current + 1);
    }
    else if (cursor == 4)
        next = direction < 0 ? (current == 0 ? 10 : current - 1)
                             : (current == 10 ? 0 : current + 1);
    else
        next = !current;
    Platform_SetSetting(settingIds[cursor], next);
}

static void Settings(void)
{
    SDL_Event event;
    enum SettingsPage page = SETTINGS_PAGE_CONTROLS;
    int controlsCursor = 0;
    int desktopCursor = 0;
    int gameDataCursor = 0;
    char message[96] = "";

    for (;;)
    {
        if (page == SETTINGS_PAGE_CONTROLS)
            DrawControlSettings(controlsCursor, FALSE, message);
        else if (page == SETTINGS_PAGE_DESKTOP)
            DrawDesktopSettings(desktopCursor, message);
        else
            DrawGameDataSettings(gameDataCursor, message);
        PresentUi();
        if (!NextEvent(&event)) return;
        if (IsCancelEvent(&event)) return;
        if (IsPageEvent(&event))
        {
            page = (page + 1) % SETTINGS_PAGE_COUNT;
            message[0] = '\0';
            continue;
        }

        if (page == SETTINGS_PAGE_CONTROLS)
        {
            int last = PLATFORM_INPUT_ACTION_COUNT + 2;
            if (IsUpEvent(&event) && controlsCursor > 0)
                controlsCursor--;
            else if (IsDownEvent(&event) && controlsCursor < last)
                controlsCursor++;
            else if ((IsLeftEvent(&event) || IsRightEvent(&event))
                  && controlsCursor == PLATFORM_INPUT_ACTION_COUNT)
                CycleFastForward(IsLeftEvent(&event) ? -1 : 1);
            else if (IsRightEvent(&event))
            {
                page = SETTINGS_PAGE_DESKTOP;
                message[0] = '\0';
            }
            else if (IsAcceptEvent(&event) && controlsCursor < PLATFORM_INPUT_ACTION_COUNT)
            {
                message[0] = '\0';
                for (;;)
                {
                    struct PlatformInputBinding candidate;
                    DrawControlSettings(controlsCursor, TRUE, NULL);
                    PresentUi();
                    if (!NextEvent(&event)) return;
                    if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
                    if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) break;
                    candidate.key = Platform_InputKeyFromScancode(event.key.keysym.scancode);
                    candidate.modifiers = ModifiersFromEvent(event.key.keysym.mod);
                    if (!Platform_InputBindingIsUsable(&candidate))
                    {
                        snprintf(message, sizeof(message), "UNSUPPORTED KEY");
                        break;
                    }
                    if (BindingConflicts(controlsCursor, &candidate))
                    {
                        snprintf(message, sizeof(message), "KEY ALREADY USED BY AN ACTION");
                        break;
                    }
                    Platform_ConfigSetKeyboardBinding(controlsCursor, &candidate);
                    Platform_ConfigStore();
                    snprintf(message, sizeof(message), "BINDING SAVED");
                    break;
                }
            }
            else if (IsAcceptEvent(&event) && controlsCursor == PLATFORM_INPUT_ACTION_COUNT)
                CycleFastForward(1);
            else if (IsAcceptEvent(&event) && controlsCursor == PLATFORM_INPUT_ACTION_COUNT + 1)
            {
                Platform_ConfigResetInputDefaults();
                Platform_ConfigStore();
                snprintf(message, sizeof(message), "DEFAULT CONTROLS RESTORED");
            }
            else if (IsAcceptEvent(&event) && controlsCursor == PLATFORM_INPUT_ACTION_COUNT + 2)
                return;
        }
        else if (page == SETTINGS_PAGE_DESKTOP)
        {
            if (IsUpEvent(&event) && desktopCursor > 0)
                desktopCursor--;
            else if (IsDownEvent(&event) && desktopCursor < 5)
                desktopCursor++;
            else if (IsLeftEvent(&event) && desktopCursor < 5)
                AdjustDesktopSetting(desktopCursor, -1);
            else if (IsRightEvent(&event) && desktopCursor < 5)
                AdjustDesktopSetting(desktopCursor, 1);
            else if (IsAcceptEvent(&event) && desktopCursor < 5)
                AdjustDesktopSetting(desktopCursor, 1);
            else if (IsAcceptEvent(&event) && desktopCursor == 5)
                return;
        }
        else
        {
            if (IsUpEvent(&event) && gameDataCursor > 0)
                gameDataCursor--;
            else if (IsDownEvent(&event) && gameDataCursor < 2)
                gameDataCursor++;
            else if (IsAcceptEvent(&event) && gameDataCursor == 0)
            {
                if (Platform_GameContentVerifyInstalled(TRUE))
                    snprintf(message, sizeof(message), "GAME DATA VERIFIED");
                else
                    snprintf(message, sizeof(message), "VERIFY FAILED: %.70s",
                             Platform_GameContentGetLastError());
            }
            else if (IsAcceptEvent(&event) && gameDataCursor == 1)
            {
                char romPath[1024];
                struct PlatformGameContentImportInfo info;
                enum PlatformFileDialogResult result;

                result = Platform_FileDialogSelectEmeraldRom(romPath, sizeof(romPath));
                if (result == PLATFORM_FILE_DIALOG_NO_PICKER_AVAILABLE)
                    snprintf(message, sizeof(message),
                             "No file picker found. Install Zenity, KDialog, or YAD.");
                else if (result == PLATFORM_FILE_DIALOG_SELECTED)
                {
                    if (Platform_GameContentImport(romPath, &info)
                        == PLATFORM_GAME_CONTENT_IMPORT_OK)
                        snprintf(message, sizeof(message), "GAME DATA REIMPORTED - SAVES PRESERVED");
                    else if (info.result == PLATFORM_GAME_CONTENT_IMPORT_UNSUPPORTED_ROM)
                        snprintf(message, sizeof(message), "UNSUPPORTED ROM: %.40s", info.detectedSha1);
                    else
                        snprintf(message, sizeof(message), "REIMPORT FAILED: %.70s", info.error);
                }
            }
            else if (IsAcceptEvent(&event) && gameDataCursor == 2)
                return;
        }
    }
}

static bool32 Launcher(void)
{
    SDL_Event event;
    u32 profileCursor = Platform_ProfileGetSelectedIndex();
    enum LauncherFocus focus = LAUNCHER_FOCUS_PROFILES;
    int actionCursor = LAUNCHER_ACTION_PLAY;
    char name[PLATFORM_PROFILE_NAME_LENGTH];
    char message[96] = "";

    for (;;)
    {
        DrawProfiles(profileCursor, focus, actionCursor, message);
        PresentUi();
        if (!NextEvent(&event)) return FALSE;
        if (IsCancelEvent(&event))
        {
            if (focus == LAUNCHER_FOCUS_ACTIONS)
                focus = LAUNCHER_FOCUS_PROFILES;
            else
                return FALSE;
            continue;
        }
        if (focus == LAUNCHER_FOCUS_PROFILES)
        {
            if (IsUpEvent(&event) && profileCursor > 0)
                profileCursor--;
            else if (IsDownEvent(&event))
            {
                if (profileCursor + 1 < Platform_ProfileCount())
                    profileCursor++;
                else
                {
                    focus = LAUNCHER_FOCUS_ACTIONS;
                    actionCursor = LAUNCHER_ACTION_PLAY;
                }
            }
            else if (IsRightEvent(&event) || IsAcceptEvent(&event))
            {
                focus = LAUNCHER_FOCUS_ACTIONS;
                actionCursor = LAUNCHER_ACTION_PLAY;
            }
        }
        else
        {
            if (IsUpEvent(&event))
            {
                if (actionCursor > LAUNCHER_ACTION_PLAY)
                    actionCursor--;
                else
                    focus = LAUNCHER_FOCUS_PROFILES;
            }
            else if (IsDownEvent(&event) && actionCursor + 1 < LAUNCHER_ACTION_COUNT)
                actionCursor++;
            else if (IsLeftEvent(&event))
                focus = LAUNCHER_FOCUS_PROFILES;
            else if (IsAcceptEvent(&event))
            {
                if (actionCursor == LAUNCHER_ACTION_PLAY)
                {
                    if (Platform_ProfileSelect(profileCursor))
                        return TRUE;
                    snprintf(message, sizeof(message), "PROFILE COULD NOT BE SELECTED");
                }
                else if (actionCursor == LAUNCHER_ACTION_NEW_PROFILE)
                {
                    if (TextEntry("NEW PROFILE", "", name, sizeof(name)))
                    {
                        u32 created;
                        if (Platform_ProfileCreate(name, &created))
                        {
                            profileCursor = created;
                            focus = LAUNCHER_FOCUS_PROFILES;
                            snprintf(message, sizeof(message), "PROFILE CREATED");
                        }
                        else
                            snprintf(message, sizeof(message), "PROFILE COULD NOT BE CREATED");
                    }
                }
                else if (actionCursor == LAUNCHER_ACTION_DELETE_PROFILE)
                {
                    const struct PlatformProfileMetadata *profile = Platform_ProfileGet(profileCursor);
                    if (Platform_ProfileCount() <= 1)
                        snprintf(message, sizeof(message), "ONE PROFILE MUST REMAIN");
                    else if (profile != NULL && ConfirmDelete(profile->displayName))
                    {
                        if (Platform_ProfileDelete(profileCursor))
                        {
                            if (profileCursor >= Platform_ProfileCount())
                                profileCursor = Platform_ProfileCount() - 1;
                            focus = LAUNCHER_FOCUS_PROFILES;
                            snprintf(message, sizeof(message), "PROFILE DELETED");
                        }
                        else
                            snprintf(message, sizeof(message), "PROFILE COULD NOT BE DELETED");
                    }
                }
                else if (actionCursor == LAUNCHER_ACTION_SETTINGS)
                    Settings();
                else if (actionCursor == LAUNCHER_ACTION_QUIT)
                    return FALSE;
            }
        }
    }
}

static SDL_Texture *LoadRayquazaTexture(void)
{
#if defined(NATIVE_LINUX) || defined(_WIN32)
    static bool32 sImageInitialized;
    char path[1024];
    SDL_Texture *texture = NULL;

    if (!sImageInitialized)
    {
        sImageInitialized = TRUE;
        if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
            SDL_Log("SDL_image could not initialize: %s", IMG_GetError());
    }
    if (Platform_AssetGetPath("images/rayquaza.png", path, sizeof(path)))
        texture = IMG_LoadTexture(sdlRenderer, path);
    if (texture == NULL)
        SDL_Log("Rayquaza image could not be loaded from images/rayquaza.png: %s", IMG_GetError());
    else
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return texture;
#else
    return NULL;
#endif
}

static void DrawGameDataSetup(int cursor, const struct PlatformGameContentImportInfo *info,
                              const char *message)
{
    DrawLauncherChrome();
    DrawTextOutlined(579, 54, "GAME DATA REQUIRED", 3,
                     (SDL_Color){244, 242, 207, 255}, (SDL_Color){2, 13, 19, 255}, 1);
    FillRect(570, 84, 338, 3, (SDL_Color){214, 166, 45, 255});

    if (info != NULL && info->result == PLATFORM_GAME_CONTENT_IMPORT_UNSUPPORTED_ROM)
    {
        DrawCenteredText(570, 111, 338, "UNSUPPORTED POKEMON EMERALD ROM", 1,
                         (SDL_Color){255, 166, 160, 255});
        DrawCenteredText(570, 137, 338, "THE SELECTED ROM DOES NOT MATCH", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawCenteredText(570, 153, 338, "THE SUPPORTED EMERALD REVISION.", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawTextSized(579, 184, "DETECTED SHA-1:", 1, (SDL_Color){112, 211, 169, 255});
        DrawTextSized(579, 201, info->detectedSha1, 1, (SDL_Color){255, 235, 119, 255});
        DrawTextSized(579, 231, "EXPECTED:", 1, (SDL_Color){112, 211, 169, 255});
        DrawTextSized(579, 248, Platform_GameContentGetExpectedSha1(), 1,
                      (SDL_Color){255, 235, 119, 255});
    }
    else
    {
        DrawCenteredText(570, 115, 338, "EMERALD RECOMP REQUIRES A COMPATIBLE", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawCenteredText(570, 133, 338, "POKEMON EMERALD ROM.", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawCenteredText(570, 169, 338, "SELECT A ROM YOU LEGALLY OWN TO", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawCenteredText(570, 187, 338, "INSTALL THE REQUIRED GAME DATA LOCALLY.", 1,
                         (SDL_Color){236, 240, 211, 255});
        DrawCenteredText(570, 225, 338, "THE ROM STAYS ON THIS COMPUTER.", 1,
                         (SDL_Color){112, 211, 169, 255});
        DrawCenteredText(570, 243, 338, "NO ROM DATA IS UPLOADED OR TRANSMITTED.", 1,
                         (SDL_Color){112, 211, 169, 255});
        if (message != NULL && message[0] != '\0')
            DrawCenteredText(570, 295, 338, message, 1,
                             (SDL_Color){255, 166, 160, 255});
        else if (info != NULL && info->error[0] != '\0')
            DrawCenteredText(570, 275, 338, info->error, 1,
                             (SDL_Color){255, 166, 160, 255});
    }

    DrawButton(570, 329, 338, 46, "SELECT ROM", cursor == 0);
    DrawButton(570, 389, 338, 46, "QUIT", cursor == 1);
    DrawCenteredText(570, 472, 338, "ARROWS MOVE   ENTER/A SELECT", 1,
                     (SDL_Color){178, 222, 197, 255});
}

bool32 Platform_FrontendRunGameDataSetup(void)
{
    SDL_Event event;
    int cursor = 0;
    char romPath[1024];
    struct PlatformGameContentImportInfo info;
    bool32 hasInfo = FALSE;
    char message[96] = "";

    memset(&info, 0, sizeof(info));
    Platform_VideoBeginHostUi();
    Platform_VideoSetStatus("Pokemon Emerald [Recomp] - Game Data Required");
    sRayquazaTexture = LoadRayquazaTexture();
    for (;;)
    {
        DrawGameDataSetup(cursor, hasInfo ? &info : NULL, message);
        PresentUi();
        if (!NextEvent(&event) || IsCancelEvent(&event))
            break;
        if (IsUpEvent(&event) || IsDownEvent(&event))
            cursor = 1 - cursor;
        else if (IsAcceptEvent(&event))
        {
            enum PlatformFileDialogResult result;

            if (cursor == 1)
                break;
            result = Platform_FileDialogSelectEmeraldRom(romPath, sizeof(romPath));
            if (result == PLATFORM_FILE_DIALOG_NO_PICKER_AVAILABLE)
            {
                snprintf(message, sizeof(message),
                         "No file picker found. Install Zenity, KDialog, or YAD.");
                continue;
            }
            if (result != PLATFORM_FILE_DIALOG_SELECTED)
                continue;
            message[0] = '\0';
            hasInfo = TRUE;
            if (Platform_GameContentImport(romPath, &info) == PLATFORM_GAME_CONTENT_IMPORT_OK)
            {
                SDL_DestroyTexture(sRayquazaTexture);
                sRayquazaTexture = NULL;
                Platform_InputClearState();
                Platform_VideoEndHostUi();
                Platform_VideoSetStatus(NULL);
                return TRUE;
            }
        }
    }
    SDL_DestroyTexture(sRayquazaTexture);
    sRayquazaTexture = NULL;
    Platform_InputClearState();
    Platform_VideoEndHostUi();
    Platform_VideoSetStatus(NULL);
    return FALSE;
}

bool32 Platform_FrontendRunStartup(void)
{
    bool32 result;
    Platform_VideoBeginHostUi();
    Platform_VideoSetStatus("Pokemon Emerald [Recomp] - Select Profile");
    sRayquazaTexture = LoadRayquazaTexture();
    result = Launcher();
    SDL_DestroyTexture(sRayquazaTexture);
    sRayquazaTexture = NULL;
    Platform_InputClearState();
    Platform_VideoEndHostUi();
    Platform_VideoSetStatus(NULL);
    return result;
}

void Platform_FrontendRunSettings(void)
{
    Platform_VideoBeginHostUi();
    Settings();
    Platform_InputClearState();
    Platform_VideoEndHostUi();
}

#endif
