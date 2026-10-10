#pragma once
#include "display/lcd_display.h"
#include "device_state.h"
#include "xkq_hardware.h"
#include <string>
#include <vector>

struct PanelNode;
struct PanelPage;

class ControllerDisplay : public SpiLcdDisplay {
   public:
    ControllerDisplay(esp_lcd_panel_io_handle_t io,
                      esp_lcd_panel_handle_t panel,
                      int width,
                      int height,
                      int offset_x,
                      int offset_y,
                      bool mirror_x,
                      bool mirror_y,
                      bool swap_xy);
    ~ControllerDisplay() override;
    void OnDeviceStateChanged(DeviceState state) override;
    void ShowNotification(const char* message, int duration_ms = 3000) override;
    void ShowNotification(const std::string& message, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void HandleKey(uint16_t key);

   private:
    struct Binding {
        lv_obj_t* object;
        const PanelNode* node;
        std::string row;
    };
    lv_obj_t* normal_screen_ = nullptr;
    lv_obj_t* chat_screen_ = nullptr;
    lv_obj_t* toast_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_group_t* group_ = nullptr;
    lv_timer_t* refresh_timer_ = nullptr;
    std::vector<Binding> bindings_;
    const PanelPage* page_definition_ = nullptr;
    std::string page_ = "home";
    std::string edit_return_ = "home";
    std::string schedule_return_ = "schedule";
    static constexpr unsigned device_ = 0;
    int draft_temperature_ = 26;
    int schedule_minutes_ = 450;
    int repeat_ = 1;
    bool regular_visible_ = false;
    int64_t toast_until_ = 0;
    void Render(const std::string& page);
    void Refresh();
    void Activate(const std::string& key);
    void Back();
    void Notify(const char* message);
    bool Send(XkqHardware::Field field, int value);
    std::string Text(const PanelNode& node, const std::string& row);
};
