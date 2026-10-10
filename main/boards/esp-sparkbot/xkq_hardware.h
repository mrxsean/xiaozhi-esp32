#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

// Wiring, CAP1298 configuration and register map from branch z_XKQ.
class XkqHardware {
   public:
    enum Key : uint16_t {
        Ok = 1,
        Menu = 2,
        Down = 4,
        Power = 8,
        Back = 16,
        Up = 32,
        Long = 0x8000
    };
    enum class Field { Power, Mode, Temperature };
    struct State {
        bool online = false;
        bool pending = false;
        bool write_failed = false;
        uint16_t power = 0;
        uint16_t mode = 1;        // z_XKQ: heat=0, cool=1, fan=5, dry=6.
        int16_t temperature = 0;  // All temperatures are in tenths of a degree.
        int16_t outlet = 0;
        int16_t inlet = 0;
        int16_t ambient = 0;
        uint16_t error = 0;
        int64_t last_seen = 0;
    };
    static XkqHardware& GetInstance();
    void Start(std::function<void(uint16_t)> on_key);
    State GetState(unsigned device);
    bool Set(unsigned device, Field field, int value);
    void Beep();
    static bool SupportsTemperature(uint16_t mode) {
        return mode == 0 || mode == 1;
    }
    static int MinimumTemperature(uint16_t mode) {
        return mode == 0 ? 25 : 5;
    }
    static int MaximumTemperature(uint16_t mode) {
        return mode == 0 ? 55 : 25;
    }

   private:
    struct Command {
        unsigned device;
        Field field;
        int value;
    };
    std::mutex mutex_;
    std::array<State, 1> states_{};
    QueueHandle_t commands_ = nullptr;
    esp_timer_handle_t beep_timer_ = nullptr;
    i2c_master_bus_handle_t touch_bus_ = nullptr;
    i2c_master_dev_handle_t touch_device_ = nullptr;
    std::function<void(uint16_t)> on_key_;
    void RunBus();
    void RunKeys();
    bool ReadRegisters(unsigned device);
    bool WriteRegisters(const Command& command);
    int Exchange(uint8_t* request, int length, uint8_t* reply, int expected);
};
