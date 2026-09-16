// bsp/weather.h — 天气系统（背景天气形态 + 温度显示）
//   数据源：open-meteo（免 key、HTTP 直连、JSON），默认长沙坐标
//   节奏：每 2 小时查询一次；失败/无网络 → 随机一种天气（同样 2 小时换一次）
//   时机：深睡/熄屏不查询；亮屏后 ≥30s 才允许首次查询；查询在独立任务里做
//        （绝不占用 UI/宠物 tick 上下文，规避看门狗）
#pragma once

#include <cstdint>

namespace boxpet::bsp {

// 天气形态（8 种，叠加在既有昼夜背景之上）
enum class Weather : uint8_t {
    Sunny    = 0,   // 晴
    Cloudy   = 1,   // 多云
    Overcast = 2,   // 阴
    Rain     = 3,   // 雨
    Snow     = 4,   // 雪
    Thunder  = 5,   // 雷阵雨
    Fog      = 6,   // 雾
    Windy    = 7,   // 大风
    Count    = 8,
};

// 上电初始化：从 NVS 载入天气/温度/上次查询时刻（无记录则随机一种）
void weather_init();

Weather     weather_current();
int         weather_temp();          // 摄氏整数
bool        weather_temp_valid();    // false = 无真实数据（离线随机），不显示温度
const char* weather_name(Weather w); // 中文名

// 由 UI tick 调用（仅在屏幕亮时）：满足"亮屏≥30s 且距上次查询≥2h"时启动联网查询。
// screen_on_ms = 屏幕本次点亮至今毫秒数（避免刚点亮时立刻联网）。
void weather_maybe_refresh(int64_t screen_on_ms);

// 版本号：天气/温度变化时自增；UI 据此重绘（避免每 tick 重画）
uint32_t weather_generation();

}  // namespace boxpet::bsp
