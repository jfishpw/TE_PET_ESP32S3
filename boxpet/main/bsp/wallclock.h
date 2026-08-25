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

}  // namespace boxpet::bsp
