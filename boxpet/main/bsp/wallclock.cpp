// bsp/wallclock.cpp — 真实时间墙钟实现
// 持久化策略：NVS 存"保存时刻的时钟读数"快照（键 wc_ep）。
// 重启后 显示时间 = 开机时长 + 快照值，误差仅为"上次保存→重启"的间隔。
// 保存时机：调时间后 2s 防抖 + 每 10 分钟周期快照 → 崩溃后时间最多回拨 10 分钟。
// （旧方案存"偏移量"，重启后 esp_timer 归零，时间会倒退回上次调整的时刻）
#include "wallclock.h"
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"

namespace boxpet::bsp {

static const char* TAG = "wallclock";
static int64_t s_offset_sec = 0;  // epoch 秒 - esp_timer 秒
static esp_timer_handle_t s_save_timer = nullptr;      // 调整后 2s 防抖保存
static esp_timer_handle_t s_snapshot_timer = nullptr;  // 周期快照（10 min）

// 默认基准：今天 12:00（epoch 固定值，运行后由用户校准）
static constexpr int64_t kDefaultEpochNoon = 12 * 3600;
static constexpr int64_t kSnapshotPeriodSec = 600;  // 10 分钟

static int64_t now_epoch_sec() {
    return esp_timer_get_time() / 1000000 + s_offset_sec;
}

// 保存当前时钟读数快照（重启后从该时刻继续走）
static void save_snapshot() {
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "wc_ep", now_epoch_sec());
    nvs_commit(h);
    nvs_close(h);
}

static void save_timer_cb(void*) { save_snapshot(); }
static void snapshot_timer_cb(void*) { save_snapshot(); }

void wallclock_init() {
    // 调整防抖定时器
    esp_timer_create_args_t cfg = {
        .callback = save_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wc_save",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &s_save_timer);

    // 读取快照：s_offset = 快照值（开机时 esp_timer=0，显示时间从快照时刻继续）
    nvs_handle_t h;
    bool loaded = false;
    if (nvs_open("boxpet", NVS_READONLY, &h) == ESP_OK) {
        int64_t v = 0;
        if (nvs_get_i64(h, "wc_ep", &v) == ESP_OK) {
            s_offset_sec = v;
            loaded = true;
        }
        nvs_close(h);
    }
    if (loaded) {
        int hh, mm, ss;
        wallclock_now(&hh, &mm, &ss);
        ESP_LOGI(TAG, "loaded snapshot=%lld -> %02d:%02d", (long long)s_offset_sec, hh, mm);
    } else {
        s_offset_sec = kDefaultEpochNoon;
        ESP_LOGW(TAG, "no snapshot, start at 12:00");
    }

    // 周期快照定时器（10 分钟一次）
    esp_timer_create_args_t pcfg = {
        .callback = snapshot_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wc_snap",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&pcfg, &s_snapshot_timer);
    esp_timer_start_periodic(s_snapshot_timer, kSnapshotPeriodSec * 1000000);
}

void wallclock_now(int* h, int* m, int* s) {
    int64_t sec = now_epoch_sec();
    int64_t day = ((sec % 86400) + 86400) % 86400;
    if (h) *h = (int)(day / 3600);
    if (m) *m = (int)(day % 3600 / 60);
    if (s) *s = (int)(day % 60);
}

void wallclock_set(int h, int m) {
    if (h < 0)  h = 0;
    if (h > 23) h = 23;
    if (m < 0)  m = 0;
    if (m > 59) m = 59;
    int64_t cur = now_epoch_sec();
    int64_t day_start = cur - (((cur % 86400) + 86400) % 86400);
    int64_t target = day_start + h * 3600 + m * 60;
    s_offset_sec += target - cur;
    // 2s 防抖后保存快照（避免在按键扫描上下文直接写 flash）
    if (s_save_timer) {
        esp_timer_stop(s_save_timer);   // 未启动时报错，忽略
        esp_timer_start_once(s_save_timer, 2ULL * 1000000);
    }
    ESP_LOGI(TAG, "set time %02d:%02d", h, m);
}

}  // namespace boxpet::bsp
