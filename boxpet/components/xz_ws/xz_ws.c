// xz_ws.c — 轻量 WebSocket 客户端（RFC6455）实现
#include "xz_ws.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char* TAG = "xz_ws";

#define RX_BUF 16384   // 接收消息缓冲（拼接分片后）

struct xz_ws {
    esp_tls_t* tls;
    bool       ok;
    char       rx[RX_BUF];
    size_t     rx_fill;      // 已缓冲字节数
    size_t     rx_pos;       // 读取游标
    int        frag_op;      // 拼接中的分片类型，-1 无
    size_t     msg_len;      // 当前拼接消息长度
    bool       saw_mask;     // 是否已发现服务器发掩码帧（一次性日志用）
    volatile bool closing;   // 协作式关闭请求：读循环 10ms 内以错误退出
    int        timeout_ms;
};

static int read_n(xz_ws_t* ws, void* buf, size_t n, int timeout_ms) {
    size_t got = 0;
    uint8_t* p = (uint8_t*)buf;
    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    while (got < n) {
        if (ws->closing) return -1;                // 协作式关闭请求
        int r = esp_tls_conn_read(ws->tls, p + got, (int)(n - got));
        if (r > 0) { got += r; continue; }
        if (r == 0) return -1;                     // 对端 close_notify
        // esp_tls 阻塞读超时返回 WANT_READ/WANT_WRITE（不设 errno！）。
        // 曾按 errno 判断 → 超时被误判为错误 → recv 返回 -1 → 主循环静默
        // 断开（表现为"卡住不动"，实为任务已退出）。
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE
            || errno == EAGAIN || errno == EWOULDBLOCK) {
            if ((int64_t)(esp_timer_get_time() / 1000) >= deadline) return -2;  // 超时
            vTaskDelay(pdMS_TO_TICKS(10));         // 防忙等
            continue;
        }
        ESP_LOGW(TAG, "conn_read err=%d errno=%d got=%u", r, errno, (unsigned)got);
        return -1;
    }
    return (int)got;
}

static int write_n(xz_ws_t* ws, const void* buf, size_t n, bool do_flush) {
    (void)do_flush;
    size_t sent = 0;
    const uint8_t* p = (const uint8_t*)buf;
    int64_t deadline = esp_timer_get_time() / 1000 + ws->timeout_ms;
    while (sent < n) {
        int r = esp_tls_conn_write(ws->tls, p + sent, (int)(n - sent));
        if (r > 0) { sent += r; continue; }
        // 非阻塞 socket：发送缓冲满返回 WANT_WRITE（瞬时状态），必须重试，
        // 否则 hello/音频帧发送被误判失败。
        if (r == ESP_TLS_ERR_SSL_WANT_WRITE || r == ESP_TLS_ERR_SSL_WANT_READ
            || errno == EAGAIN || errno == EWOULDBLOCK) {
            if ((int64_t)(esp_timer_get_time() / 1000) >= deadline) {
                ESP_LOGW(TAG, "write timeout %u/%u", (unsigned)sent, (unsigned)n);
                return -1;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        ESP_LOGW(TAG, "conn_write err=%d errno=%d", r, errno);
        return -1;
    }
    return (int)sent;
}

// 解析响应里若干行头（crlf 结尾）。返回第 accept 状态码是否 101。
static bool handshake_read(xz_ws_t* ws, char** out_headers) {
    char line[512];
    bool got_upgrade = false, got_accept = false;
    // 读状态行
    int li = 0; char c;
    while (read_n(ws, &c, 1, 5000) == 1 && c != '\n') {
        if (li < 511) line[li++] = (char)c;
    }
    line[li] = 0;
    if (strstr(line, "101") == NULL) { ESP_LOGE(TAG, "ws handshake: %s", line); return false; }
    char* hdrs = (char*)calloc(1, 1024);
    size_t hof = 0;
    for (;;) {
        li = 0; bool eoh = false;
        while (read_n(ws, &c, 1, 5000) == 1 && c != '\n') {
            if (li < 511) line[li++] = (char)c;
        }
        line[li] = 0;
        if (li <= 1) { eoh = true; }   // 空行
        else {
            if (hof + li + 2 < 1024) {
                hdrs[hof] = 0; strcat(hdrs, line); strcat(hdrs, "\n");
                hof += li + 1;
            }
            if (strncasecmp(line, "Upgrade: websocket", 18) == 0) got_upgrade = true;
            if (strncasecmp(line, "Sec-WebSocket-Accept", 20) == 0) got_accept = true;
        }
        if (eoh) break;
    }
    if (out_headers) *out_headers = hdrs; else free(hdrs);
    return got_upgrade || got_accept;
}

static char* b64_key(uint8_t* key16) {
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, key16, 16);
    char* out = (char*)malloc(olen + 1);
    mbedtls_base64_encode((unsigned char*)out, olen, &olen, key16, 16);
    out[olen] = 0;
    return out;
}

// 生成 16 字节随机 key 与 Client-Id（无加密需求，用伪随机 + 时间）
static void rand_bytes(uint8_t* p, size_t n) {
    uint64_t seed = esp_timer_get_time();
    for (size_t i = 0; i < n; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        p[i] = (uint8_t)(seed >> 56);
    }
}

xz_ws_t* xz_ws_connect(const xz_ws_cfg_t* cfg) {
    if (!cfg || !cfg->host) return NULL;
    esp_tls_cfg_t tc = {};
    tc.timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 10000;
    // 握手阶段保持 esp_tls 默认阻塞模式：non_block=true 会让
    // esp_tls_conn_new_sync 的握手循环变成无延时的忙等（WANT_READ 立即
    // 重试），CPU100% 饿死 IDLE → task watchdog 5s 超时（实测复现）。
    // 而 lwIP 未开 CONFIG_LWIP_SO_RCVTIMEO，SO_RCVTIMEO 静默无效，数据
    // 阶段必须非阻塞 + read_n 自管超时（本工程 lwip 非阻塞 recv 不违反
    // MBEDTLS_THREADING：socket 单任务读单任务写已由上行队列串行化）。
    // 折中：握手完成后用 fcntl 把底层 socket 手动切成非阻塞。
    if (cfg->tls) {
        tc.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_tls_t* tls = esp_tls_init();   // IDF5.4：先创建句柄，conn_new_sync 写入
    if (!tls) { ESP_LOGE(TAG, "tls init failed"); return NULL; }
    int rc = esp_tls_conn_new_sync(cfg->host, (int)strlen(cfg->host),
                                   (int)cfg->port, &tc, tls);
    // esp_tls_conn_new_sync 返回值：1=成功、-1=失败、0=超时
    if (rc != 1) {
        // 输出主机 + 详细 TLS 错误（取出错误句柄，转换为 errno + 栈错误码）
        int esp_os_err = 0;
        int tls_flags = 0;
        esp_tls_error_handle_t eh = NULL;
        esp_err_t e2 = esp_tls_get_error_handle(tls, &eh);
        esp_err_t e3 = (eh) ? esp_tls_get_and_clear_last_error(eh, &esp_os_err, &tls_flags) : 0;
        ESP_LOGE(TAG, "tls connect failed host=%s:%d rc=%d e2=0x%x e3=0x%x esp_err=%d tls_flags=0x%x",
                 cfg->host, (int)cfg->port, rc, e2, e3, esp_os_err, tls_flags);
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    xz_ws_t* ws = (xz_ws_t*)calloc(1, sizeof(xz_ws_t));
    ws->tls = tls;
    ws->ok = true;
    ws->frag_op = -1;
    ws->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 10000;

    uint8_t key16[16];
    rand_bytes(key16, sizeof(key16));
    char* keyb64 = b64_key(key16);
    char req[1152];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s\r\n",                                     // 附加头（Authorization 等）
        cfg->path ? cfg->path : "/", cfg->host, (int)cfg->port, keyb64,
        cfg->extra_headers ? cfg->extra_headers : "");
    free(keyb64);
    if (write_n(ws, req, strlen(req), true) < 0) {
        ESP_LOGE(TAG, "ws handshake send failed");
        xz_ws_close(ws);
        return NULL;
    }
    char* hdrs = NULL;
    if (!handshake_read(ws, &hdrs)) {
        ESP_LOGE(TAG, "ws handshake rejected. headers:\n%s", hdrs ? hdrs : "");
        free(hdrs);
        xz_ws_close(ws);
        return NULL;
    }
    free(hdrs);
    // 握手完成，此刻安全地把底层 socket 切成非阻塞：此后 recv 无数据立即
    // 返回 EWOULDBLOCK（经 mbedtls 转 WANT_READ），超时由 read_n 的
    // esp_timer deadline 自管——绕开 lwIP SO_RCVTIMEO 未编译生效的死等。
    {
        int fd = -1;
        if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0) {
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
    }
    ESP_LOGI(TAG, "ws connected %s://%s:%d%s", cfg->tls ? "wss" : "ws",
             cfg->host, (int)cfg->port, cfg->path);
    return ws;
}

void xz_ws_close(xz_ws_t* ws) {
    if (!ws) return;
    if (ws->tls) esp_tls_conn_destroy(ws->tls);
    free(ws);
}

bool xz_ws_ok(const xz_ws_t* ws) { return ws && ws->ok; }

void xz_ws_shutdown(xz_ws_t* ws) { if (ws) ws->closing = true; }

void xz_ws_set_idle_timeout(xz_ws_t* ws, int ms) { if (ws) ws->timeout_ms = ms; }

int xz_ws_send(xz_ws_t* ws, int op, const void* data, size_t len, bool mask) {
    if (!ws || !ws->ok) return -1;
    uint8_t hdr[16];
    int hn = 0;
    hdr[hn++] = 0x80 | (op & 0x0F);                    // FIN=1 + opcode
    bool do_mask = false;
    (void)do_mask;
    if (mask) {
        hdr[hn++] = 0x80;                              // MASK=1
        if (len < 126) hdr[hn - 1] |= (uint8_t)len;
        else if (len < 65536) { hdr[hn - 1] |= 126; hdr[hn++] = (uint8_t)(len >> 8); hdr[hn++] = (uint8_t)len; }
        else { hdr[hn - 1] |= 127; for (int i = 7; i >= 0; --i) hdr[hn++] = (uint8_t)((uint64_t)len >> (8 * i)); }
        uint8_t mk[4];
        rand_bytes(mk, 4);
        memcpy(hdr + hn, mk, 4);
        hn += 4;
        if (write_n(ws, hdr, hn, false) < 0) return -1;
        // 掩码 payload（流式分块避免大拷贝）
        const uint8_t* src = (const uint8_t*)data;
        uint8_t chunk[512];
        size_t off = 0;
        int rc = 0;
        while (off < len) {
            size_t n = (len - off) < sizeof(chunk) ? (len - off) : sizeof(chunk);
            for (size_t i = 0; i < n; ++i) chunk[i] = src[off + i] ^ mk[(off + i) & 3];
            if (write_n(ws, chunk, n, false) < 0) { rc = -1; break; }
            off += n;
        }
        return rc < 0 ? -1 : (int)len;
    } else {
        // 服务端不需要（一般不会用）：仅支持无掩码的大分块场景（发送服务端帧不作支持）
        return -1;
    }
}

int xz_ws_send_text(xz_ws_t* ws, const char* text) {
    return xz_ws_send(ws, XZ_WS_OP_TEXT, text, strlen(text), true);
}
int xz_ws_send_bin(xz_ws_t* ws, const void* data, size_t len) {
    return xz_ws_send(ws, XZ_WS_OP_BIN, data, len, true);
}

// 从流中读 1 字节。返回 0..255，-1=错误 -2=超时。
// 修复双重 bug：① 旧实现把刚读的字节存入缓冲、下次调用又原样返回一遍
//（read_n 从不预读，缓冲只会被 rd 自己重复消费 → 帧头字节成对重复
// "A A B B…" → 帧错位、JSON 被啃成乱码）；② rx[] 为有符号 char，
// ≥0x80 的字节（如 FIN 帧 b0=0x81/0x82）符号扩展成负数 → 被当错误
// 返回。必须强转 uint8_t 且不再回写缓冲。
// 从流中读 1 字节（可指定超时）。返回 0..255，-1=错误 -2=超时。
static int rd_t(xz_ws_t* ws, int tmo) {
    if (ws->rx_pos < ws->rx_fill) return (uint8_t)ws->rx[ws->rx_pos++];
    unsigned char c;
    int r = read_n(ws, &c, 1, tmo);
    if (r != 1) return r;   // -1 / -2 原样透传
    return c;
}
static int rd(xz_ws_t* ws) { return rd_t(ws, ws->timeout_ms); }

// 读取一帧的 n 字节负载到 dst：先消费内部缓冲（TLS 一次性多读进来的字节），
// 缓冲耗尽再直读底层。返回成功读到的字节数，失败返回 -1，超时返回 -2。
// 注意：残留缓冲（本负载之后的字节）被保留，供下一帧 rd() 使用。
static int sread_payload(xz_ws_t* ws, uint8_t* dst, size_t n, int timeout_ms) {
    size_t got = 0;
    while (got < n && ws->rx_pos < ws->rx_fill) dst[got++] = ws->rx[ws->rx_pos++];
    if (got >= n) return (int)n;
    int r = read_n(ws, dst + got, n - got, timeout_ms);
    if (r != (int)(n - got)) return r;   // read_n 失败返回 -1/-2（成功返回确切字节数）
    return (int)n;
}

int xz_ws_recv(xz_ws_t* ws, int* op_code, const uint8_t** data, size_t* len) {
    if (!ws || !ws->ok) return -1;
    (void)data; (void)len;
    // 会话期 timeout_ms 被上层缩短（~100ms）做上行队列轮询节拍：b0 用它
    // 短等，超时即返回 -2 让调用方有机会发送排队数据；一旦 b0 到达，帧
    // 已开始传输，服务器必然继续发完，b1/长度/mask/payload 用长超时。
    const int frame_tmo = 8000;
    for (;;) {
        int b0 = rd_t(ws, ws->timeout_ms);
        if (b0 < 0) return b0 == -2 ? -2 : -1;
        int b1 = rd_t(ws, frame_tmo);
        if (b1 < 0) return -1;
        int fin = (b0 >> 7) & 1;
        int op  = b0 & 0x0F;
        int masked = (b1 >> 7) & 1;
        uint64_t plen = b1 & 0x7F;
        if (plen == 126) {
            int h1 = rd_t(ws, frame_tmo), h2 = rd_t(ws, frame_tmo);
            if (h1 < 0 || h2 < 0) return -1;
            plen = ((uint64_t)h1 << 8) | (uint64_t)h2;
        } else if (plen == 127) {
            plen = 0;
            for (int i = 0; i < 8; ++i) {
                int h = rd_t(ws, frame_tmo);
                if (h < 0) return -1;
                plen = (plen << 8) | (uint64_t)h;
            }
        }
        // RFC6455 规定服务端帧不带掩码，但 tenclass 网关实测会偶发下发带
        // 掩码帧。忽略 MASK 位会让 4 字节 key 混进 payload、且流的真实
        // payload 被截走 4 字节 → 每帧错位 4 字节级联损坏（此前 hello
        // 回执被啃成乱码、opus err=-4、假 len=2 帧的根源）。合规做法：
        // 读掉 key，payload 逐字节 ^key[i%4] 还原。
        uint8_t mk[4] = {0};
        if (masked) {
            int m0 = rd_t(ws, frame_tmo), m1 = rd_t(ws, frame_tmo);
            int m2 = rd_t(ws, frame_tmo), m3 = rd_t(ws, frame_tmo);
            if (m0 < 0 || m1 < 0 || m2 < 0 || m3 < 0) return -1;
            mk[0] = (uint8_t)m0; mk[1] = (uint8_t)m1;
            mk[2] = (uint8_t)m2; mk[3] = (uint8_t)m3;
            if (!ws->saw_mask) {
                ws->saw_mask = true;
                ESP_LOGW(TAG, "masked frame from server (non-standard), unmasking");
            }
        }
        // 帧过长作截断保护（tmp 区只有半缓冲，单帧上限取 RX_BUF/2）
        if (plen > RX_BUF / 2) { ws->rx_fill = ws->rx_pos = 0; return -3; }
        size_t p = (size_t)plen;

        // 控制帧：负载临时放到栈/固定区处理
        if (op == XZ_WS_OP_PING) {
            uint8_t tmp[126];
            if (sread_payload(ws, tmp, p, frame_tmo) < 0) return -1;
            if (masked) for (size_t i = 0; i < p; ++i) tmp[i] ^= mk[i & 3];
            xz_ws_send(ws, XZ_WS_OP_PONG, tmp, p, true);
            continue;
        }
        if (op == XZ_WS_OP_CLOSE) {
            ws->ok = false;
            return -1;
        }

        if (op == XZ_WS_OP_CONT || op == XZ_WS_OP_TEXT || op == XZ_WS_OP_BIN) {
            // 超限丢弃整条消息（含已拼接部分），避免覆盖临时区/越界
            if (ws->msg_len + p > RX_BUF / 2) {
                ws->frag_op = -1; ws->msg_len = 0; ws->rx_fill = ws->rx_pos = 0;
                continue;
            }
            // 负载先读进 rx 后半段临时区，再追加到已拼接消息（rx[0..msg_len)）。
            // 两段区不重叠（msg_len <= RX_BUF/2），用 memcpy。
            uint8_t* tmp = (uint8_t*)ws->rx + RX_BUF / 2;
            if (sread_payload(ws, tmp, p, frame_tmo) < 0) return -1;
            if (masked) for (size_t i = 0; i < p; ++i) tmp[i] ^= mk[i & 3];
            memcpy(ws->rx + ws->msg_len, tmp, p);
            ws->msg_len += p;
            if (ws->frag_op < 0) ws->frag_op = (op == XZ_WS_OP_CONT) ? XZ_WS_OP_BIN : op;
            if (fin) {
                int out_op = ws->frag_op;
                ws->frag_op = -1;
                if (op_code) *op_code = out_op;
                if (len) *len = ws->msg_len;
                if (data) *data = (const uint8_t*)ws->rx;
                ws->msg_len = 0;
                // 注意：不清 rx_fill/rx_pos，保留可能已多读进来的下一帧字节
                return (int)out_op;
            }
            continue;
        }
        // 未知 opcode：跳过负载继续（分块丢弃）
        {
            uint8_t tmp[512];
            size_t skip = p;
            while (skip) {
                size_t n = skip < sizeof(tmp) ? skip : sizeof(tmp);
                if (sread_payload(ws, tmp, n, frame_tmo) < 0) return -1;
                skip -= n;
            }
        }
        continue;
    }
}