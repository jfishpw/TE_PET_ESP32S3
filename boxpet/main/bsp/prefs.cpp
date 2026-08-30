// bsp/prefs.cpp — NVS 偏好存储（namespace "boxpet"）
// 键：slp_h0/slp_h1 作息；net_cfg 网络配置（WiFi + 小智云）。
#include "prefs.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>

namespace boxpet::bsp {

static const char* TAG = "prefs";

// 小智云默认 WebSocket 地址（官方 OTA/语音接入点）。重置存档 / 首次配网
// 若配置里没有该地址，get_net 自动兜底填充 → 无需手动填写即"默认生效"。
static constexpr const char* kDefaultXzUrl = "wss://api.tenclass.net:443/xiaozhi/v1/";

esp_err_t prefs_init() {
    // NVS 已在 app_main 初始化；此处仅确保分区可用（打开关闭一次）
    nvs_handle_t h;
    esp_err_t err = nvs_open("boxpet", NVS_READWRITE, &h);
    if (err == ESP_OK) nvs_close(h);
    return err;
}

// ===== 作息窗口 =====
void prefs_get_sleep_window(int* start_hour, int* wake_hour) {
    int h0 = 23, h1 = 6;   // 默认 23:00~06:00
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READONLY, &h) == ESP_OK) {
        int8_t v;
        if (nvs_get_i8(h, "slp_h0", &v) == ESP_OK) h0 = v;
        if (nvs_get_i8(h, "slp_h1", &v) == ESP_OK) h1 = v;
        nvs_close(h);
    }
    if (h0 < 0 || h0 > 23) h0 = 23;
    if (h1 < 0 || h1 > 23) h1 = 6;
    if (h0 == h1) h0 = 23;  // 防御：全天窗非法
    if (start_hour) *start_hour = h0;
    if (wake_hour)  *wake_hour  = h1;
}

void prefs_set_sleep_window(int start_hour, int wake_hour) {
    if (start_hour == wake_hour) return;
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "slp_h0", (int8_t)start_hour);
    nvs_set_i8(h, "slp_h1", (int8_t)wake_hour);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "sleep window set %02d:00-%02d:00", start_hour, wake_hour);
}

// ===== 网络配置 =====
bool prefs_get_net(NetConfig* out) {
    if (!out) return false;
    memset(out, 0, sizeof(NetConfig));
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open("boxpet", NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[sizeof(NetConfig)];
        size_t len = sizeof(blob);
        ok = (nvs_get_blob(h, "net_cfg", blob, &len) == ESP_OK
              && len == sizeof(NetConfig));
        nvs_close(h);
        if (ok) memcpy(out, blob, sizeof(NetConfig));
    }
    out->wifi_ssid[sizeof(out->wifi_ssid) - 1] = 0;
    out->wifi_pass[sizeof(out->wifi_pass) - 1] = 0;
    out->xz_url[sizeof(out->xz_url) - 1] = 0;
    // 小智云地址默认兜底：重置存档 / 配网页留空时直接使用官方地址
    if (!out->xz_url[0]) {
        strncpy(out->xz_url, kDefaultXzUrl, sizeof(out->xz_url) - 1);
        out->xz_url[sizeof(out->xz_url) - 1] = 0;
    }
    return out->wifi_ssid[0] != 0;   // 至少要有 SSID 才算有效
}

void prefs_set_net(const NetConfig* in) {
    if (!in) return;
    NetConfig c = *in;
    // 字符串截断保护
    c.wifi_ssid[sizeof(c.wifi_ssid) - 1] = 0;
    c.wifi_pass[sizeof(c.wifi_pass) - 1] = 0;
    c.xz_url[sizeof(c.xz_url) - 1] = 0;
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "net_cfg", &c, sizeof(c));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "net config saved (ssid=%s)", in->wifi_ssid);
}

bool prefs_has_net() {
    NetConfig c;
    return prefs_get_net(&c);
}

// ===== 小智 OTA 注册（token/激活码）=====
static void nvs_set_str(const char* key, const char* val) {
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READWRITE, &h) != ESP_OK) return;
    if (val && val[0]) ::nvs_set_str(h, key, val);
    else ::nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

bool prefs_get_xz_token(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = len;
    esp_err_t e = nvs_get_str(h, "xz_token", out, &sl);
    nvs_close(h);
    if (e != ESP_OK) { out[0] = 0; return false; }
    out[len - 1] = 0;
    return out[0] != 0;
}

void prefs_set_xz_token(const char* in) { nvs_set_str("xz_token", in); }
bool prefs_get_xz_code(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = len;
    esp_err_t e = nvs_get_str(h, "xz_code", out, &sl);
    nvs_close(h);
    if (e != ESP_OK) { out[0] = 0; return false; }
    out[len - 1] = 0;
    return out[0] != 0;
}
void prefs_set_xz_code(const char* in)  { nvs_set_str("xz_code",  in); }

bool prefs_get_xz_client(char* out, size_t len) {
    if (!out || len == 0) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sl = len;
    esp_err_t e = nvs_get_str(h, "xz_cli", out, &sl);
    nvs_close(h);
    if (e != ESP_OK) { out[0] = 0; return false; }
    out[len - 1] = 0;
    return out[0] != 0;
}
void prefs_set_xz_client(const char* in) { nvs_set_str("xz_cli", in); }

}  // namespace boxpet::bsp
