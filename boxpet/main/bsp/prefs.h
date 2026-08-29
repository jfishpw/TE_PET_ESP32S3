// bsp/prefs.h — 用户偏好与网络配置的 NVS 持久化（需求2 作息 / 需求4 配网）
#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

namespace boxpet::bsp {

// ===== 网络配置（配网页提交，需求4 + 小智云端 路径B）=====
struct NetConfig {
    // 语音对话已改用小智云（路径B）：ASR/LLM/TTS 均在云端，设备仅需
    // WiFi 凭据 + 小智云地址。Token 不再手动填写，而是 OTA 注册流程
    // 自动获取（见 xz_auth），单独持久化（xz_auth_token）。
    char wifi_ssid[33];
    char wifi_pass[65];
    char xz_url[128];     // 小智云 WebSocket 地址（wss://...）
};

esp_err_t prefs_init();

// ===== 作息窗口（宠物睡眠时段，需求2，可配置）=====
// start_hour: 入睡小时（含）；wake_hour: 起床小时（不含）。支持跨午夜（如 23→6）。
void prefs_get_sleep_window(int* start_hour, int* wake_hour);
void prefs_set_sleep_window(int start_hour, int wake_hour);

// ===== 网络配置读写 =====
bool prefs_get_net(NetConfig* out);        // 返回明文
void prefs_set_net(const NetConfig* in);
bool prefs_has_net();                      // 是否已保存过 WiFi 凭据

// ===== 小智 OTA 注册（token/激活码自动获取，xz_client 使用）=====
bool prefs_get_xz_token(char* out, size_t len);          // 是否已自动获取到 token
void prefs_set_xz_token(const char* in);                 // 保存 token
bool prefs_get_xz_code(char* out, size_t len);           // 读取待绑定激活码（供屏显）
void prefs_set_xz_code(const char* in);                  // 保存待绑定激活码（未绑定时）
// 设备 Client-Id（UUID v4）：OTA 注册与 WS 握手必须使用同一值（服务器按
// Device-Id+Client-Id 匹配绑定），与激活码分开持久化，不得互相覆盖。
bool prefs_get_xz_client(char* out, size_t len);
void prefs_set_xz_client(const char* in);

}  // namespace boxpet::bsp
