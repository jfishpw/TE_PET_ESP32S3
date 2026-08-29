// bsp/xz_client.h — 小智云端对话客户端（路径B，飞机自研轻量实现）
// 协议：WebSocket(WSS) + JSON 控制帧 + 二进制音频（PCM/Opus）。
// 依赖：components/xz_ws（RFC6455 客户端）、audio 流式采集/播放。
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdbool>
#include "esp_err.h"

namespace boxpet::bsp {

// 服务器下发文本（对话/状态）回调
typedef void (*XzTextFn)(const char* text);
// 服务器音频流到达（PCM 16k mono）回调（供播放）
typedef void (*XzPcmFn)(const int16_t* pcm, size_t n);
// 会话状态变化：0=断开 1=已连接(hello) 2=开始说话 3=结束说话
typedef void (*XzStateFn)(int state);

// OTA 注册结果回调：0=失败 1=已绑定(token就绪) 2=待绑定(激活码已存，去控制台绑定)
typedef void (*XzAuthFn)(int status);

void xz_set_callbacks(XzTextFn text, XzPcmFn pcm, XzStateFn state);

// 启动：连接 + hello（异步）。url 形如 wss://host[:port]/path
esp_err_t xz_start(const char* url, const char* token);
void      xz_stop();                 // 断开并复位
bool      xz_active();               // 已连上且 hello 完成

// OTA 注册（自动获取 token/激活码）。ws_url 用于派生同源的 OTA 接口。
// 结果经 cb 异步回调（内部起一次性任务，不阻塞调用者）。绑定成功后
// 自动持久化 token（prefs_set_xz_token）并更新 net.xz_url。
void xz_auth_begin(const char* ws_url, XzAuthFn cb);

// 说话控制：采集端开启/停止时通知协议（发 start/stop 文本帧）
void xz_talk_begin();
void xz_talk_end();

// 音频上传：采集帧（16k mono int16）交协议发送
void xz_send_audio(const int16_t* pcm, size_t n);

}  // namespace boxpet::bsp