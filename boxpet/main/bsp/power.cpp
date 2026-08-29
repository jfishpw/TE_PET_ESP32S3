// bsp/power.cpp — 电源管理（参考 xiaozhi-esp32 power_manager 实现）
//   * SYS_POW=GPIO2：开机后置高保持系统不掉电
//   * CODEC_PWR=GPIO14：拉高使能音频 codec 电源
//   * CHG_CTRL=GPIO47：拉低 → 100ms 测量电池电压 → 拉高
//   * CHRG=GPIO48：低电平 = Type-C 已接入
//   * BAT_VSEN=GPIO1：ADC1_CH0，ADC_ATTEN_DB_12
#include "power.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

static const char* TAG = "power";

namespace boxpet::bsp {

namespace {

static adc_oneshot_unit_handle_t s_adc = nullptr;
static esp_timer_handle_t        s_timer = nullptr;
static PowerStatus               s_status = {};
static PowerEventCb              s_evt_cb = nullptr;
static void*                     s_evt_ctx = nullptr;
static bool                      s_low_warned = false;

// 电量查表：直接采用 xiaozhi-esp32 / esp-claw 的 ATK-DNESP32S3-BOX0 实测标定
// （boards/atk-dnesp32s3-box0，与硬件厂商固件一致的 raw↔% 关系）。
// 该板 ADC1_CH0(GPIO1) + CHG_CTRL 切换通路的实测值：
//   raw 2951→0%（接近硬件低电量阈值 2877），3019→20%，3037→40%，
//   3091→60%，3124→80%，3231→100%（4.20V 满电）。
// 此前使用的"完整锂电 3.40-4.20V 估算曲线"与整机实际截止电压不符，
// 中低电量显示严重偏高（看起来还有很多电，实际已接近关机），故换用实测表。
struct AdcLut { uint16_t adc; uint8_t pct; };
static const AdcLut kBatteryAdcTable[] = {
    {2951,  0},
    {3019, 20},
    {3037, 40},
    {3091, 60},
    {3124, 80},
    {3231,100},
};
static constexpr int kBatteryLutCount = sizeof(kBatteryAdcTable) / sizeof(kBatteryAdcTable[0]);

static uint8_t lookup_battery_pct(uint16_t adc) {
    if (adc <= kBatteryAdcTable[0].adc) return 0;
    if (adc >= kBatteryAdcTable[kBatteryLutCount - 1].adc) return 100;
    for (int i = 0; i < kBatteryLutCount - 1; i++) {
        if (adc >= kBatteryAdcTable[i].adc && adc < kBatteryAdcTable[i+1].adc) {
            float r = float(adc - kBatteryAdcTable[i].adc) /
                      float(kBatteryAdcTable[i+1].adc - kBatteryAdcTable[i].adc);
            return uint8_t(kBatteryAdcTable[i].pct + r * (kBatteryAdcTable[i+1].pct - kBatteryAdcTable[i].pct));
        }
    }
    return 0;
}

static uint16_t read_battery_adc_average(int samples = 10) {
    // CHG_CTRL 拉低 100ms 切换到电池通路，再连续采样（对齐 BOX0 实测流程）
    gpio_set_level(CHG_CTRL_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    gpio_set_level(CHG_CTRL_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    return uint16_t(sum / samples);
}

// ===== 需求1：电量滤波 + 迟滞 =====
// 管线：10 次硬件平均（read_battery_adc_average 内）
//      → 中值滤波(5)：剔除 WiFi/功放开启瞬间的纹波毛刺
//      → 滑动均值(8)：平滑曲线
//      → 迟滞：非充电单调递减（电压负载恢复造成的"回跳"不显示），
//              单次变化 ≤2%（30s 采样周期内真实电量变化远小于 2%）
static uint8_t s_med_buf[5] = {0};   // 中值窗口（原始查表 pct）
static int      s_med_idx = 0;
static int      s_med_cnt = 0;
static uint8_t s_avg_buf[8] = {0};   // 滑动均值窗口（中值输出）
static int      s_avg_idx = 0;
static int      s_avg_cnt = 0;

static uint8_t median5(uint8_t* a, int n) {
    // 插入排序后取中位（n≤5，数据量极小）
    for (int i = 1; i < n; ++i) {
        uint8_t k = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; --j; }
        a[j + 1] = k;
    }
    return a[n / 2];
}

static uint8_t filtered_pct(uint8_t raw) {
    s_med_buf[s_med_idx] = raw;
    s_med_idx = (s_med_idx + 1) % 5;
    if (s_med_cnt < 5) s_med_cnt++;
    uint8_t med = median5(s_med_buf, s_med_cnt);

    s_avg_buf[s_avg_idx] = med;
    s_avg_idx = (s_avg_idx + 1) % 8;
    if (s_avg_cnt < 8) s_avg_cnt++;
    uint32_t sum = 0;
    for (int i = 0; i < s_avg_cnt; ++i) sum += s_avg_buf[i];
    return (uint8_t)(sum / s_avg_cnt);
}

static void poll_fn(void* /*arg*/) {
    // 1. 充电状态读取（低电平表示 Type-C 接入）
    bool charging_now = (gpio_get_level(CHRG_PIN) == 0);
    // 2. ADC 采样 → 查表 → 滤波（需求1）
    uint16_t adc = read_battery_adc_average(10);
    uint8_t raw_pct = lookup_battery_pct(adc);
    uint8_t filt = filtered_pct(raw_pct);

    // 3. 迟滞（需求1）：
    //    首次采样直接采用；充电允许上升；放电只允许下降且单次 ≤2%
    int disp = s_status.battery_pct;
    static bool s_first = true;
    if (s_first) {
        disp = filt;
        s_first = false;
    } else if (charging_now) {
        // 充电：允许上升/下降（充满拔出瞬间回落），单次限幅 ≤5% 防跳变
        if (filt > disp + 5) disp += 5;
        else if (filt < disp - 5) disp = (filt > disp - 5) ? filt : disp - 5;
        else disp = filt;
    } else {
        // 放电：单调不增；单次下降 ≤2%；回升（负载减轻电压恢复）不显示
        int target = (filt < disp) ? filt : disp;
        if (disp - target > 2) target = disp - 2;
        disp = target;
    }
    if (disp < 0) disp = 0;
    if (disp > 100) disp = 100;
    uint8_t pct = (uint8_t)disp;

    // 4. 更新状态
    bool changed = false;
    if (s_status.battery_pct != pct) { s_status.battery_pct = pct; changed = true; }
    if (s_status.charging != charging_now) { s_status.charging = charging_now; changed = true; }
    s_status.supply = charging_now ? PowerSupply::TypeC : PowerSupply::Battery;
    // 低电量阈值 15%（需求1：≤15% 图标变红闪烁）
    bool low = (pct <= 15) && !charging_now;
    if (low != s_status.low_voltage) { s_status.low_voltage = low; changed = true; }
    ESP_LOGD(TAG, "ADC=%u raw=%u filt=%u disp=%u charging=%d",
             adc, raw_pct, filt, pct, charging_now);
    if (changed && s_evt_cb) s_evt_cb(s_status, s_evt_ctx);
}

}  // namespace

esp_err_t power_init() {
    // SYS_POW / CODEC_PWR 输出高电平保持
    gpio_config_t out_cfg = {};
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pin_bit_mask = (1ULL << SYS_POW_PIN) | (1ULL << AUDIO_CODEC_PWR_PIN) | (1ULL << CHG_CTRL_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio out");
    ESP_LOGI(TAG, "bc1: gpio out cfg done, setting latches...");
    gpio_set_level(SYS_POW_PIN, 1);
    // 电源锁存脚必须 hold：Light Sleep 时 IDF 会隔离所有 GPIO（置高阻），
    // SYS_POW 悬空后锁存电路缓慢放电（几十秒~几分钟，随机）→ 系统断电 →
    // POWERON 复位循环（reset reason=1，无 coredump；表现为"息屏约 30 秒后
    // 重启、时钟显示 00:00 后回到入睡时刻且不走、单击唤不醒"）。
    // gpio_hold_en 是 pad 级保持，Light Sleep 中持续输出高电平，软复位后仍有效。
    gpio_hold_en(SYS_POW_PIN);
    ESP_LOGI(TAG, "bc2: SYS_POW high (held)");
    gpio_set_level(AUDIO_CODEC_PWR_PIN, 1);
    // 同 SYS_POW：睡眠中保持 codec 供电。ES8311 断电丢配置且仅在开机初始化，
    // 若睡眠中被隔离掉电，唤醒后无声（且 I2S 时钟丢失会导致 codec lock loss）。
    // CHG_CTRL 不 hold：电池采样每 30s 需要动态拉低/拉高切换测量通路。
    gpio_hold_en(AUDIO_CODEC_PWR_PIN);
    ESP_LOGI(TAG, "bc3: CODEC_PWR high (held)");
    gpio_set_level(CHG_CTRL_PIN, 1);
    ESP_LOGI(TAG, "bc4: CHG_CTRL high");
    // CHRG 输入上拉
    gpio_config_t in_cfg = {};
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    in_cfg.pin_bit_mask = (1ULL << CHRG_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "gpio in CHRG");
    ESP_LOGI(TAG, "bc5: CHRG input cfg done");

    // ADC 初始化（ADC1_CH0 = GPIO1）
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc init");
    ESP_LOGI(TAG, "bc6: adc unit ok");
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ch_cfg), TAG, "adc ch cfg");
    // 初始读一次
    s_status.battery_pct = lookup_battery_pct(read_battery_adc_average(4));
    s_status.charging = (gpio_get_level(CHRG_PIN) == 0);
    s_status.supply = s_status.charging ? PowerSupply::TypeC : PowerSupply::Battery;
    s_status.low_voltage = (s_status.battery_pct <= 20) && !s_status.charging;
    ESP_LOGI(TAG, "power init: pct=%u charging=%d", s_status.battery_pct, s_status.charging);
    return ESP_OK;
}

esp_err_t power_start_monitor() {
    if (s_timer) return ESP_OK;
    esp_timer_create_args_t t = {
        .callback = poll_fn,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_poll",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&t, &s_timer), TAG, "create timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, 30ULL * 1000 * 1000), TAG, "start timer");  // 30s（5s 太频繁，CHG_CTRL 每 5s 切换 100ms+6 次 ADC ≈ 130ms 断续耗电）
    ESP_LOGI(TAG, "power monitor started (30s period)");
    return ESP_OK;
}

PowerStatus power_get_status() {
    return s_status;
}

void power_register_event_cb(PowerEventCb cb, void* ctx) {
    s_evt_cb = cb;
    s_evt_ctx = ctx;
}

void power_deinit() {
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = nullptr;
    }
}

}  // namespace boxpet::bsp