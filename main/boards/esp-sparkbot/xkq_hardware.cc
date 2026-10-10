#include "xkq_hardware.h"
#include "xkq_protocol.h"
#include <algorithm>
#include <cstring>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/task.h>

namespace {
constexpr auto kUart = UART_NUM_1;
constexpr const char* kTag = "XkqHardware";
// Unmodified CAP1298 register values from z_XKQ/drv_touch_key.c.
constexpr uint8_t kCapConfig[][2] = {
    {0x00, 0x4c}, {0x1f, 0x4f}, {0x20, 0x90}, {0x21, 0x3f}, {0x22, 0xa4}, {0x23, 0},
    {0x24, 0x38}, {0x26, 0xc0}, {0x27, 0x3f}, {0x28, 0},    {0x29, 0},    {0x2a, 0},
    {0x2b, 0x84}, {0x2d, 0x3f}, {0x2f, 0},    {0x30, 0x50}, {0x31, 0x50}, {0x32, 0x50},
    {0x33, 0x50}, {0x34, 0x50}, {0x35, 0x50}, {0x36, 0x50}, {0x37, 0x50}, {0x38, 3},
    {0x40, 0},    {0x41, 0x30}, {0x42, 2},    {0x43, 0x40}, {0x44, 0x44}, {0x60, 0},
    {0x61, 0x22}, {0x80, 0x55}, {0x81, 0x55}};
}  // namespace

XkqHardware& XkqHardware::GetInstance() {
    static XkqHardware instance;
    return instance;
}

void XkqHardware::Start(std::function<void(uint16_t)> on_key) {
    if (commands_) {
        return;
    }
    on_key_ = std::move(on_key);
    commands_ = xQueueCreate(8, sizeof(Command));
    ESP_ERROR_CHECK(commands_ ? ESP_OK : ESP_ERR_NO_MEM);
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_1;
    timer.duty_resolution = LEDC_TIMER_13_BIT;
    timer.freq_hz = 2930;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel{};
    channel.gpio_num = 37;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_1;
    channel.timer_sel = LEDC_TIMER_1;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
    esp_timer_create_args_t beep{};
    beep.name = "xkq_beep";
    beep.callback = [](void*) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    };
    ESP_ERROR_CHECK(esp_timer_create(&beep, &beep_timer_));
    ESP_ERROR_CHECK(xTaskCreate(
                        [](void* p) {
                            static_cast<XkqHardware*>(p)->RunBus();
                        },
                        "xkq_485",
                        4096,
                        this,
                        4,
                        nullptr) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(
                        [](void* p) {
                            static_cast<XkqHardware*>(p)->RunKeys();
                        },
                        "xkq_keys",
                        4096,
                        this,
                        3,
                        nullptr) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}

void XkqHardware::Beep() {
    if (!beep_timer_) {
        return;
    }
    esp_timer_stop(beep_timer_);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1024);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    esp_timer_start_once(beep_timer_, 80000);
}

XkqHardware::State XkqHardware::GetState(unsigned device) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device >= states_.size()) {
        return {};
    }
    auto state = states_[device];
    state.online = state.last_seen != 0 && esp_timer_get_time() - state.last_seen < 6000000;
    return state;
}

bool XkqHardware::Set(unsigned device, Field field, int value) {
    auto state = GetState(device);
    if (!commands_ || !state.online) {
        return false;
    }
    if (field == Field::Power && value != 0 && value != 1) {
        return false;
    }
    if (field == Field::Mode && value != 0 && value != 1 && value != 5 && value != 6) {
        return false;
    }
    if (field == Field::Temperature &&
        (!SupportsTemperature(state.mode) || value < MinimumTemperature(state.mode) * 10 ||
         value > MaximumTemperature(state.mode) * 10)) {
        return false;
    }
    Command command{device, field, value};
    // Publish pending before enqueue: the worker may run as soon as the queue wakes.
    std::lock_guard<std::mutex> lock(mutex_);
    if (states_[device].pending) {
        return false;
    }
    states_[device].pending = true;
    states_[device].write_failed = false;
    if (xQueueSend(commands_, &command, 0) != pdTRUE) {
        states_[device].pending = false;
        return false;
    }
    return true;
}

int XkqHardware::Exchange(uint8_t* request, int length, uint8_t* reply, int expected) {
    auto crc = xkq::Crc(request, length);
    request[length++] = crc & 255;
    request[length++] = crc >> 8;
    uart_flush_input(kUart);
    if (uart_write_bytes(kUart, request, length) != length) {
        return 0;
    }
    if (uart_wait_tx_done(kUart, pdMS_TO_TICKS(100)) != ESP_OK) {
        return 0;
    }
    int count = 0;
    auto deadline = esp_timer_get_time() + 350000;
    while (count < expected && esp_timer_get_time() < deadline) {
        int received = uart_read_bytes(kUart, reply + count, expected - count, pdMS_TO_TICKS(20));
        if (received > 0) {
            count += received;
        }
        if (count >= 5 && (reply[1] & 0x80)) {
            return 0;
        }
    }
    if (count != expected || !xkq::ValidReply(reply, count, request[0], request[1])) {
        return 0;
    }
    return count;
}

bool XkqHardware::ReadRegisters(unsigned device) {
    uint8_t request[8] = {uint8_t(device + 1), 3, 0, 0, 0, 50};
    uint8_t reply[105]{};
    if (!Exchange(request, 6, reply, sizeof(reply)) || reply[2] != 100) {
        return false;
    }
    auto word = [&](int reg) {
        return uint16_t((reply[3 + reg * 2] << 8) | reply[4 + reg * 2]);
    };
    // Reject nonsensical control registers; never index arrays using remote data.
    if (word(0) > 1 || word(1) > 7) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto& s = states_[device];
    s.power = word(0);
    s.mode = word(1);
    s.temperature = int16_t(word(2));
    s.outlet = int16_t(word(3));
    s.inlet = int16_t(word(4));
    s.ambient = int16_t(word(5));
    s.error = word(6);
    s.last_seen = esp_timer_get_time();
    s.online = true;
    return true;
}

bool XkqHardware::WriteRegisters(const Command& command) {
    auto s = GetState(command.device);
    if (!s.online) {
        return false;
    }
    if (command.field == Field::Power) {
        s.power = command.value;
    }
    if (command.field == Field::Mode) {
        s.mode = command.value;
        if (SupportsTemperature(s.mode)) {
            s.temperature = std::clamp<int>(
                s.temperature, MinimumTemperature(s.mode) * 10, MaximumTemperature(s.mode) * 10);
        }
    }
    if (command.field == Field::Temperature) {
        if (!SupportsTemperature(s.mode) || command.value < MinimumTemperature(s.mode) * 10 ||
            command.value > MaximumTemperature(s.mode) * 10) {
            return false;
        }
        s.temperature = command.value;
    }
    uint8_t request[15] = {uint8_t(command.device + 1),
                           0x10,
                           0,
                           0,
                           0,
                           3,
                           6,
                           0,
                           uint8_t(s.power),
                           0,
                           uint8_t(s.mode),
                           uint8_t(uint16_t(s.temperature) >> 8),
                           uint8_t(s.temperature)};
    uint8_t reply[8]{};
    if (!Exchange(request, 13, reply, 8) || std::memcmp(request, reply, 6) != 0) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!ReadRegisters(command.device)) {
        return false;
    }
    auto actual = GetState(command.device);
    return actual.power == s.power && actual.mode == s.mode && actual.temperature == s.temperature;
}

void XkqHardware::RunBus() {
    uart_config_t config{};
    config.baud_rate = 9600;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kUart, 2048, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kUart, &config));
    ESP_ERROR_CHECK(uart_set_pin(kUart, 11, 10, 9, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(kUart, UART_MODE_RS485_HALF_DUPLEX));
    ESP_ERROR_CHECK(uart_set_rx_timeout(kUart, 3));
    unsigned device = 0;
    for (;;) {
        Command command{};
        if (xQueueReceive(commands_, &command, pdMS_TO_TICKS(50)) == pdTRUE) {
            bool ok = WriteRegisters(command);
            std::lock_guard<std::mutex> lock(mutex_);
            states_[command.device].pending = false;
            states_[command.device].write_failed = !ok;
        } else {
            ReadRegisters(device);
            device = (device + 1) % states_.size();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void XkqHardware::RunKeys() {
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_1;
    bus.sda_io_num = GPIO_NUM_6;
    bus.scl_io_num = GPIO_NUM_5;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &touch_bus_));
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = 0x28;
    config.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(touch_bus_, &config, &touch_device_));
    gpio_config_t irq{};
    irq.pin_bit_mask = 1ULL << 4;
    irq.mode = GPIO_MODE_INPUT;
    irq.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&irq));
    auto read = [&](uint8_t reg, uint8_t* bytes, size_t length) {
        return i2c_master_transmit_receive(touch_device_, &reg, 1, bytes, length, 50) == ESP_OK;
    };
    uint8_t product = 0, maker = 0;
    bool cap = read(0xfd, &product, 1) && read(0xfe, &maker, 1) && product == 0x71 && maker == 0x5d;
    if (cap) {
        for (auto& pair : kCapConfig) {
            ESP_ERROR_CHECK(i2c_master_transmit(touch_device_, pair, 2, 50));
        }
    }
    ESP_LOGI(kTag, "Touch controller: %s", cap ? "CAP1298" : "Cypress-compatible (z_XKQ)");
    uint8_t previous = 0, stable = 0;
    int stable_count = 0;
    int64_t pressed = 0, next_repeat = 0;
    bool long_sent = false;
    for (;;) {
        uint8_t bytes[6]{};
        if (!read(cap ? 0 : 0x14, bytes, sizeof(bytes))) {
            // Lost I2C must not turn stale data into a release/click.
            previous = stable = 0;
            stable_count = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint8_t key = bytes[cap ? 3 : 0] & 0x3f;
        if (cap) {
            uint8_t clear[] = {0, uint8_t(bytes[0] & 0xfe)};
            i2c_master_transmit(touch_device_, clear, 2, 50);
        }
        if (key == stable) {
            ++stable_count;
        } else {
            stable = key;
            stable_count = 1;
        }
        auto now = esp_timer_get_time();
        if (stable_count >= 2 && key != previous) {
            if (previous && key == 0 && !long_sent) {
                Beep();
                on_key_(previous);
            }
            previous = key;
            pressed = now;
            next_repeat = now + 500000;
            long_sent = false;
        }
        if (previous && key == previous) {
            if ((key == Up || key == Down) && now >= next_repeat) {
                on_key_(key);
                next_repeat = now + 150000;
                long_sent = true;
            } else if (key != Up && key != Down && !long_sent && now - pressed >= 1500000) {
                Beep();
                on_key_(key | Long);
                long_sent = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
