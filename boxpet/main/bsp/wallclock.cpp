// bsp/wallclock.cpp — 真实时间墙钟实现
// 持久化策略：NVS 存"保存时刻的时钟读数"快照（键 wc_ep）。
// 重启后 显示时间 = 开机时长 + 快照值，误差仅为"上次保存→重启"的间隔。
// 保存时机：调时间后 2s 防抖 + 每 10 分钟周期快照 → 崩溃后时间最多回拨 10 分钟。
// （旧方案存"偏移量"，重启后 esp_timer 归零，时间会倒退回上次调整的时刻）
#include "wallclock.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"   // portMUX（offset 并发保护）
#include "esp_timer.h"
#include "esp_log.h"

namespace boxpet::bsp {

static const char* TAG = "wallclock";
static int64_t s_offset_sec = 0;  // epoch 秒 - esp_timer 秒
// 并发保护：offset 被 4 个上下文读写（宠物 tick 补跳、按键调时间、NTP 对时、
// 定时器快照）。int64 在 32 位机上的非原子读写可能撕裂 → 时间跳飞，故加自旋锁。
static portMUX_TYPE s_wc_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_save_timer = nullptr;      // 调整后 2s 防抖保存
static esp_timer_handle_t s_snapshot_timer = nullptr;  // 周期快照（10 min）

// 默认基准：今天 12:00（epoch 固定值，运行后由用户校准）
static constexpr int64_t kDefaultEpochNoon = 12 * 3600;
static constexpr int64_t kSnapshotPeriodSec = 600;  // 10 分钟

static int64_t now_epoch_sec() {
    portENTER_CRITICAL(&s_wc_mux);
    int64_t v = esp_timer_get_time() / 1000000 + s_offset_sec;
    portEXIT_CRITICAL(&s_wc_mux);
    return v;
}

// 保存当前时钟读数快照（重启后从该时刻继续走）
static void save_snapshot() {
    int64_t snap = now_epoch_sec();   // 先取值（内部加锁），NVS 写不在锁内
    nvs_handle_t h;
    if (nvs_open("boxpet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "wc_ep", snap);
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

int64_t wallclock_epoch() { return now_epoch_sec(); }

void wallclock_set(int h, int m) {
    if (h < 0)  h = 0;
    if (h > 23) h = 23;
    if (m < 0)  m = 0;
    if (m > 59) m = 59;
    int64_t cur = now_epoch_sec();
    int64_t day_start = cur - (((cur % 86400) + 86400) % 86400);
    int64_t target = day_start + h * 3600 + m * 60;
    portENTER_CRITICAL(&s_wc_mux);
    s_offset_sec += target - cur;
    portEXIT_CRITICAL(&s_wc_mux);
    // 2s 防抖后保存快照（避免在按键扫描上下文直接写 flash）
    if (s_save_timer) {
        esp_timer_stop(s_save_timer);   // 未启动时报错，忽略
        esp_timer_start_once(s_save_timer, 2ULL * 1000000);
    }
    ESP_LOGI(TAG, "set time %02d:%02d", h, m);
}

// 深休眠恢复：RTC 计数器在深休眠期间照走，醒来后 offset 仍是入睡时刻。
// 逐秒/批量快进 offset，把墙钟拨到真实当前时刻（配合宠物逐秒补跳使用，
// 保证睡眠窗判定在补跳过程中按"虚拟当前时刻"推进）。
void wallclock_advance_by(int64_t sec) {
    if (sec <= 0) return;
    portENTER_CRITICAL(&s_wc_mux);
    s_offset_sec += sec;
    portEXIT_CRITICAL(&s_wc_mux);
}

// 深休眠/关机前强制快照：把当前 epoch 写 NVS，重启后从该时刻继续走，
// 避免深休眠这段"墙钟已走但快照旧"导致回落 >10 分钟。
void wallclock_force_snapshot() {
    save_snapshot();
}

// 网络对时（NTP）：把墙钟校准到给定 epoch 秒并立即持久化。
// 传入"显示用的本地时刻"（本机按东八区显示，调用方用 UTC+8）。返回校准量（秒）。
int64_t wallclock_sync_to_epoch(int64_t epoch) {
    int64_t delta;
    portENTER_CRITICAL(&s_wc_mux);
    int64_t cur = esp_timer_get_time() / 1000000 + s_offset_sec;
    delta = epoch - cur;
    s_offset_sec += delta;
    portEXIT_CRITICAL(&s_wc_mux);
    save_snapshot();
    return delta;
}

}  // namespace boxpet::bsp
