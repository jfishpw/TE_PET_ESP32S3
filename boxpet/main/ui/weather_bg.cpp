// ui/weather_bg.cpp — 天气背景层实现（画布 2x2 像素块绘制，1 次 invalidate/帧）
#include "weather_bg.h"
#include "ui_font_16.h"
#include "bsp/weather.h"
#include "bsp/wallclock.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <cstdio>

namespace boxpet::ui {

namespace {

using bsp::Weather;

constexpr int kW  = 240;          // 画布宽（整屏）
constexpr int kH  = 124;          // 画布高（天空 92 + 草地 32）
constexpr int kSC = 2;            // 像素块边长 → 逻辑网格 120 x 62
constexpr int kGW = kW / kSC;     // 120
constexpr int kGH = kH / kSC;     // 62
constexpr int kSkyRows = 46;      // 天空占前 46 行（92px）

lv_obj_t*   s_cv  = nullptr;
lv_obj_t*   s_lbl = nullptr;
lv_color_t* s_buf = nullptr;
uint32_t    s_last_gen  = 0;
bool        s_last_light = true;
bool        s_ready = false;

// 动画元素（逻辑网格坐标）
struct Drop { int16_t x, y; };
Drop s_rain[26];
Drop s_snow[22];
int  s_cloud_x[3] = {10, 48, 86};
int  s_wind_x = 0;
int  s_fog_x  = 0;
int64_t s_next_flash_ms = 0;
bool    s_flash = false;

const lv_color_t C_DAY_SKY    = lv_color_hex(0xBFE3F5);
const lv_color_t C_NIGHT_SKY  = lv_color_hex(0x25315F);
const lv_color_t C_DAY_GRASS  = lv_color_hex(0x8CD08C);
const lv_color_t C_NIGHT_GRASS= lv_color_hex(0x2C4A2C);
const lv_color_t C_RAIN       = lv_color_hex(0x51606E);
const lv_color_t C_FLASH      = lv_color_hex(0xFFFFFF);
const lv_color_t C_SNOW       = lv_color_hex(0xFFFFFF);
const lv_color_t C_CLOUD      = lv_color_hex(0xF2F6F8);
const lv_color_t C_CLOUD_DARK = lv_color_hex(0xD3DADF);
const lv_color_t C_WIND       = lv_color_hex(0xE8EEF2);
const lv_color_t C_FOG        = lv_color_hex(0xE6EBEE);

// 天气 → 天空底色（随昼夜）
lv_color_t sky_color_for(Weather w, bool light) {
    switch (w) {
        case Weather::Overcast: return lv_color_hex(light ? 0xA8B2B8 : 0x2A3242);
        case Weather::Rain:
        case Weather::Thunder:  return lv_color_hex(light ? 0x7C8894 : 0x232B3A);
        case Weather::Snow:     return lv_color_hex(light ? 0xCFDDE8 : 0x2A3550);
        case Weather::Fog:      return lv_color_hex(light ? 0xC3CBD1 : 0x2E3850);
        default:                return light ? C_DAY_SKY : C_NIGHT_SKY;
    }
}

inline void put(int gx, int gy, lv_color_t c) {
    if (gx < 0 || gy < 0 || gx >= kGW || gy >= kGH) return;
    int bx = gx * kSC, by = gy * kSC;
    for (int dy = 0; dy < kSC; ++dy)
        for (int dx = 0; dx < kSC; ++dx)
            lv_canvas_set_px(s_cv, bx + dx, by + dy, c, LV_OPA_COVER);
}

void init_particles() {
    for (auto& r : s_rain) { r.x = (int16_t)(esp_random() % kGW); r.y = (int16_t)(esp_random() % kSkyRows); }
    for (auto& f : s_snow) { f.x = (int16_t)(esp_random() % kGW); f.y = (int16_t)(esp_random() % kSkyRows); }
    s_cloud_x[0] = 6;  s_cloud_x[1] = 44; s_cloud_x[2] = 82;
    s_wind_x = 0;
    s_fog_x  = 0;
}

// 云朵（横向 10 格、高 2~3 行的圆润块）
void draw_cloud(int gx, int gy, lv_color_t c, int w) {
    for (int i = 0; i < w; ++i) {
        put(gx + i, gy, c);
        if (i >= 1 && i < w - 1) { put(gx + i, gy - 1, c); put(gx + i, gy + 1, c); }
    }
}

void render(Weather w, bool light, int64_t now_ms) {
    // 背景
    lv_canvas_fill_bg(s_cv, sky_color_for(w, light), LV_OPA_COVER);
    lv_color_t grass = light ? C_DAY_GRASS : C_NIGHT_GRASS;
    for (int y = kSkyRows; y < kGH; ++y)
        for (int x = 0; x < kGW; ++x) put(x, y, grass);

    switch (w) {
        case Weather::Sunny:
            break;
        case Weather::Cloudy:
            for (int i = 0; i < 3; ++i)
                draw_cloud(s_cloud_x[i], 12 + (i % 2) * 10, i == 1 ? C_CLOUD_DARK : C_CLOUD, 14);
            break;
        case Weather::Overcast:
            for (int i = 0; i < 3; ++i)
                draw_cloud(s_cloud_x[i], 10 + (i % 2) * 12, C_CLOUD_DARK, 18);
            break;
        case Weather::Rain:
        case Weather::Thunder:
            for (auto& r : s_rain) { put(r.x, r.y, C_RAIN); put(r.x, r.y + 1, C_RAIN); }
            break;
        case Weather::Snow:
            for (auto& f : s_snow) { put(f.x, f.y, C_SNOW); put(f.x + 1, f.y, C_SNOW); }
            break;
        case Weather::Fog:
            for (int y = 8; y < kSkyRows - 2; y += 6) {
                int off = (y * 3 + s_fog_x) % 20;
                for (int x = off - 10; x < kGW; x += 20) {
                    for (int i = 0; i < 14; ++i) put(x + i, y, C_FOG);
                }
            }
            break;
        case Weather::Windy:
            for (int i = 0; i < 7; ++i) {
                int y = 6 + i * 5;
                int x = (s_wind_x + i * 17) % (kGW + 16) - 16;
                for (int k = 0; k < 12; ++k) put(x + k, y + k / 6, C_WIND);   // 斜向风线
            }
            break;
        default: break;
    }
    // 雷阵雨：每 4 秒闪一次白（持续 1 帧）
    if (w == Weather::Thunder) {
        if (now_ms >= s_next_flash_ms) { s_next_flash_ms = now_ms + 4000; s_flash = true; }
        if (s_flash) {
            s_flash = false;
            for (int y = 0; y < kSkyRows; ++y)
                for (int x = 0; x < kGW; ++x) put(x, y, C_FLASH);
        }
    }
    lv_obj_invalidate(s_cv);
}

// 动画推进（逻辑网格步进）
void step_anim(Weather w) {
    switch (w) {
        case Weather::Cloudy:
            for (int i = 0; i < 3; ++i) s_cloud_x[i] = (s_cloud_x[i] + 1 + i) % (kGW + 20) - 10;
            break;
        case Weather::Overcast:
            for (int i = 0; i < 3; ++i) s_cloud_x[i] = (s_cloud_x[i] + 1) % (kGW + 24) - 12;
            break;
        case Weather::Rain:
        case Weather::Thunder:
            for (auto& r : s_rain) { r.y += 4; if (r.y >= kSkyRows) { r.y = 0; r.x = (int16_t)(esp_random() % kGW); } }
            break;
        case Weather::Snow:
            for (auto& f : s_snow) {
                f.y += 1;
                f.x += ((f.y & 1) ? 1 : -1);
                if (f.x < 0) f.x = kGW - 1;
                if (f.x >= kGW) f.x = 0;
                if (f.y >= kSkyRows) { f.y = 0; f.x = (int16_t)(esp_random() % kGW); }
            }
            break;
        case Weather::Fog:   s_fog_x = (s_fog_x + 1) % 20; break;
        case Weather::Windy: s_wind_x = (s_wind_x + 4) % (kGW + 16); break;
        default: break;
    }
}

bool animated(Weather w) { return w != Weather::Sunny; }

void update_label(bool light) {
    if (!s_lbl) return;
    char b[24];
    if (bsp::weather_temp_valid())
        snprintf(b, sizeof(b), "%s %d°", bsp::weather_name(bsp::weather_current()), bsp::weather_temp());
    else
        snprintf(b, sizeof(b), "%s", bsp::weather_name(bsp::weather_current()));
    lv_label_set_text(s_lbl, b);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(light ? 0x203040 : 0xE0E8F0), 0);
}

}  // namespace

void weather_bg_create(lv_obj_t* parent) {
    s_buf = (lv_color_t*)heap_caps_malloc(kW * kH * sizeof(lv_color_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) return;
    s_cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_cv, s_buf, kW, kH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_cv, kW, kH);
    lv_obj_set_pos(s_cv, 0, 70);
    lv_obj_set_style_bg_opa(s_cv, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_cv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_cv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cv, [](lv_event_t* e) {
        void* b = lv_event_get_user_data(e);
        if (b) heap_caps_free(b);
    }, LV_EVENT_DELETE, s_buf);

    // 天气 + 温度标签（画布之上，天空区左上）
    s_lbl = lv_label_create(parent);
    lv_obj_set_pos(s_lbl, 4, 72);
    lv_obj_set_style_text_font(s_lbl, ui_font_16, 0);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(0x203040), 0);
    lv_label_set_text(s_lbl, "");

    init_particles();
    s_ready = true;
}

void weather_bg_update(int64_t now_ms, bool light_on) {
    if (!s_ready) return;
    uint32_t gen = bsp::weather_generation();
    bsp::Weather w = bsp::weather_current();
    bool need = false;
    if (gen != s_last_gen || light_on != s_last_light) {
        s_last_gen   = gen;
        s_last_light = light_on;
        need = true;
    }
    if (animated(w)) {
        static int div = 0;
        if (++div >= 2) { div = 0; step_anim(w); need = true; }   // 5Hz 动画
    }
    if (!need) return;
    render(w, light_on, now_ms);
    update_label(light_on);
}

// 天气感知底色（供宠物/图标画布作底，避免露出与背景不一致的方块）
lv_color_t weather_bg_sky_color(bool light_on) {
    return sky_color_for(bsp::weather_current(), light_on);
}

lv_color_t weather_bg_grass_color(bool light_on) {
    return light_on ? C_DAY_GRASS : C_NIGHT_GRASS;
}

}  // namespace boxpet::ui
