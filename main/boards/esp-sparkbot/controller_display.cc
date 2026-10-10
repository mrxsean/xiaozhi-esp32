#include "controller_display.h"
#include "controller_mode.h"
#include "application.h"
#include "board.h"
#include "wifi_board.h"
#include "wifi_manager.h"
#include "audio_codec.h"
#include <algorithm>
#include <cstring>
#include <ctime>

enum class PanelKind { Box, Text, Image };
struct PanelNode {
    PanelKind kind;
    int x;
    int y;
    int w;
    int h;
    const char* text;
    int size;
    uint32_t fg;
    uint32_t bg;
    int radius;
    const char* action;
    int align;
    const lv_image_dsc_t* image;
    int border;
};
struct PanelPage {
    const char* name;
    const PanelNode* nodes;
    size_t count;
};
#include "controller_pages.inc"

namespace {
const char* DeviceName(unsigned) {
    return "当前设备";
}
const char* ModeName(int mode) {
    switch (mode) {
        case 0:
            return "制热";
        case 1:
            return "制冷";
        case 2:
            return "热水";
        case 3:
            return "制冷热水";
        case 4:
            return "制热热水";
        case 5:
            return "送风";
        case 6:
            return "除湿";
        default:
            return "未知";
    }
}
std::string Temperature(int value) {
    char b[24];
    snprintf(b, sizeof(b), "%.1f °C", value / 10.0);
    return b;
}
bool IsTemperaturePage(const std::string& page) {
    return page == "temp" || page == "temp25" || page == "temp27";
}
}  // namespace

ControllerDisplay::ControllerDisplay(esp_lcd_panel_io_handle_t io,
                                     esp_lcd_panel_handle_t panel,
                                     int width,
                                     int height,
                                     int ox,
                                     int oy,
                                     bool mx,
                                     bool my,
                                     bool swap)
    : SpiLcdDisplay(io, panel, width, height, ox, oy, mx, my, swap) {
    DisplayLockGuard lock(this);
    chat_screen_ = lv_display_get_screen_active(display_);
    normal_screen_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(normal_screen_);
    lv_obj_set_size(normal_screen_, 320, 240);
    lv_obj_set_style_bg_color(normal_screen_, lv_color_hex(0x10191f), 0);
    lv_obj_set_style_bg_opa(normal_screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(normal_screen_, LV_OBJ_FLAG_SCROLLABLE);
    group_ = lv_group_create();
    Render("home");
    refresh_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<ControllerDisplay*>(lv_timer_get_user_data(timer))->Refresh();
        },
        500,
        this);
}

ControllerDisplay::~ControllerDisplay() {
    DisplayLockGuard lock(this);
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
    }
    lv_screen_load(chat_screen_);
    lv_obj_delete(normal_screen_);
    lv_group_delete(group_);
}

void ControllerDisplay::OnDeviceStateChanged(DeviceState state) {
    DisplayLockGuard lock(this);
    // Boot, activation, OTA and configuration retain the existing system UI.
    const bool regular = ShowRegularController(state);
    if (regular == regular_visible_) {
        return;
    }
    regular_visible_ = regular;
    if (regular) {
        Refresh();
    }
    lv_screen_load(regular ? normal_screen_ : chat_screen_);
}

void ControllerDisplay::ShowNotification(const char* message, int duration_ms) {
    SpiLcdDisplay::ShowNotification(message, duration_ms);
    DisplayLockGuard lock(this);
    if (regular_visible_) {
        Notify(message);
        toast_until_ = esp_timer_get_time() + int64_t(duration_ms) * 1000;
    }
}

void ControllerDisplay::ShowNotification(const std::string& message, int duration_ms) {
    ShowNotification(message.c_str(), duration_ms);
}

void ControllerDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    DisplayLockGuard lock(this);
    if (regular_visible_ && role && std::strcmp(role, "system") == 0 && content && content[0]) {
        Notify(content);
    }
}

void ControllerDisplay::Render(const std::string& page) {
    const PanelPage* definition = nullptr;
    for (const auto& p : panel_pages) {
        if (page == p.name) {
            definition = &p;
            break;
        }
    }
    if (!definition) {
        return;
    }
    page_ = page;
    page_definition_ = definition;
    lv_group_remove_all_objs(group_);
    bindings_.clear();
    toast_ = nullptr;
    wifi_icon_ = nullptr;
    lv_obj_clean(normal_screen_);
    std::string row;
    for (size_t i = 0; i < definition->count; ++i) {
        const auto& n = definition->nodes[i];
        lv_obj_t* obj = nullptr;
        if (n.kind == PanelKind::Box) {
            obj = lv_obj_create(normal_screen_);
            lv_obj_remove_style_all(obj);
            lv_obj_set_style_bg_color(obj, lv_color_hex(n.bg), 0);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(obj, n.radius, 0);
            if (!n.action[0] && n.border) {
                lv_obj_set_style_border_width(obj, n.border, 0);
                lv_obj_set_style_border_color(obj, lv_color_hex(0x63e0cb), 0);
            }
            row = n.action;
        } else if (n.kind == PanelKind::Image) {
            obj = lv_image_create(normal_screen_);
            lv_image_set_src(obj, n.image);
            if (n.x == 248 && n.y == 3) {
                wifi_icon_ = obj;
            }
        } else {
            obj = lv_label_create(normal_screen_);
            lv_obj_remove_style_all(obj);
            lv_obj_set_style_text_font(obj, PanelFont(n.size), 0);
            lv_obj_set_style_text_color(obj, lv_color_hex(n.fg), 0);
            lv_obj_set_style_text_align(obj,
                                        n.align == 1   ? LV_TEXT_ALIGN_CENTER
                                        : n.align == 2 ? LV_TEXT_ALIGN_RIGHT
                                                       : LV_TEXT_ALIGN_LEFT,
                                        0);
            lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
            lv_label_set_text(obj, Text(n, row).c_str());
            bindings_.push_back({obj, &n, row});
        }
        lv_obj_set_pos(obj, n.x, n.y);
        lv_obj_set_size(obj, n.w, n.h);
        if (n.kind == PanelKind::Text) {
            // Figma uses a line box; LVGL fonts include ascent and descent.
            int line = PanelFont(n.size)->line_height;
            lv_obj_set_y(obj, n.y + (n.h - line) / 2);
            lv_obj_set_height(obj, std::max(n.h, line));
            if (n.size == 44 && (IsTemperaturePage(page_) || page_ == "schedtime")) {
                lv_obj_set_x(obj, 24);
                lv_obj_set_width(obj, 272);
                lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
            }
            if (page_ == "home" && std::strcmp(n.text, "室温27°") == 0) {
                lv_obj_set_width(obj, 104);
            }
            if (page_ == "home" &&
                (std::strcmp(n.text, "制冷") == 0 || std::strcmp(n.text, "自动风") == 0)) {
                lv_obj_set_x(obj, std::strcmp(n.text, "制冷") == 0 ? 12 : 108);
                lv_obj_set_width(obj, 88);
                lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
            }
            if (std::strcmp(n.text, "在线") == 0 || std::strcmp(n.text, "运行中") == 0) {
                lv_obj_set_x(obj, 228);
                lv_obj_set_width(obj, 80);
                lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_RIGHT, 0);
            }
        }
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        const bool page_number =
            n.action[0] >= '0' && n.action[0] <= '9' && std::strstr(n.action, " / ");
        const bool read_only_row = n.kind == PanelKind::Box && n.y < 196 &&
                                   (page_ == "state1" || page_ == "state2" ||
                                    page_ == "statepage2" || page_ == "run" || page_ == "sensors");
        if (n.action[0] && !page_number && !read_only_row) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(obj, this);
            lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(obj, lv_color_hex(0x63e0cb), LV_STATE_FOCUSED);
            lv_obj_set_style_radius(obj, 6, LV_STATE_FOCUSED);
            lv_group_add_obj(group_, obj);
            lv_obj_add_event_cb(
                obj,
                [](lv_event_t* e) {
                    auto self = static_cast<ControllerDisplay*>(
                        lv_obj_get_user_data(lv_event_get_target_obj(e)));
                    auto node = static_cast<const PanelNode*>(lv_event_get_user_data(e));
                    std::string key = node->action;
                    std::string page = self->page_;
                    Application::GetInstance().Schedule([self, key, page] {
                        DisplayLockGuard lock(self);
                        if (self->regular_visible_ && self->page_ == page) {
                            self->Activate(key);
                        }
                    });
                },
                LV_EVENT_CLICKED,
                const_cast<PanelNode*>(&n));
        }
    }
    // The six-key design always shows the currently selected item.
    if (auto focused = lv_group_get_focused(group_)) {
        lv_obj_add_state(focused, LV_STATE_FOCUSED);
    }
}

void ControllerDisplay::Refresh() {
    if (!regular_visible_) {
        return;
    }
    auto state = XkqHardware::GetInstance().GetState(device_);
    if (state.online && !state.pending && (page_ == "home" || page_ == "off")) {
        const char* desired = state.power ? "home" : "off";
        if (page_ != desired) {
            Render(desired);
            return;
        }
    }
    for (auto& b : bindings_) {
        lv_label_set_text(b.object, Text(*b.node, b.row).c_str());
    }
    if (wifi_icon_) {
        lv_obj_set_style_opa(
            wifi_icon_, WifiManager::GetInstance().IsConnected() ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (toast_ && esp_timer_get_time() > toast_until_) {
        lv_obj_delete(toast_);
        toast_ = nullptr;
    }
}

std::string ControllerDisplay::Text(const PanelNode& n, const std::string& row) {
    std::string text = n.text;
    auto state = XkqHardware::GetInstance().GetState(device_);
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char time_string[24] = "--:--";
    if (local.tm_year >= 125) {
        strftime(time_string, sizeof(time_string), "%H:%M", &local);
    }
    if (n.y < 24 && text == "09:41") {
        return time_string;
    }
    if (text == "在线" || text == "运行中") {
        return state.pending        ? "等待响应"
               : state.write_failed ? "控制失败"
               : !state.online      ? "未连接"
               : state.power        ? "运行中"
                                    : "已关机";
    }
    if (text == "26" && page_ == "home") {
        return state.online ? std::to_string(state.temperature / 10) : "--";
    }
    if (text == "制冷" && page_ == "home") {
        return state.online ? ModeName(state.mode) : "模式 --";
    }
    if (text == "室温27°") {
        return state.online ? std::string("环境") + std::to_string(state.ambient / 10) + "°"
                            : "环境 --°";
    }
    if (text == "自动风") {
        return "风速 --";
    }
    if (text == "当前设备 · 室温 27°C") {
        return std::string(DeviceName(device_)) + " · " +
               (state.online ? Temperature(state.ambient) : "-- °C");
    }
    if (text == "日程仍按设定执行") {
        return "长按确认唤醒小智";
    }
    if (IsTemperaturePage(page_) && n.size == 44) {
        return std::to_string(draft_temperature_) + " °C";
    }
    if (text.find("范围 ") == 0) {
        return "范围 " + std::to_string(XkqHardware::MinimumTemperature(state.mode)) + "–" +
               std::to_string(XkqHardware::MaximumTemperature(state.mode)) + "°C · 步进 1°C";
    }
    if (page_ == "mode" && n.align == 2) {
        int mode = row == "制冷" ? 1 : row == "制热" ? 0 : row == "除湿" ? 6 : 5;
        return state.online && state.mode == mode ? "当前" : "";
    }
    if (page_ == "fan" && n.align == 2) {
        return "未接入";
    }
    if (n.align == 2 && (page_ == "state1" || page_ == "state2" || page_ == "statepage2")) {
        if (!state.online) {
            return "-- °C";
        }
        if (row == "室内温度") {
            return Temperature(state.ambient);
        }
        if (row == "室外温度") {
            return Temperature(state.inlet);
        }
        if (row == "盘管温度") {
            return Temperature(state.outlet);
        }
        if (row == "设定温度") {
            return Temperature(state.temperature);
        }
        return "-- °C";
    }
    // The reference protocol describes a heat pump, not room/coil sensors.
    if ((page_ == "state1" || page_ == "state2") && text == "室内温度") {
        return "环境温度";
    }
    if ((page_ == "state1" || page_ == "state2") && text == "室外温度") {
        return "回水温度";
    }
    if ((page_ == "state1" || page_ == "state2") && text == "盘管温度") {
        return "出水温度";
    }
    if (n.align == 2 && page_ == "run") {
        if (!state.online) {
            return "--";
        }
        if (row == "开关状态") {
            return state.power ? "运行中" : "已关机";
        }
        if (row == "运行模式") {
            return ModeName(state.mode);
        }
        return "未接入";
    }
    if (n.align == 2 && page_ == "sensors") {
        return row == "通讯状态" ? (state.online ? "正常" : "未连接") : "未接入";
    }
    if (n.align == 2 && page_ == "comfort") {
        if (row == "设定温度") {
            return state.online ? Temperature(state.temperature) + " ›" : "-- °C ›";
        }
        if (row == "运行模式") {
            return std::string(ModeName(state.mode)) + " ›";
        }
        return "未接入 ›";
    }
    if (n.align == 2 && (page_ == "system" || page_ == "sysconfig")) {
        return "未接入";
    }
    if (n.align == 2 && page_ == "comm") {
        return row == "设备地址" ? std::to_string(device_ + 1)
               : row == "波特率" ? "9600"
                                 : "无校验";
    }
    if (page_ == "confirm") {
        if (n.align == 2) {
            return std::to_string(draft_temperature_) + " °C";
        }
        if (text.find("将更新") == 0) {
            return std::string("将更新 ") + DeviceName(device_) + " 的设定";
        }
    }
    if (page_ == "faults" && n.align == 2) {
        if (row == "运行状态") {
            return !state.online ? "未连接" : state.power ? "运行中" : "已关机";
        }
        if (row == "E03 通讯异常") {
            return state.online ? "正常" : "未连接";
        }
        return !state.online     ? "--"
               : state.error > 1 ? "E" + std::to_string(state.error) + " ›"
                                 : "无故障";
    }
    if (page_ == "faults" && text == "E01 室温传感器") {
        return "设备故障";
    }
    if (page_ == "faults" && text == "E03 通讯异常") {
        return "通讯状态";
    }
    if (page_ == "faultdetail" || page_ == "empty") {
        if (text.find("E01") == 0) {
            return state.online ? "故障码：" + std::to_string(state.error) : "设备未连接";
        }
        if (text.find("设备：") == 0) {
            return std::string("设备：") + DeviceName(device_);
        }
        if (text.find("发生：") == 0 || text.find("最近更新") == 0) {
            return std::string("最近读取 ") + time_string;
        }
        if (text == "当前未恢复") {
            return !state.online ? "未连接" : state.error > 1 ? "当前未恢复" : "无当前故障";
        }
        if (text == "请联系维护人员检查传感器") {
            return "故障含义请查设备说明";
        }
        if (text == "当前无故障") {
            return state.online ? "当前无故障" : "设备未连接";
        }
    }
    if (page_ == "history" || page_ == "historydetail") {
        if (n.y >= 56 && n.y < 196) {
            return n.y == 80 && n.align == 0 ? "暂无历史记录" : "";
        }
    }
    if (page_ == "schedules") {
        if (n.align == 2) {
            return "未接入 ›";
        }
    }
    if ((page_ == "schedule" || page_ == "timer") && n.align == 2 && row == "执行时间") {
        char value[32];
        snprintf(
            value, sizeof(value), "%02d:%02d ›", schedule_minutes_ / 60, schedule_minutes_ % 60);
        return value;
    }
    if (page_ == "schedule" && n.align == 2 && row == "重复周期") {
        if (repeat_ == 0) {
            return "仅一次 ›";
        }
        if (repeat_ == 2) {
            return "每天 ›";
        }
        return "工作日 ›";
    }
    if (page_ == "schedsave" && n.y == 56) {
        char value[64];
        const char* cycle = repeat_ == 0 ? "仅一次" : repeat_ == 2 ? "每天" : "周一至周五";
        snprintf(value,
                 sizeof(value),
                 "%02d:%02d · %s",
                 schedule_minutes_ / 60,
                 schedule_minutes_ % 60,
                 cycle);
        return value;
    }
    if (page_ == "schedtime" && n.size == 44) {
        char b[16];
        snprintf(b, sizeof(b), "%02d : %02d", schedule_minutes_ / 60, schedule_minutes_ % 60);
        return b;
    }
    if (page_ == "repeat" && n.align == 2) {
        return (row == "仅一次" ? 0 : row == "每天" ? 2 : 1) == repeat_ ? "已选 ✓" : "○";
    }
    if (page_ == "network" && n.align == 2) {
        return WifiManager::GetInstance().IsConnected() ? WifiManager::GetInstance().GetSsid()
                                                        : "未连接";
    }
    if (page_ == "users" && n.align == 2 && row == "配网") {
        return WifiManager::GetInstance().IsConnected() ? "已连接 ›" : "未连接 ›";
    }
    if ((page_ == "users" && row == "时间设置" && n.align == 2) ||
        (page_ == "time" && row == "时间" && n.align == 2)) {
        return time_string;
    }
    if (page_ == "time" && row == "日期" && n.align == 2) {
        if (local.tm_year < 125) {
            return "未校时";
        }
        strftime(time_string, sizeof(time_string), "%Y-%m-%d", &local);
        return time_string;
    }
    if (page_ == "voiceprefs") {
        if (n.align == 2 && row == "播报音量") {
            return std::to_string(Board::GetInstance().GetAudioCodec()->output_volume()) + "% ›";
        }
        if (text.find("唤醒词：") == 0) {
            return "按住确认键可唤醒小智";
        }
    }
    return text;
}

void ControllerDisplay::Notify(const char* message) {
    if (toast_) {
        lv_obj_delete(toast_);
    }
    toast_ = lv_label_create(normal_screen_);
    lv_obj_set_size(toast_, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(toast_, &panel_font_14, 0);
    lv_obj_set_style_text_align(toast_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(toast_, lv_color_hex(0xf2f7fa), 0);
    lv_obj_set_style_bg_color(toast_, lv_color_hex(0x263842), 0);
    lv_obj_set_style_bg_opa(toast_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(toast_, 8, 0);
    lv_obj_set_style_radius(toast_, 6, 0);
    lv_label_set_text(toast_, message);
    lv_obj_align(toast_, LV_ALIGN_CENTER, 0, 0);
    toast_until_ = esp_timer_get_time() + 2500000;
}

bool ControllerDisplay::Send(XkqHardware::Field field, int value) {
    if (field == XkqHardware::Field::Temperature &&
        !XkqHardware::SupportsTemperature(XkqHardware::GetInstance().GetState(device_).mode)) {
        Notify("此模式的温度控制未接入");
        return false;
    }
    bool queued = XkqHardware::GetInstance().Set(device_, field, value);
    Notify(queued ? "已发送，等待设备响应" : "设备未连接或正在处理");
    return queued;
}

void ControllerDisplay::Back() {
    if (page_ == "home" || page_ == "off") {
        return;
    }
    if (page_ == "mode" || page_ == "fan" || IsTemperaturePage(page_)) {
        Render(edit_return_);
    } else if (page_ == "confirm") {
        Render("temp");
    } else if (page_ == "state1" || page_ == "state2" || page_ == "statepage2" || page_ == "run" ||
               page_ == "sensors") {
        Render("groups");
    } else if (page_ == "comfort" || page_ == "system" || page_ == "comm" || page_ == "sysconfig") {
        Render("settings");
    } else if (page_ == "time" || page_ == "network" || page_ == "language" ||
               page_ == "voiceprefs") {
        Render("users");
    } else if (page_ == "schedule" || page_ == "timer") {
        Render("schedules");
    } else if (page_ == "schedtime") {
        Render(schedule_return_);
    } else if (page_ == "repeat" || page_ == "action" || page_ == "schedsave") {
        Render("schedule");
    } else if (page_ == "faultdetail" || page_ == "empty") {
        Render("faults");
    } else if (page_ == "historydetail") {
        Render("history");
    } else if (page_ == "menu") {
        Render("home");
    } else {
        Render("menu");
    }
}

void ControllerDisplay::Activate(const std::string& key) {
    auto state = XkqHardware::GetInstance().GetState(device_);

    if (key == "语音" || key == "语音控制" || key == "试试语音") {
        Application::GetInstance().ToggleChatState();
        return;
    }
    if (key == "设备开机" || key == "设备关机") {
        Send(XkqHardware::Field::Power, key == "设备开机");
        return;
    }
    if (key == "＋" || key == "−") {
        const int delta = key == "＋" ? 1 : -1;
        if (IsTemperaturePage(page_)) {
            draft_temperature_ = std::clamp(draft_temperature_ + delta,
                                            XkqHardware::MinimumTemperature(state.mode),
                                            XkqHardware::MaximumTemperature(state.mode));
        } else if (page_ == "schedtime") {
            schedule_minutes_ = (schedule_minutes_ + delta + 1440) % 1440;
        }
        Refresh();
        return;
    }
    if (page_ == "mode") {
        int mode;
        if (key == "制冷") {
            mode = 1;
        } else if (key == "制热") {
            mode = 0;
        } else if (key == "送风") {
            mode = 5;
        } else if (key == "除湿") {
            mode = 6;
        } else {
            return;
        }
        if (Send(XkqHardware::Field::Mode, mode)) {
            Render(edit_return_);
            Notify("已发送，等待设备响应");
        }
        return;
    }
    if ((page_ == "home" && key == "制冷") || key == "运行模式") {
        edit_return_ = page_;
        Render("mode");
        return;
    }
    if (key == "自动风" || key == "设定风速") {
        edit_return_ = page_;
        Render("fan");
        return;
    }
    if (key == "设定温度" && (page_ == "home" || page_ == "comfort")) {
        if (!XkqHardware::SupportsTemperature(state.mode)) {
            Notify("此模式的温度控制未接入");
            return;
        }
        edit_return_ = page_;
        draft_temperature_ = std::clamp(state.temperature / 10,
                                        XkqHardware::MinimumTemperature(state.mode),
                                        XkqHardware::MaximumTemperature(state.mode));
        Render("temp");
        return;
    }
    if (IsTemperaturePage(page_) && key == "保存") {
        Render("confirm");
        return;
    }
    if (page_ == "confirm" && key == "确认保存") {
        if (Send(XkqHardware::Field::Temperature, draft_temperature_ * 10)) {
            Render(edit_return_);
            Notify("已发送，等待设备响应");
        }
        return;
    }
    if (key.find("下一页") == 0) {
        Render("system");
        return;
    }
    if (key.find("上一页") == 0) {
        Render("comfort");
        return;
    }
    if (page_ == "faults") {
        if (key == "运行状态") {
            Render("run");
        } else {
            Render(state.online && state.error <= 1 ? "empty" : "faultdetail");
        }
        return;
    }
    if (key == "开始配网") {
        Application::GetInstance().Schedule([] {
            static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
        });
        return;
    }
    if (page_ == "repeat") {
        if (key == "保存周期") {
            Render("schedule");
        } else {
            repeat_ = key == "仅一次" ? 0 : key == "每天" ? 2 : 1;
            Refresh();
        }
        return;
    }
    if (page_ == "schedtime" && key == "确定") {
        Render(schedule_return_);
        return;
    }
    if (page_ == "voiceprefs" && key == "播报音量") {
        auto codec = Board::GetInstance().GetAudioCodec();
        codec->SetOutputVolume((codec->output_volume() + 10) % 110);
        Refresh();
        return;
    }
    if (key == "执行时间") {
        schedule_return_ = page_;
        Render("schedtime");
        return;
    }

    struct Route {
        const char* label;
        const char* page;
    };
    static const Route routes[] = {
        {"状态参数", "groups"},     {"设置参数", "settings"},   {"定时/日程", "schedules"},
        {"当前故障", "faults"},     {"历史故障", "history"},    {"用户设置", "users"},
        {"温度参数", "state1"},     {"运行参数", "run"},        {"传感器状态", "sensors"},
        {"舒适设定", "comfort"},    {"系统设定", "sysconfig"},  {"通讯设定", "comm"},
        {"时间设置", "time"},       {"配网", "network"},        {"语言设置", "language"},
        {"语音设置", "voiceprefs"}, {"07:30 开机", "schedule"}, {"22:00 关机", "schedule"},
        {"新增日程", "schedule"},   {"单次定时", "timer"},      {"重复周期", "repeat"},
        {"控制动作", "action"},     {"动作", "action"},         {"下一步", "schedsave"},
    };
    for (const auto& route : routes) {
        if (key == route.label) {
            Render(route.page);
            return;
        }
    }

    if (page_ == "language") {
        Notify("界面语言由固件配置决定");
    } else if (page_ == "time") {
        Notify("联网后由系统自动校时");
    } else if (page_ == "voiceprefs") {
        Notify("语音功能使用当前固件配置");
    } else if (page_ == "history") {
        Notify("暂无历史记录");
    } else {
        Notify("此功能尚未接入设备协议");
    }
}

void ControllerDisplay::HandleKey(uint16_t key) {
    DisplayLockGuard lock(this);

    if (key == (XkqHardware::Ok | XkqHardware::Long)) {
        Application::GetInstance().ToggleChatState();
        return;
    }
    if (!regular_visible_) {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state != kDeviceStateConnecting && state != kDeviceStateListening &&
            state != kDeviceStateSpeaking) {
            return;
        }
        if (key == XkqHardware::Ok) {
            app.ToggleChatState();
        } else if (key == XkqHardware::Back || key == XkqHardware::Menu ||
                   key == XkqHardware::Power) {
            if (key == XkqHardware::Menu) {
                Render("menu");
            } else if (key == XkqHardware::Power) {
                Render("home");
            }
            app.EndConversation();
        }
        return;
    }
    // The physical power key returns home; equipment power is a home action.
    if (key == XkqHardware::Power) {
        auto state = XkqHardware::GetInstance().GetState(device_);
        Render(state.online && !state.power ? "off" : "home");
        return;
    }
    if (key == XkqHardware::Menu) {
        Render("menu");
        return;
    }
    if (key == XkqHardware::Back) {
        Back();
        return;
    }
    if (key == XkqHardware::Up || key == XkqHardware::Down) {
        if (IsTemperaturePage(page_) || page_ == "schedtime") {
            Activate(key == XkqHardware::Up ? "＋" : "−");
        } else if (page_ == "state1" || page_ == "statepage2") {
            Render(page_ == "state1" ? "statepage2" : "state1");
        } else if (key == XkqHardware::Up) {
            lv_group_focus_prev(group_);
        } else {
            lv_group_focus_next(group_);
        }
        return;
    }
    if (key != XkqHardware::Ok) {
        return;
    }

    if (IsTemperaturePage(page_)) {
        Activate("保存");
    } else if (page_ == "confirm") {
        Activate("确认保存");
    } else if (page_ == "schedtime") {
        Activate("确定");
    } else if (page_ == "network") {
        Activate("开始配网");
    } else if (page_ == "schedsave") {
        Notify("日程执行尚未接入设备");
    } else if (page_ == "faultdetail") {
        Render("history");
    } else if (page_ == "historydetail") {
        Render("faults");
    } else if (page_ == "empty") {
        Render("groups");
    } else if (page_ == "networkdone") {
        Render("users");
    } else if (page_ == "state1" || page_ == "statepage2" || page_ == "run" || page_ == "sensors" ||
               page_ == "comm") {
        Notify("当前参数为设备只读数据");
    } else if (auto focused = lv_group_get_focused(group_)) {
        lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
    }
}
