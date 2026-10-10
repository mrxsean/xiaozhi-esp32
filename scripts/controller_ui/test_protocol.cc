// Compile-time regression tests; no board or ESP-IDF runtime is required.
#include "main/boards/esp-sparkbot/xkq_protocol.h"
#include "main/boards/esp-sparkbot/controller_mode.h"

constexpr uint8_t read_reply[] = {1, 3, 2, 0, 10, 0x38, 0x43};
constexpr uint8_t corrupt_reply[] = {1, 3, 2, 0, 11, 0x38, 0x43};
constexpr uint8_t write_reply[] = {1, 0x10, 0, 0, 0, 3, 0x80, 0x08};
constexpr uint8_t reversed_crc[] = {1, 3, 2, 0, 10, 0x43, 0x38};
static_assert(xkq::ValidReply(read_reply, sizeof(read_reply), 1, 3));
static_assert(xkq::ValidReply(write_reply, sizeof(write_reply), 1, 0x10));
static_assert(!xkq::ValidReply(corrupt_reply, sizeof(corrupt_reply), 1, 3));
static_assert(!xkq::ValidReply(reversed_crc, sizeof(reversed_crc), 1, 3));
static_assert(!xkq::ValidReply(read_reply, sizeof(read_reply), 2, 3));
static_assert(!xkq::ValidReply(read_reply, sizeof(read_reply), 1, 4));
static_assert(!xkq::ValidReply(read_reply, 4, 1, 3));
static_assert(!xkq::ValidReply(read_reply, 6, 1, 3));
static_assert(ShowRegularController(kDeviceStateIdle));
static_assert(!ShowRegularController(kDeviceStateUnknown));
static_assert(!ShowRegularController(kDeviceStateStarting));
static_assert(!ShowRegularController(kDeviceStateConnecting));
static_assert(!ShowRegularController(kDeviceStateListening));
static_assert(!ShowRegularController(kDeviceStateSpeaking));
static_assert(!ShowRegularController(kDeviceStateWifiConfiguring));
static_assert(!ShowRegularController(kDeviceStateActivating));
static_assert(!ShowRegularController(kDeviceStateUpgrading));
static_assert(!ShowRegularController(kDeviceStateAudioTesting));
static_assert(!ShowRegularController(kDeviceStateFatalError));
