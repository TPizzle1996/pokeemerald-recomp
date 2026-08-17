#ifndef GUARD_PLATFORM_DESKTOP_CONFIG_H
#define GUARD_PLATFORM_DESKTOP_CONFIG_H

#include "platform.h"
#include "platform/desktop_input.h"

void Platform_ConfigLoad(void);
void Platform_ConfigStore(void);
void Platform_ConfigSetSetting(enum PlatformSetting setting, u8 value);
void Platform_ConfigGetKeyboardBinding(enum PlatformInputAction action, struct PlatformInputBinding *binding);
void Platform_ConfigSetKeyboardBinding(enum PlatformInputAction action, const struct PlatformInputBinding *binding);
void Platform_ConfigResetInputDefaults(void);
u8 Platform_ConfigGetFastForwardSpeed(void);
void Platform_ConfigSetFastForwardSpeed(u8 speed);

#endif
