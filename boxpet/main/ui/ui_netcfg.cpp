// ui/ui_netcfg.cpp — 配网模式提示页（需求4）
// 屏幕：热点名 Pet-XXXX / 密码 12345678 / 浏览器打开 192.168.4.1
// 服务：bsp::net_mgr_start_ap()（SoftAP + HTTPD，5 分钟无人配置自动关闭）
#include "ui_netcfg.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "bsp/net_mgr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <cstdio>

static const char* TAG = "netcfg";

namespace boxpet::ui {

namespace {

struct NetUi {
    lv_obj_t* root = nullptr;
    bool want_leave = false;
};
static NetUi s;

static void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        s.want_leave = true;
        bsp::audio_play(bsp::Sound::Beep);
    }
}

static lv_obj_t* make_line(lv_obj_t* parent, int y, const char* text,
                           lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 224);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, ui_font_16, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

}  // namespace

lv_obj_t* ui_netcfg_create() {
    s.want_leave = false;
    s.root = lv_obj_create(nullptr);
    lv_obj_set_size(s.root, 240, 240);
    lv_obj_set_style_bg_color(s.root, lv_color_hex(0x24303F), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    // 启动 AP + Web 服务（5 分钟超时自动关，见 net_mgr）
    if (bsp::net_mgr_start_ap() != ESP_OK) {
        ESP_LOGE(TAG, "start ap failed");
    }

    // 热点名（与 net_mgr 一致：Pet- + MAC 后 4 位）
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[64];
    make_line(s.root, 30, "配网模式", lv_color_hex(0xFFD966));
    snprintf(buf, sizeof(buf), "1 手机连 WiFi:");
    make_line(s.root, 62, buf, lv_color_hex(0xFFFFFF));
    snprintf(buf, sizeof(buf), "   Pet-%02X%02X", mac[4], mac[5]);
    make_line(s.root, 86, buf, lv_color_hex(0x8ED6F0));
    snprintf(buf, sizeof(buf), "   密码 %s", "12345678");
    make_line(s.root, 110, buf, lv_color_hex(0x8ED6F0));
    make_line(s.root, 140, "2 浏览器打开:", lv_color_hex(0xFFFFFF));
    make_line(s.root, 164, "   192.168.4.1", lv_color_hex(0x8ED6F0));
    make_line(s.root, 192, "保存后设备自动重启", lv_color_hex(0xA0B0C0));

    // 底栏
    lv_obj_t* bot = lv_obj_create(s.root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hint = lv_label_create(bot);
    lv_label_set_text(hint, "长按中键退出");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(hint, ui_font_16, 0);
    lv_obj_center(hint);

    bsp::buttons_set_callback(on_key);
    bsp::audio_play(bsp::Sound::Beep);
    return s.root;
}

void ui_netcfg_close() {
    bsp::net_mgr_stop();   // 退出即关 AP（配网完成由 /save 重启生效）
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);
        lvgl_port_unlock();
    }
    s.root = nullptr;
}

bool ui_netcfg_wants_leave() { return s.want_leave; }
void ui_netcfg_clear_leave_flag() { s.want_leave = false; }

}  // namespace boxpet::ui
