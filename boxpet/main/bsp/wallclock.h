// bsp/wallclock.h — 真实时间墙钟（无 RTC 芯片，用 esp_timer + NVS offset 模拟）
// 原理：offset = 用户设定时刻的 epoch - 当时 esp_timer 秒数。
//       之后 now = esp_timer 当前秒 + offset。offset 持久化到 NVS，重启继续走。
#pragma once
#include <cstdint>

namespace boxpet::bsp {

// 从 NVS 读取 offset（无记录则从 12:00 开始）
void wallclock_init();

// 当前时间（时/分/秒）
void wallclock_now(int* h, int* m, int* s);

// 设置时间（时、分）；写入 NVS 并立即生效
void wallclock_set(int h, int m);

// 时间快进 sec 秒（深休眠恢复：睡眠期间 RTC 照走，醒来把墙钟拨到当前时刻）。
// 仅改内存 offset，不写 NVS（后续 10 分钟周期快照会兜底持久化）。
void wallclock_advance_by(int64_t sec);

// 立即把当前时刻快照写入 NVS（深休眠/关机前强制保存，防重启回拨 >10 分钟）。
void wallclock_force_snapshot();

}  // namespace boxpet::bsp
