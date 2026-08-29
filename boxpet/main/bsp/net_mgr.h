// bsp/net_mgr.h — WiFi 管理：SoftAP 配网（AP + Web 配置页）与 STA 按需连接
// 需求4：AP 热点 Pet-XXXX（MAC 后 4 位，密码 12345678），esp_http_server
//       监听 192.168.4.1，配置项写入 NVS（prefs），5 分钟超时自动关闭。
// 需求5：聊天前按需 connect_sta（省电：平时 WiFi 完全关闭，Light Sleep 可用）。
#pragma once

#include "esp_err.h"

namespace boxpet::bsp {

enum class NetMode : uint8_t {
    Off = 0,          // WiFi 完全关闭（默认，省电）
    ApProvisioning,   // 配网模式（AP + HTTP 服务）
    StaConnecting,    // STA 连接中
    StaConnected,     // STA 已联网
    StaFailed,        // STA 连接失败（重试耗尽）
};

// 初始化 netif + event loop（幂等，不启动 WiFi）
esp_err_t net_mgr_init();

NetMode  net_mgr_mode();
bool     net_mgr_has_credentials();   // NVS 是否已有 WiFi 凭据

// 启动配网：SoftAP + Web 配置页；5 分钟无配置自动关闭（返回前同步生效）
esp_err_t net_mgr_start_ap();

// 手动关闭配网（设置页退出时调用）
void net_mgr_stop();

// 按需连接 STA（异步）：用 prefs 保存的凭据；结果轮询 net_mgr_mode()
esp_err_t net_mgr_connect_sta();

}  // namespace boxpet::bsp
