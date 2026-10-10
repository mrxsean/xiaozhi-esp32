#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "controller_display.h"
#include "xkq_hardware.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <cstring>

#include "esp_video.h"

#define TAG "esp_sparkbot"

class SparkBotEs8311AudioCodec : public Es8311AudioCodec {
private:    

public:
    SparkBotEs8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
                        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true)
        : Es8311AudioCodec(i2c_master_handle, i2c_port, input_sample_rate, output_sample_rate,
                             mclk,  bclk,  ws,  dout,  din,pa_pin,  es8311_addr,  use_mclk = true) {}

    void EnableOutput(bool enable) override {
        if (enable == output_enabled_) {
            return;
        }
        if (enable) {
            Es8311AudioCodec::EnableOutput(enable);
        } else {
           // Nothing todo because the display io and PA io conflict
        }
    }
};

class EspSparkBot : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Display* display_;
    EspVideo* camera_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_GPIO;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_GPIO;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_GPIO;
        io_config.dc_gpio_num = DISPLAY_DC_GPIO;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new ControllerDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        // 开机
        mcp_server.AddTool(
            "self.ac.power_on",
            "打开地址1的空调/热泵，通过485发送；返回排队状态，实际结果请查询状态",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return std::string(XkqHardware::GetInstance().Set(0, XkqHardware::Field::Power, 1)
                                       ? "指令已排队，等待设备响应"
                                       : "设备未连接或正在处理，未执行");
            });

        // 关机
        mcp_server.AddTool(
            "self.ac.power_off",
            "关闭地址1的空调/热泵，通过485发送；返回排队状态，实际结果请查询状态",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                return std::string(XkqHardware::GetInstance().Set(0, XkqHardware::Field::Power, 0)
                                       ? "指令已排队，等待设备响应"
                                       : "设备未连接或正在处理，未执行");
            });

        mcp_server.AddTool(
            "self.ac.get_state",
            "读取当前485设备的真实状态，设备地址为1",
            PropertyList({Property("device", kPropertyTypeInteger, 1, 1, 1)}),
            [](const PropertyList& p) -> ReturnValue {
                auto s = XkqHardware::GetInstance().GetState(p["device"].value<int>() - 1);
                cJSON* result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "online", s.online);
                cJSON_AddBoolToObject(result, "pending", s.pending);
                cJSON_AddBoolToObject(result, "write_failed", s.write_failed);
                if (s.online) {
                    cJSON_AddBoolToObject(result, "power", s.power);
                    cJSON_AddNumberToObject(result, "mode", s.mode);
                    cJSON_AddNumberToObject(result, "temperature", s.temperature / 10.0);
                    cJSON_AddNumberToObject(result, "outlet", s.outlet / 10.0);
                    cJSON_AddNumberToObject(result, "inlet", s.inlet / 10.0);
                    cJSON_AddNumberToObject(result, "ambient", s.ambient / 10.0);
                    cJSON_AddNumberToObject(result, "error_code", s.error);
                }
                return result;
            });
        mcp_server.AddTool(
            "self.ac.set_temperature",
            "设置485设备温度，制冷5至25度，制热25至55度；返回排队状态",
            PropertyList({Property("temperature", kPropertyTypeInteger, 5, 55),
                          Property("device", kPropertyTypeInteger, 1, 1, 1)}),
            [](const PropertyList& p) -> ReturnValue {
                return std::string(
                    XkqHardware::GetInstance().Set(p["device"].value<int>() - 1,
                                                   XkqHardware::Field::Temperature,
                                                   p["temperature"].value<int>() * 10)
                        ? "指令已排队，等待设备响应"
                        : "未执行：设备离线、忙或温度超出当前模式范围");
            });
        mcp_server.AddTool(
            "self.ac.set_mode",
            "设置485设备模式：0制热、1制冷、5送风、6除湿；返回排队状态",
            PropertyList({Property("mode", kPropertyTypeInteger, 0, 6),
                          Property("device", kPropertyTypeInteger, 1, 1, 1)}),
            [](const PropertyList& p) -> ReturnValue {
                return std::string(XkqHardware::GetInstance().Set(p["device"].value<int>() - 1,
                                                                  XkqHardware::Field::Mode,
                                                                  p["mode"].value<int>())
                                       ? "指令已排队，等待设备响应"
                                       : "未执行：设备离线、忙或模式不支持");
            });

        // //获取开关机状态
        // mcp_server.AddTool(
        //     "self.ac.get_power_state",
        //     "获取开关机状态",
        //     PropertyList(),
        //     [this](const PropertyList&) -> ReturnValue {

        //         bool state = 0;//this->ac.isPowerOn();

        //         ReturnValue ret;
        //         ret["power"] = state ? "on" : "off";
        //         return ret;
        //     }
        // );

        // // 调温度
        // mcp_server.AddTool(
        //     "self.ac.set_temperature",
        //     "设置温度",
        //     PropertyList{
        //         Property("temp", PropertyType::Int, "目标温度")
        //     },
        //     [this](const PropertyList& props) {
        //         int temp = props.GetInt("temp");
        //         this->ac.setTemperature(temp);
        //         return ReturnValue::from("temperature set");
        //     }
        // );

        // // 获取设定温度
        // mcp_server.AddTool(
        //     "self.ac.get_temperature",
        //     "获取设定温度",
        //     PropertyList(),
        //     [this](const PropertyList&) -> ReturnValue {

        //         int temp = this->ac.getTemperature();

        //         ReturnValue ret;
        //         ret["temperature"] = temp;
        //         return ret;
        //     }
        // );


        // // 改模式
        // mcp_server.AddTool(
        //     "self.ac.set_mode",
        //     "设置模式",
        //     PropertyList{
        //         Property("mode", PropertyType::String, "模式: cool/heat/auto/dry/fan")
        //     },
        //     [this](const PropertyList& props) {
        //         std::string mode = props.GetString("mode");
        //         this->ac.setMode(mode);
        //         return ReturnValue::from("mode set");
        //     }
        // );
    }

public:
    EspSparkBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        XkqHardware::GetInstance().Start([this](uint16_t key) {
            Application::GetInstance().Schedule([this, key]() {
                static_cast<ControllerDisplay*>(display_)->HandleKey(key);
            });
        });
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
         static SparkBotEs8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EspSparkBot);
