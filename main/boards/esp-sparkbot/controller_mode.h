#pragma once
#include "device_state.h"

constexpr bool ShowRegularController(DeviceState state) {
    return state == kDeviceStateIdle;
}
