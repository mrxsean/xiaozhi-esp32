#pragma once
#include <cstddef>
#include <cstdint>

namespace xkq {
constexpr uint16_t Crc(const uint8_t* bytes, size_t size) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xa001 : 0);
        }
    }
    return crc;
}
constexpr bool ValidReply(const uint8_t* reply, size_t size, uint8_t address, uint8_t function) {
    if (size < 5 || reply[0] != address || reply[1] != function) {
        return false;
    }
    return Crc(reply, size - 2) == (reply[size - 2] | (uint16_t(reply[size - 1]) << 8));
}
constexpr uint8_t kReadVector[] = {1, 3, 0, 0, 0, 50};
static_assert(Crc(kReadVector, sizeof(kReadVector)) == 0x1fc4, "Modbus CRC byte order");
}  // namespace xkq
