// xz_ws.h — 轻量 WebSocket 客户端（供小智云端对接使用）
// 支持 ws:// 与 wss://（TLS 走 esp-tls），客户端掩码、分片接收拼接。
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xz_ws xz_ws_t;

enum {
    XZ_WS_OP_CONT = 0x0,
    XZ_WS_OP_TEXT = 0x1,
    XZ_WS_OP_BIN  = 0x2,
    XZ_WS_OP_CLOSE= 0x8,
    XZ_WS_OP_PING = 0x9,
    XZ_WS_OP_PONG = 0xA,
};

typedef struct xz_ws_cfg {
    const char* host;         // 主机名（用于 TLS 校验，无端口）
    uint16_t    port;         // 默认 80/443 由调用方给出
    const char* path;         // 如 "/xiaozhi/v1/"
    bool        tls;          // true=wss
    const char* origin;       // 可空
    // 附加握手头（多行 "K: V\r\n"，可空）
    const char* extra_headers;
    int         timeout_ms;   // 连接/IO 超时
} xz_ws_cfg_t;

// 连接（阻塞，含握手）。失败返回 NULL。
xz_ws_t* xz_ws_connect(const xz_ws_cfg_t* cfg);
void     xz_ws_close(xz_ws_t* ws);
bool     xz_ws_ok(const xz_ws_t* ws);

// 协作式关闭请求：仅置标志（内存仍归调用方稍后 xz_ws_close 释放），
// 阻塞中的读循环在 10ms 内以错误返回，由持有任务的线程自行收尾关闭。
// 用途：UI 线程请求停止时绝不直接 free，杜绝 double-free/UAF。
void     xz_ws_shutdown(xz_ws_t* ws);

// 发送（阻塞；自动加客户端 mask）。op: TEXT/BIN/PING。
// text 模式自动加掩码并置 mask bit。返回发送字节数或 <0。
int xz_ws_send(xz_ws_t* ws, int op, const void* data, size_t len, bool mask);
int xz_ws_send_text(xz_ws_t* ws, const char* text);
int xz_ws_send_bin(xz_ws_t* ws, const void* data, size_t len);

// 接收一个完整消息（拼接分片）。op_code 输出消息类型（TEXT/BIN）。
// 缓冲不足时返回 -2。关闭连接返回 -1。阻塞直到消息或超时。
// data 指向内部静态缓冲（下一次调用会被覆盖），len 为消息长度。
int xz_ws_recv(xz_ws_t* ws, int* op_code, const uint8_t** data, size_t* len);

// 运行时 IO 超时（握手完成后调用；连接阶段用 cfg->timeout_ms）。
// 用途：会话建立后缩短到 ~100ms 轮询节拍，及时发送上行队列数据。
void xz_ws_set_idle_timeout(xz_ws_t* ws, int ms);

#ifdef __cplusplus
}
#endif