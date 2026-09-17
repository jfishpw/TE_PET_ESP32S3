// bsp/weather.cpp — 天气系统实现（open-meteo 查询 + 离线随机 + NVS 持久化）
//   查询在独立任务（wx_task）里做：不占 UI/宠物 tick 上下文、不碰 LVGL，
//   避免重蹈"在 esp_timer 上下文做重活 → 看门狗"的坑。
#include "weather.h"
#include "net_mgr.h"
#include "prefs.h"
#include "wallclock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "esp_netif_sntp.h"   // 网络对时（SNTP）
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"
#include <cstring>
#include <ctime>

namespace boxpet::bsp {

namespace {

const char* TAG = "weather";

constexpr const char* kNvsNamespace = "boxpet";
constexpr const char* kNvsWeather   = "wx_id";   // u8  当前天气
constexpr const char* kNvsTemp      = "wx_t";    // i16 温度（℃）
constexpr const char* kNvsTempValid = "wx_tv";   // u8  温度是否有效
constexpr const char* kNvsLastQuery = "wx_at";   // i64 上次查询时刻（epoch 秒）

constexpr int64_t kQueryPeriodSec = 2 * 3600;    // 每 2 小时查询/随机刷新一次
constexpr int64_t kFirstDelayMs   = 30 * 1000;   // 亮屏后 30s 才允许首次查询
constexpr int     kHttpTimeoutMs  = 8000;
constexpr int     kStaWaitTicks   = 60;          // 60 × 200ms = 12s 等联网

// 长沙默认坐标（用户未指定城市 → 需求默认 changsha）
const char* kUrl =
    "http://api.open-meteo.com/v1/forecast"
    "?latitude=28.2282&longitude=112.9388"
    "&current=temperature_2m,weather_code,wind_speed_10m"
    "&timezone=Asia%2FShanghai";

Weather           s_weather    = Weather::Sunny;
int               s_temp       = 0;
bool              s_temp_valid = false;
int64_t           s_last_query = 0;      // epoch 秒
volatile uint32_t s_gen        = 1;      // 变化版本号（UI 重绘触发）
volatile bool     s_running    = false;  // 查询任务进行中

// ===== NVS =====
void load_state() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t w = 0, tv = 0;
    int16_t t = 0;
    int64_t at = 0;
    if (nvs_get_u8(h, kNvsWeather, &w) == ESP_OK && w < (uint8_t)Weather::Count)
        s_weather = (Weather)w;
    if (nvs_get_i16(h, kNvsTemp, &t) == ESP_OK) s_temp = t;
    if (nvs_get_u8(h, kNvsTempValid, &tv) == ESP_OK) s_temp_valid = (tv != 0);
    if (nvs_get_i64(h, kNvsLastQuery, &at) == ESP_OK) s_last_query = at;
    nvs_close(h);
}

void save_state() {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, kNvsWeather, (uint8_t)s_weather);
    nvs_set_i16(h, kNvsTemp, (int16_t)s_temp);
    nvs_set_u8(h, kNvsTempValid, s_temp_valid ? 1 : 0);
    nvs_set_i64(h, kNvsLastQuery, wallclock_epoch());
    nvs_commit(h);
    nvs_close(h);
}

// 离线/失败：随机一种天气（不带温度）
void apply_random() {
    s_weather    = (Weather)(esp_random() % (uint32_t)Weather::Count);
    s_temp_valid = false;
    ESP_LOGW(TAG, "no data -> random weather=%s", weather_name(s_weather));
}

// WMO weather_code → 天气形态（wind_km/h 用于识别大风）
Weather code_to_weather(int code, float wind_kmh) {
    if (code >= 95 && code <= 99)                                   return Weather::Thunder;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)     return Weather::Snow;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))   return Weather::Rain;
    if (code == 45 || code == 48)                                   return Weather::Fog;
    if (code >= 30 && code <= 39)                                   return Weather::Fog;   // 沙尘/雾霾
    if (code == 3)                                                  return Weather::Overcast;
    if (code == 1 || code == 2)                                     return Weather::Cloudy;
    if (code == 0)                                                  return Weather::Sunny;
    return Weather::Overcast;
}

bool parse_and_apply(const char* js) {
    cJSON* root = cJSON_Parse(js);
    if (!root) return false;
    cJSON* cur = cJSON_GetObjectItem(root, "current");
    if (!cur) { cJSON_Delete(root); return false; }
    cJSON* jt  = cJSON_GetObjectItem(cur, "temperature_2m");
    cJSON* jc  = cJSON_GetObjectItem(cur, "weather_code");
    cJSON* jw  = cJSON_GetObjectItem(cur, "wind_speed_10m");
    bool ok = cJSON_IsNumber(jt) && cJSON_IsNumber(jc);
    if (ok) {
        float temp = (float)jt->valuedouble;
        int   code = (int)jc->valuedouble;
        float wind = cJSON_IsNumber(jw) ? (float)jw->valuedouble : 0.0f;
        Weather w = code_to_weather(code, wind);
        // 大风（非降水类）优先：风速 ≥25km/h 且不是雨/雪/雷
        if (wind >= 25.0f && w != Weather::Rain && w != Weather::Snow
            && w != Weather::Thunder) w = Weather::Windy;
        s_weather    = w;
        s_temp       = (int)(temp + (temp >= 0 ? 0.5f : -0.5f));
        s_temp_valid = true;
        ESP_LOGI(TAG, "online: code=%d wind=%.1f -> %s %dC", code, wind,
                 weather_name(s_weather), s_temp);
    }
    cJSON_Delete(root);
    return ok;
}

bool fetch_http() {
    char buf[512];
    esp_http_client_config_t cfg = {};
    cfg.url        = kUrl;
    cfg.method     = HTTP_METHOD_GET;
    cfg.timeout_ms = kHttpTimeoutMs;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(c);
        int want = (len > 0 && len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
        int n = esp_http_client_read(c, buf, want);
        if (n > 0) {
            buf[n] = '\0';
            ok = parse_and_apply(buf);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

// 网络对时（NTP）：复用天气查询的联网窗口，把墙钟校准到真实时间。
// 本机按东八区（中国）显示，故用 UTC 时间 + 8h 作为"显示用本地时刻"。
// 目的：深睡期间 RTC 慢时钟（内部 RC）漂移会让时钟积累误差（每晚 1~2 分钟），
// 每次联网顺手校时即可把累积误差清零。
static void try_ntp_sync() {
    // 单服务器即可（多服务器需要放大 CONFIG_LWIP_SNTP_MAX_SERVERS，没必要）
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed");
        return;
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
    time_t now = time(nullptr);
    if (err == ESP_OK && now > 1600000000) {          // 合理时间（晚于 2020-09）
        int64_t delta = wallclock_sync_to_epoch((int64_t)now + 8 * 3600);
        ESP_LOGI(TAG, "ntp ok: %lld (+8h local), corrected %+lld s",
                 (long long)now, (long long)delta);
    } else {
        ESP_LOGW(TAG, "ntp sync timeout/failed (err=0x%x)", (int)err);
    }
    esp_netif_sntp_deinit();
}

// 查询任务：联网（复用 net_mgr）→ HTTP GET → 解析 → 保存 → 挂自己的 WiFi 回来
void wx_task(void*) {
    bool we_connected = false;
    bool ok = false;
    bool has_cred = net_mgr_has_credentials();
    NetMode m0 = net_mgr_mode();
    if (m0 == NetMode::StaConnected) {
        ok = fetch_http();                       // 已联网（如聊天中）直接查
    } else if (has_cred) {
        if (net_mgr_connect_sta() == ESP_OK) {
            we_connected = true;
            for (int i = 0; i < kStaWaitTicks; ++i) {
                vTaskDelay(pdMS_TO_TICKS(200));
                NetMode m = net_mgr_mode();
                if (m == NetMode::StaConnected) { ok = fetch_http(); break; }
                if (m == NetMode::StaFailed)    break;
            }
        } else {
            ESP_LOGW(TAG, "connect_sta failed");
        }
    }

    // 顺手网络对时：有连接就校一次（失败不影响天气流程/不影响功能）
    if (net_mgr_mode() == NetMode::StaConnected) try_ntp_sync();

    if (ok) {
        s_last_query = wallclock_epoch();
        save_state();
    } else if (has_cred) {        // 有凭据但联网/查询失败 → 随机一种天气，并按 2 小时节奏（用户选择）
        apply_random();
        s_last_query = wallclock_epoch();
        save_state();
    } else {
        // 无凭据：不换天气、不写 NVS（避免每次重试都改天气/磨 Flash）；
        // 下次"开机点亮后"会再试一次（配网成功后即自动生效）
        ESP_LOGW(TAG, "no wifi credentials -> keep current weather");
    }
    s_gen++;
    if (we_connected) net_mgr_stop();            // 自己连的自己关（省电）
    s_running = false;
    vTaskDelete(nullptr);
}

}  // namespace

void weather_init() {
    load_state();
    static bool s_first = true;
    if (s_first) {
        s_first = false;
        // 无历史记录（首次上电）：随机一种，避免"晴"默认值看起来没生效
        if (s_last_query == 0) {
            apply_random();
            s_last_query = wallclock_epoch();
            save_state();
        }
    }
    ESP_LOGI(TAG, "init: %s temp=%d%s (last=%lld)", weather_name(s_weather), s_temp,
             s_temp_valid ? "C" : "C?", (long long)s_last_query);
    s_gen++;
}

Weather     weather_current()    { return s_weather; }
int         weather_temp()       { return s_temp; }
bool        weather_temp_valid() { return s_temp_valid; }
uint32_t    weather_generation() { return s_gen; }

const char* weather_name(Weather w) {
    switch (w) {
        case Weather::Sunny:    return "晴";
        case Weather::Cloudy:   return "多云";
        case Weather::Overcast: return "阴";
        case Weather::Rain:     return "雨";
        case Weather::Snow:     return "雪";
        case Weather::Thunder:  return "雷阵雨";
        case Weather::Fog:      return "雾";
        case Weather::Windy:    return "大风";
        default:                return "晴";
    }
}

void weather_maybe_refresh(int64_t screen_on_ms) {
    if (s_running) return;
    if (screen_on_ms < kFirstDelayMs) return;             // 亮屏后 30s 内不联网
    int64_t now = wallclock_epoch();
    bool due_2h = (s_last_query == 0) || (now - s_last_query >= kQueryPeriodSec);
    // 每次开机（点亮后）允许补查一次：深睡醒来/重启后能尽快拿到最新天气，
    // 也保证"配网成功后"无需等满 2 小时即可生效。之后按 2 小时节奏。
    static bool s_boot_tried = false;
    if (!due_2h && s_boot_tried) return;
    s_boot_tried = true;
    s_running = true;
    if (xTaskCreate(wx_task, "wx", 6144, nullptr, 3, nullptr) != pdPASS) {
        s_running = false;
        ESP_LOGW(TAG, "task create failed");
    }
}

}  // namespace boxpet::bsp
