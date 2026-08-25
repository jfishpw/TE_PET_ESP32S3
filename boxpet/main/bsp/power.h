// bsp/power.h — 电源管理（SYS_POW 保持、电池 ADC、充电检测）
#pragma once

#include <stdint.h>
#include "esp_err.h"

namespace boxpet::bsp {

enum class PowerSupply : uint8_t {
    Battery = 0,
    TypeC   = 1,
};

struct PowerStatus {
    uint8_t      battery_pct;  // 0~100（查表估算）
    bool         charging;
    PowerSupply supply;
    bool         low_voltage;  // battery_pct <= 20
};

// 初始化电源引脚（拉高 SYS_POW / CODEC_PWR / CHG_CTRL，配置 CHRG 输入）
esp_err_t power_init();

// 启动电池 ADC 周期性采集任务（默认每 5s 一次）
esp_err_t power_start_monitor();

// 获取当前电源状态快照
PowerStatus power_get_status();

// 注册充电状态变化回调
using PowerEventCb = void (*)(PowerStatus status, void* ctx);
void power_register_event_cb(PowerEventCb cb, void* ctx);

// 进入深度低功耗前调用：关闭 ADC、停止定时器
void power_deinit();

}  // namespace boxpet::bsp