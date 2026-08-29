// bsp/xz_client.cpp — 小智云协议实现（轻量版，Opus 音频模式）
#include "xz_client.h"
#include "xz_ws.h"
#include "cJSON.h"
#include "prefs.h"
#include "opus.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace boxpet::bsp {

static const char* TAG = "xz";

namespace {

struct XzClient {
    xz_ws_t*   ws = nullptr;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t lock = nullptr;
    volatile bool active = false;     // hello 已完成
    volatile bool running = false;
    char      url[192] = {0};
    char      token[160] = {0};
    XzTextFn  cb_text = nullptr;
    XzPcmFn   cb_pcm  = nullptr;
    XzStateFn cb_state= nullptr;
    XzAuthFn  cb_auth = nullptr;
    // 上行队列：esp_tls/mbedtls 未启用 MBEDTLS_THREADING，不可跨任务并发
    // 读写同一句柄（mic 任务写 + xz 任务读 → 状态损坏 → Interrupt WDT）。
    // 所有帧（音频 BIN + 控制 TEXT）由产生者入队，socket 只由 xz_task 读写。
    QueueHandle_t txq = nullptr;
    // 生命周期：ws 的释放权归 xz_task 唯一持有（UI 侧只发停止请求，
    // 绝不直接 close/free → 杜绝 double-free / use-after-free 堆损坏）。
    volatile bool task_alive = false;
    SemaphoreHandle_t task_done = nullptr;   // xz_task 退出时 give
    // 上行编码任务（常驻，PSRAM 栈）：mic 环→opus→txq
    TaskHandle_t enc_task = nullptr;
    volatile bool enc_alive = false;
};
static XzClient g;

// 上行帧：op=BIN/TEXT，len+data 为 WS payload。
struct XzTxPacket {
    uint16_t op;
    uint16_t len;
    uint8_t  data[528];   // 上行 VBR 音频帧（24k×60ms 常见 120~300B）留足余量；listen 文本 <60B
};

// 把队列中全部待发送帧交给 socket（仅 xz_task 调用）。
static void drain_tx_queue(xz_ws_t* ws) {
    XzTxPacket pk;
    int guard = 32;   // 单拍最多发 32 帧，防服务器卡住时本地堆积空转
    while (guard-- > 0 && xQueueReceive(g.txq, &pk, 0) == pdTRUE) {
        if (xz_ws_send(ws, pk.op, pk.data, pk.len, true) < 0) {
            ESP_LOGW(TAG, "tx send failed op=%u len=%u", pk.op, pk.len);
        }
    }
}

// [诊断] 上行丢帧累计（定位"服务器录音粗/短/嗒嗒"）
static volatile uint32_t s_ring_drop = 0;   // SPSC 环满丢弃
static volatile uint32_t s_tx_drop    = 0;   // txq 满丢弃

// 产生者线程统一入口：入队（非阻塞，队满丢帧防卡采集）
static bool tx_enqueue(int op, const void* data, size_t len) {
    if (!g.txq || len > sizeof(XzTxPacket::data)) return false;
    XzTxPacket pk;   // 栈上填充（多任务并发安全），入队即拷贝
    pk.op = (uint16_t)op;
    pk.len = (uint16_t)len;
    memcpy(pk.data, data, len);
    if (xQueueSend(g.txq, &pk, 0) != pdTRUE) {
        s_tx_drop = s_tx_drop + 1;
        ESP_LOGW(TAG, "tx queue full, drop %u bytes", (unsigned)len);
        return false;
    }
    return true;
}

// ===== Opus 编解码（线上频率 = 服务器协商值）=====
// 官方向导：服务器只支持 opus 音频帧（pcm 会被静默丢弃，hello 无人应答）。
// 服务器在 hello 回执里下发真实 audio_params.sample_rate。此前只在 on_text
// 里 LOG、却从不采纳——若下发不是 24000，本地仍按 24k 编码/重采样 → 服务器
// ASR/TTS 全乱（"听不清/看不对"）。现在把频率当成状态：hello 回执一到就在
// 会话开始（尚无音频流）前重建编解码器，上行编码/下行解码/双向重采样全部
// 跟随它，自愈任何协商频率（16k/24k/48k）。
#define DEVICE_SAMPLE_RATE 16000            // 本地 I2S 采集/播放恒定
#define OPUS_FRAME_MS      60
#define MIC_FRAME_16K      (DEVICE_SAMPLE_RATE * OPUS_FRAME_MS / 1000)  // 960 @16k = 60ms
#define MAX_NET_SAMPLES    (48000 * OPUS_FRAME_MS / 1000)               // 2880（48k×60ms，缓存上限）
static OpusDecoder* g_dec = nullptr;
static OpusEncoder* g_enc = nullptr;
static int  g_dec_rate = 0;                 // g_dec 当前频率（0=未建）
static int  s_net_rate = 0;                 // 0=未知(hello 回执前)；回执后=服务器频率
static uint32_t s_play_ctr = 0;             // 已播放声音帧计数（诊断下行是否通）
static int  xz_rate() {                      // 当前生效频率（未协商前退回 24k 假设）
    return (s_net_rate == 16000 || s_net_rate == 24000 || s_net_rate == 48000)
           ? s_net_rate : 24000;
}
// 大缓冲外移 .ext_ram.bss（PSRAM）省内部 RAM：TLS/xz_task 栈在内部 RAM 分配，
// 内部只留 31KB 时 24KB 栈连续块会碎片化分配失败。以下数组仅 CPU 访问、
// 不涉 flash/外设 DMA，放 PSRAM 安全（CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y）。
static int16_t  s_dec_pcm[MAX_NET_SAMPLES] __attribute__((section(".ext_ram.bss")));  // 解码输出（块内重采样源）
static int16_t  s_dec16[MAX_NET_SAMPLES]   __attribute__((section(".ext_ram.bss")));  // 解码后→16k 供播放
static int16_t  s_mic16000[MIC_FRAME_16K] __attribute__((section(".ext_ram.bss")));   // 上行 16k 累帧
static size_t   s_mic16_len = 0;              // s_mic16000 已填样本数
static int16_t  s_enc_buf[MAX_NET_SAMPLES] __attribute__((section(".ext_ram.bss")));  // 编码输入帧（协商频率）
static uint8_t  s_enc_pkt[2048]     __attribute__((section(".ext_ram.bss")));  // opus 上行帧缓冲

// 分块线性重采样 [nin]→[nout]（整数对齐分块：24k↔16k 比率 2/3，天然整帧对齐）。
// 纯整数定点，ESP32 上无浮点开销。
static void rsmp(int16_t* out, size_t nout, const int16_t* in, size_t nin) {
    if (nout == 0 || nin == 0) return;
    uint32_t step = (uint32_t)(((uint64_t)nin * 65536) / nout);
    uint32_t pos = 0;
    for (size_t j = 0; j < nout; ++j) {
        size_t i0 = pos >> 16;
        int     frac = (int)(pos & 0xFFFF);
        size_t  i1 = (i0 + 1 < nin) ? (i0 + 1) : i0;
        int32_t v = (int32_t)in[i0] * (65536 - frac) + (int32_t)in[i1] * frac;
        out[j] = (int16_t)(v >> 16);
        pos += step;
    }
}

// ===== mic 任务 → xz_task 无锁 SPSC 环形缓冲 =====
// mic_s 任务栈仅 3KB，opus_encode 需 6~10KB+：在其栈上编码直接溢出炸掉
// 返回地址（InstrFetchProhibited、寄存器被 PCM 数据淹没的崩溃形态）。
// 编码全部收回 xz_task（16KB 栈）：mic 回调只写环（写者移动 w），
// xz_task 排空编码（读者移动 r），SPSC 无锁安全。
#define MIC_RING 4096
static int16_t s_mic_ring[MIC_RING] __attribute__((section(".ext_ram.bss")));
static volatile size_t s_mic_w = 0;   // 生产者（mic_s 任务）
static volatile size_t s_mic_r = 0;   // 消费者（xz_task）

// ===== 上行编码任务：mic 环 → opus 编码 → txq（audio）=====
// opus_encode 需较大栈（60ms 帧实测 >16KB，曾溢出）；把它放到独立任务
//（PSRAM 栈 32KB），xz_task 只剩 TLS/WS 解析（内部 RAM 20KB 内即可——内
// 部碎片化时 32KB 连续块分不出，此乃实测根因）。encoder 不涉 flash 操作，
// PSRAM 栈安全：既是 SPSC 读者（mic_s 写、本任务读），又是 txq 写者（xz_task 读）。
static void xz_encode_task(void*) {
    for (;;) {
        if (!g.active) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        size_t w = s_mic_w, r = s_mic_r;
        while (r != w) {
            s_mic16000[s_mic16_len++] = s_mic_ring[r];
            r = (r + 1) % MIC_RING;
            s_mic_r = r;                 // 消费进度实时发布
            if (s_mic16_len == MIC_FRAME_16K) {
                // 上行**恒定 16k**（官方做法）：累满 60ms@16k=960 样本直接编码，
                // 绝不上采样到协商频率——服务器 ASR 上行按 16k 处理，之前按
                // 24k 上采样编码令服务器把 24k 帧当 16k 读 → 录音变粗/变短/嗒嗒。
                memcpy(s_enc_buf, s_mic16000, MIC_FRAME_16K * sizeof(int16_t));
                int nb = opus_encode(g_enc, s_enc_buf, MIC_FRAME_16K,
                                     s_enc_pkt, sizeof(s_enc_pkt));
                if (nb > 0) {
                    tx_enqueue(XZ_WS_OP_BIN, s_enc_pkt, (size_t)nb);
                }
                s_mic16_len = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));   // 10ms 级轮询：低延迟抬音
    }
}

// 编解码器按官方固件配置就位：
//  - 编码器（上行麦克风）**恒定为 16000Hz** —— 官方 audio_service 硬编码
//    encoder_sample_rate_=16000、AS_OPUS_ENC_CONFIG 固定 16k/mono/AUDIO/60ms，
//    绝不跟随服务器协商频率。服务器 ASR 上行一直按 16k 接收；此前我们按协商
//    (24k) 上采样编码，服务器把 24k 帧当 16k 处理 → 录音变粗/变短/嗒嗒。
//  - 解码器（下行 TTS）**跟随服务器协商频率**（hello 回执 sample_rate）。
// 频率变化仅重建解码器；编码器常驻（跨会话复用，mic 任务与 xz_task 竞态安全）。
static bool opus_codecs_for(int rate) {
    // 编码器：恒定 16k + AUDIO 应用模式 + VBR/DTX（对齐官方 AS_OPUS_ENC_CONFIG）
    if (!g_enc) {
        int err = OPUS_OK;
        g_enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &err);
        if (!g_enc) { ESP_LOGE(TAG, "opus encoder create @16k err=%d", err); return false; }
        opus_encoder_ctl(g_enc, OPUS_SET_VBR(1));          // enable_vbr=true
        opus_encoder_ctl(g_enc, OPUS_SET_DTX(1));          // enable_dtx=true
        opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(OPUS_AUTO));  // bitrate=auto
        opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(0));   // 官方 complexity=0，省算力
    }
    // 解码器：跟随服务器协商频率
    if (!g_dec || g_dec_rate != rate) {
        if (g_dec) { opus_decoder_destroy(g_dec); g_dec = nullptr; g_dec_rate = 0; }
        int err = OPUS_OK;
        g_dec = opus_decoder_create(rate, 1, &err);
        if (!g_dec) { ESP_LOGE(TAG, "opus decoder create @%d err=%d", rate, err); return false; }
        g_dec_rate = rate;
    }
    ESP_LOGI(TAG, "opus codec ready (enc=16k dec=%dHz mono %dms)", rate, OPUS_FRAME_MS);
    return true;
}
// 兼容旧调用：会话建立即按当前生效频率建好默认编解码器。
static void opus_init_codecs() { opus_codecs_for(xz_rate()); }
// 注意：编解码器跨会话持久复用、不随会话销毁——mic 任务的 xz_send_audio
// 与 xz_task 存在编码竞态窗口，任何"会话结束即 free"都可能让 mic 正在
// 使用的 encoder 悬空（use-after-free）。固定占用 <50KB，比竞态风险划算。

// 简单 URL 解析：scheme://host[:port]/path
static bool parse_url(const char* url, bool* tls, char* host, int hl,
                      int* port, char* path, int pl) {
    const char* p = url;
    if (strncmp(p, "wss://", 6) == 0) { *tls = true;  p += 6; }
    else if (strncmp(p, "ws://", 5) == 0) { *tls = false; p += 5; }
    else return false;
    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    size_t host_len;
    if (slash && colon && colon < slash) { host_len = (size_t)(colon - p); *port = atoi(colon + 1); }
    else if (slash) { host_len = (size_t)(slash - p); *port = *tls ? 443 : 80; }
    else { host_len = strlen(p); *port = *tls ? 443 : 80; }
    if (host_len >= (size_t)hl) host_len = hl - 1;
    memcpy(host, p, host_len);
    host[host_len] = 0;
    snprintf(path, pl, "%s", slash ? slash : "/");
    return true;
}

static void set_state(int s) {
    g.active = (s != 0);
    if (g.cb_state) g.cb_state(s);
}

// 处理服务器文本 JSON
static void on_text(const char* msg) {
    cJSON* root = cJSON_Parse(msg);
    if (!root) { if (g.cb_text) g.cb_text(msg); return; }
    const char* type = "";
    cJSON* t = cJSON_GetObjectItem(root, "type");
    if (t && cJSON_IsString(t)) type = t->valuestring ? t->valuestring : "";
    if (strcmp(type, "hello") == 0) {
        // 服务器确认：audio_params 决定收发格式。**采纳**它下发的 sample_rate
        // 并据此重建编解码器（此刻会话尚未开始、无音频流 → 安全）。若服务器
        // 实际协商不是生产假设的 24000，此前按 24k 编码会导致 ASR 全乱。
        cJSON* ap = cJSON_GetObjectItem(root, "audio_params");
        int sr = -1;
        if (ap) {
            cJSON* f = cJSON_GetObjectItem(ap, "format");
            cJSON* srr = cJSON_GetObjectItem(ap, "sample_rate");
            if (srr && cJSON_IsNumber(srr)) sr = (int)srr->valuedouble;
            ESP_LOGI(TAG, "server audio_params: format=%s sample_rate=%d",
                     (f && cJSON_IsString(f)) ? f->valuestring : "?",
                     sr);
        }
        if (sr == 16000 || sr == 24000 || sr == 48000) {
            s_net_rate = sr;                       // 采纳服务器频率
            opus_codecs_for(sr);                   // 会话前重建（频率变化则重建）
        } else {
            ESP_LOGW(TAG, "unexpected sample_rate=%d, keep %d", sr, xz_rate());
        }
        set_state(1);
    } else if (strcmp(type, "hello_error") == 0) {
        cJSON* m = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "server hello_error: %s",
                 (m && cJSON_IsString(m)) ? m->valuestring : "");
        set_state(0);
    } else if (strcmp(type, "tts") == 0) {
        // 可含 sentence/text 字段：显示给屏
        cJSON* txt = cJSON_GetObjectItem(root, "text");
        if (!txt) txt = cJSON_GetObjectItem(root, "sentence");
        if (txt && cJSON_IsString(txt) && g.cb_text) g.cb_text(txt->valuestring);
        cJSON* st = cJSON_GetObjectItem(root, "state");
        if (st && cJSON_IsString(st) && strcmp(st->valuestring, "start") == 0)
            set_state(2);
        else if (st && cJSON_IsString(st) && strcmp(st->valuestring, "stop") == 0)
            set_state(3);
    } else if (strcmp(type, "stt") == 0) {
        cJSON* txt = cJSON_GetObjectItem(root, "text");
        if (txt && cJSON_IsString(txt) && g.cb_text) {
            char buf[160];
            snprintf(buf, sizeof(buf), "你说：%.130s", txt->valuestring);
            g.cb_text(buf);
        }
    } else if (strcmp(type, "llm") == 0 || strcmp(type, "chat") == 0) {
        cJSON* txt = cJSON_GetObjectItem(root, "text");
        if (txt && cJSON_IsString(txt) && g.cb_text) g.cb_text(txt->valuestring);
        cJSON* st = cJSON_GetObjectItem(root, "state");
        if (st && cJSON_IsString(st) && strcmp(st->valuestring, "start") == 0)
            set_state(2);
        else if (st && cJSON_IsString(st) && strcmp(st->valuestring, "stop") == 0)
            set_state(3);
    } else {
        // 未识别消息类型：打日志便于排查服务器到底回了什么
        ESP_LOGI(TAG, "recv other: %.200s", msg);
    }
    cJSON_Delete(root);
}

// 设备 Client-Id（UUID v4）：与 OTA 注册同一持久化值（服务器按
// Device-Id + Client-Id 匹配绑定表，两阶段不一致会导致 hello 无人应答）。
// 与激活码槽位分离，绝不互相覆盖。
static void ensure_client_id(char* out, size_t len) {
    if (prefs_get_xz_client(out, len) && strlen(out) == 36) return;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, len, "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    prefs_set_xz_client(out);
}

// 任务唯一出口：释放 ws 归本线程（socket 单所有者）。give 放最后：
// xz_stop 从 task_done 返回时，可保证 ws/active/状态通知均已落地。
static void task_exit(xz_ws_t* ws) {
    if (ws) xz_ws_close(ws);
    g.ws = nullptr;
    g.running = false;
    g.task_alive = false;
    set_state(0);   // 断开通知（幂等）
    if (g.task_done) xSemaphoreGive(g.task_done);
    vTaskDeleteWithCaps(nullptr);   // WithCaps 任务自删须用配套 API，否则泄漏栈
}

static void xz_task(void*) {
    bool tls = false;
    char host[128] = {0};
    char path[128] = {0};
    int  port = 0;
    if (!parse_url(g.url, &tls, host, sizeof(host), &port, path, sizeof(path))) {
        ESP_LOGE(TAG, "bad url: %s", g.url);
        task_exit(nullptr);
        return;
    }
    ESP_LOGI(TAG, "connecting %s://%s:%d%s", tls ? "wss" : "ws", host, port, path);
    char extra[512] = {0};
    // 官方（docs/websocket.md）握手头：Authorization / Protocol-Version /
    // Device-Id / Client-Id。这两个标识必须与 OTA 注册时**完全一致**，否则
    // 服务器按 (MAC, Client-Id, token) 匹配不到已绑定设备，hello 便无人应答：
    //   - Device-Id  = 小写冒号分格 MAC（与 OTA 注册用的 did 一致）
    //   - Client-Id = 持久化的 UUID v4（与 OTA 注册用的 client_uuid 一致）
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char did[24], cid[40];
    snprintf(did, sizeof(did), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ensure_client_id(cid, sizeof(cid));
    snprintf(extra, sizeof(extra),
        "Authorization: Bearer %s\r\n"
        "Protocol-Version: 1\r\n"
        "Device-Id: %s\r\n"
        "Client-Id: %s\r\n",
        g.token, did, cid);

    xz_ws_cfg_t cfg = {};
    cfg.host = host;
    cfg.port = (uint16_t)port;
    cfg.path = path;
    cfg.tls  = tls;
    cfg.extra_headers = extra;
    cfg.timeout_ms = 8000;
    xz_ws_t* ws = xz_ws_connect(&cfg);
    if (!ws) { task_exit(nullptr); return; }
    g.ws = ws;

    // hello：官方要求 audio_params.format 必须为 "opus"（pcm 服务器不支持会
    // 静默丢弃 hello，导致"hello sent, waiting server..."后无任何回包）。
    // 声明 24000 作为期望值；服务器在回执里会给出它实际使用的 sample_rate，
    // hello 处理分支会据此重建编解码器（见 on_text→hello），收发自愈。
    opus_init_codecs();
    char hello[256];
    snprintf(hello, sizeof(hello),
        "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
        "\"channels\":1,\"frame_duration\":%d},"
        "\"features\":{\"mcp\":false,\"aec\":false}}",
        24000, OPUS_FRAME_MS);
    if (xz_ws_send_text(ws, hello) < 0) {
        ESP_LOGE(TAG, "hello send failed");
        task_exit(ws);
        return;
    }
    ESP_LOGI(TAG, "hello sent, waiting server... (did=%s cid=%s)", did, cid);

    int rc = 0;
    int op = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
    int hello_waits = 0;   // hello 未确认期间的读超时轮数（每轮≈8s）
    int64_t last_ping_ms = esp_timer_get_time() / 1000;
    // 上行数据统一在本任务写出（socket 单所有者原则）
    drain_tx_queue(ws);
    while (g.running) {
        // 会话 active 后，上传期下行是空闲的（recv 拿不到数据）。若像旧逻辑
        // 只在"收到消息"那拍才把空闲超时缩到 100ms，则上传全程 recv 会以
        // 8s 的帧等待阻塞、绝不返回 → 上行队列得不到及时排空 → 积压满 24 →
        // 持续丢音频帧 → 服务器 ASR 只收到残缺语音 → 回复内容错乱（症状：
        // "tx queue full, drop N" + "有回复但不是对我说的话"）。
        // 修正：只要 active 就先把读空闲超时钉在 100ms，并在每次进循环先排空
        // 一次，让上行以 100ms 节拍稳定流出，绝不阻塞积压。
        if (g.active) { xz_ws_set_idle_timeout(ws, 100); drain_tx_queue(ws); }
        rc = xz_ws_recv(ws, &op, &data, &len);
        if (rc < 0) {
            if (rc == -2) {
                if (!g.active) {
                    hello_waits++;
                    ESP_LOGI(TAG, "waiting server hello... (%ds)", hello_waits * 8);
                    if (hello_waits == 1) {
                        // 官方服务端在查询设备绑定状态期间（1s 竞态窗口）会
                        // 静默丢弃早到的消息（connection.py::_route_message），
                        // 连接建立后毫秒级发出的 hello 恰易撞上该窗口被丢。
                        // 8s 仍无回执则主动重发一次 hello。
                        ESP_LOGW(TAG, "hello no reply, resend");
                        xz_ws_send_text(ws, hello);
                    } else if (hello_waits >= 3) {
                        // 约 24s 仍无回执 → 设备未绑定/身份不匹配，主动断开，
                        // 避免 UI 永远停在"连接中"。
                        ESP_LOGE(TAG, "hello timeout: no server reply (device not bound? re-register)");
                        break;
                    }
                } else {
                    // 会话期 100ms 空转：排空上行队列；每 25s 补发保活 Ping
                    drain_tx_queue(ws);
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - last_ping_ms >= 25000) {
                        last_ping_ms = now_ms;
                        xz_ws_send(ws, 0x9, "hb", 2, true);
                    }
                }
                continue;
            }
            break;
        }
        hello_waits = 0;
        // hello 一旦确认（active），把 b0 等待缩短到 100ms：既保证上行队列
        // 及时 flush，又让 read 频繁返回、避免长时间占用 socket。
        if (g.active) xz_ws_set_idle_timeout(ws, 100);
        drain_tx_queue(ws);   // 收到下行即说明链路通畅，顺带 flush 上行
        if (op == XZ_WS_OP_TEXT) {
            char* t = (char*)malloc(len + 1);
            if (t) { memcpy(t, data, len); t[len] = 0; on_text(t); free(t); }
        } else if (op == XZ_WS_OP_BIN) {
            // 服务器 BIN opcode 帧的实际内容有多种（docs/websocket.md + 实测）：
            //  1) BP2 封装(v2)：16B 头 [version(2) type(2) reserved(4) ts(4) size(4)]
            //  2) BP3 封装(v3)： 4B 头 [type(1) reserved(1) size(2)]
            //  3) JSON 文本用 BIN 帧发送（实测 hello 回执即如此，首字节 '{'，
            //     必须嗅探转给 on_text，否则服务器应答丢失、永远等不到音频）
            //  4) v1：裸 Opus → 解码失败时打 hex dump 定位，不再猜格式
            const uint8_t* p = data;
            const uint8_t* pay = nullptr;
            uint32_t psize = 0;
            bool handled = false;
            if (len >= 16 && p[0] == 0 && p[1] == 2) {   // v2（首字段 version=2）
                uint16_t type = (uint16_t)((p[2] << 8) | p[3]);
                psize = ((uint32_t)p[12] << 24) | ((uint32_t)p[13] << 16)
                      | ((uint32_t)p[14] << 8) | (uint32_t)p[15];
                if (type == 1 && psize == len - 16) {
                    // BP2 type=1：JSON 消息
                    char* t = (char*)malloc(psize + 1);
                    if (t) { memcpy(t, p + 16, psize); t[psize] = 0; on_text(t); free(t); }
                } else if (type != 0) {
                    ESP_LOGI(TAG, "bin v2 type=%u len=%u", type, (unsigned)len);
                } else if (psize == len - 16) {
                    pay = p + 16;
                } else {
                    ESP_LOGE(TAG, "bin v2 size mismatch %u/%u", (unsigned)psize, (unsigned)len);
                }
                handled = true;
            } else if (len >= 4 && p[0] == 0 && p[1] == 0 &&
                       (uint32_t)((p[2] << 8) | p[3]) == len - 4) {  // v3 音频
                psize = (uint32_t)((p[2] << 8) | p[3]);
                pay = p + 4;
                handled = true;
            } else if (len > 0 && (p[0] == '{' || p[0] == '[')) {
                // BIN 帧装着 JSON：转文本通道处理
                char* t = (char*)malloc(len + 1);
                if (t) { memcpy(t, data, len); t[len] = 0; on_text(t); free(t); }
                handled = true;
            }
            if (!handled && g_dec && len > 0) {
                // 裸 Opus（v1）兜底：解不动再 dump 帧头定位
                int ns = opus_decode(g_dec, data, (opus_int32)len,
                                     s_dec_pcm, (int)(sizeof(s_dec_pcm)/sizeof(s_dec_pcm[0])), 0);
                if (ns > 0 && g.cb_pcm) {
                    int rate = xz_rate();
                    size_t n16 = (size_t)ns * DEVICE_SAMPLE_RATE / rate;
                    rsmp(s_dec16, n16, s_dec_pcm, (size_t)ns);   // 协商频率→16k 供播放
                    g.cb_pcm(s_dec16, n16);
                    if ((++s_play_ctr & 0x3F) == 1)              // 每 64 帧确认一次播放
                        ESP_LOGI(TAG, "TTS play raw frame=%u ns=%d n16=%u", (unsigned)s_play_ctr, ns, (unsigned)n16);
                }
                else if (ns < 0) {
                    ESP_LOGE(TAG, "bin raw opus err=%d len=%u head:", ns, (unsigned)len);
                    ESP_LOG_BUFFER_HEX(TAG, data, (len < 32 ? len : 32));
                }
            } else if (pay && g_dec) {
                int ns = opus_decode(g_dec, pay, (opus_int32)psize,
                                     s_dec_pcm, (int)(sizeof(s_dec_pcm)/sizeof(s_dec_pcm[0])), 0);
                if (ns > 0 && g.cb_pcm) {
                    int rate = xz_rate();
                    size_t n16 = (size_t)ns * DEVICE_SAMPLE_RATE / rate;
                    rsmp(s_dec16, n16, s_dec_pcm, (size_t)ns);   // 协商频率→16k 供播放
                    g.cb_pcm(s_dec16, n16);
                    if ((++s_play_ctr & 0x3F) == 1)              // 每 64 帧确认一次播放
                        ESP_LOGI(TAG, "TTS play env frame=%u ns=%d n16=%u", (unsigned)s_play_ctr, ns, (unsigned)n16);
                }
                else if (ns < 0) ESP_LOGW(TAG, "opus decode err=%d len=%u", ns, (unsigned)psize);
            } else if (!pay && !handled && len > 0) {
                // 既非已知封装、又未解码：外部包裹格式未知 → 打印首帧头定位
                ESP_LOGW(TAG, "bin unhandled len=%u head:", (unsigned)len);
                ESP_LOG_BUFFER_HEX(TAG, data, (len < 16 ? len : 16));
            }
        }
    }
    task_exit(ws);
}

// ===== OTA 注册（自动获取 token/激活码）=====
// 官方 OTA 接口：POST https://<host>/xiaozhi/ota/，头带 Device-Id/Client-Id/User-Agent。
// 未绑定 → 返回 activation.code（6位激活码，去 xiaozhi.me 控制台绑定）；
// 已绑定 → 返回 websocket.url + websocket.token（据此连接）。
struct AuthCtx {
    char      host[128];
    int       port;
    XzAuthFn  cb;
};

static void auth_report(int status) {
    if (g.cb_auth) g.cb_auth(status);
}

static void xz_auth_task(void* arg) {
    AuthCtx* c = (AuthCtx*)arg;
    if (!c) { vTaskDelete(nullptr); return; }
    int status = 0;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // Device-Id：官方为小写冒号分隔 MAC
    char did[24], cid[40], ua[48];
    snprintf(did, sizeof(did), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    // Client-Id：官方为 UUID v4（独立槽持久化；注册与 WS 握手必须同值，
    // 且绝不被激活码覆盖）
    static char client_uuid[40];
    if (!client_uuid[0]) ensure_client_id(client_uuid, sizeof(client_uuid));
    snprintf(cid, sizeof(cid), "%s", client_uuid);
    snprintf(ua, sizeof(ua), "boxpet/%s", esp_app_get_description()->version);

    // body：对齐官方 GetSystemInfoJson 顶层结构
    cJSON* b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "version", 2);
    cJSON_AddStringToObject(b, "language", "zh-CN");
    uint32_t flash_sz = 0;
    esp_flash_get_size(NULL, &flash_sz);
    cJSON_AddNumberToObject(b, "flash_size", flash_sz);
    cJSON_AddNumberToObject(b, "minimum_free_heap_size", esp_get_free_heap_size());
    cJSON_AddStringToObject(b, "mac_address", did);
    cJSON_AddStringToObject(b, "uuid", cid);
    cJSON_AddStringToObject(b, "chip_model_name", CONFIG_IDF_TARGET);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    cJSON* chip = cJSON_AddObjectToObject(b, "chip_info");
    cJSON_AddNumberToObject(chip, "model", ci.model);
    cJSON_AddNumberToObject(chip, "cores", ci.cores);
    cJSON_AddNumberToObject(chip, "revision", ci.revision);
    cJSON_AddNumberToObject(chip, "features", ci.features);
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON* ap = cJSON_AddObjectToObject(b, "application");
    cJSON_AddStringToObject(ap, "name", app->project_name);
    cJSON_AddStringToObject(ap, "version", app->version);
    // 对齐官方 GetSystemInfoJson 的 application 字段：compile_time=日期T时间Z，
    // 并携带 idf_version 与 elf_sha256，供服务器严格校验时通过。
    char ct[64];
    snprintf(ct, sizeof(ct), "%sT%sZ", app->date, app->time);
    cJSON_AddStringToObject(ap, "compile_time", ct);
    cJSON_AddStringToObject(ap, "idf_version", app->idf_ver);
    char sha[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha + i * 2, sizeof(sha) - i * 2, "%02x", app->app_elf_sha256[i]);
    cJSON_AddStringToObject(ap, "elf_sha256", sha);
    char* body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    if (!body) { free(c); auth_report(0); vTaskDelete(nullptr); return; }

    esp_http_client_config_t cfg = {};
    cfg.host = c->host;                 // 显式 host/port/path，绕开 URL 字符串再解析
    cfg.port = c->port;
    cfg.path = "/xiaozhi/ota/";
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // 挂证书链，启用服务器校验
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGE(TAG, "auth: http init fail"); free(c); auth_report(0); vTaskDelete(nullptr); return; }

    esp_http_client_set_header(cli, "Device-Id", did);
    esp_http_client_set_header(cli, "Client-Id", cid);
    // Activation-Version：官方在 efuse 烧录了 Serial-Number 才发 "2"（且需再带
    // Serial-Number 头）；本机未烧录序列号，只能发 "1"，否则服务器校验失败返回 400。
    esp_http_client_set_header(cli, "Activation-Version", "1");
    esp_http_client_set_header(cli, "User-Agent", ua);
    esp_http_client_set_header(cli, "Accept-Language", "zh-CN");
    esp_http_client_set_header(cli, "Content-Type", "application/json");

    char* resp = (char*)calloc(4096, 1);   // 放堆上：栈空间需留给 TLS 握手
    bool ok = false;
    // 人工模式读取响应：perform() 会自动把整个 body 读完，期间 flush 掉 socket，
    // 之后再 read_response 会因无数据返回 0。改用
    // open -> write -> fetch_headers -> read_response 手动拿回完整 body。
    esp_err_t err = esp_http_client_open(cli, (int)strlen(body));
    if (err == ESP_OK) {
        esp_http_client_write(cli, body, (int)strlen(body));
        esp_http_client_fetch_headers(cli);
        ESP_LOGI(TAG, "ota perform err=0x%x status=%d", err,
                 esp_http_client_get_status_code(cli));
        int rl = esp_http_client_read_response(cli, resp, 4095);
        if (rl > 0) { resp[rl] = 0; ok = true; }
    } else {
        ESP_LOGE(TAG, "ota open failed err=0x%x", err);
    }
    esp_http_client_cleanup(cli);

    cJSON* root = (ok && resp) ? cJSON_Parse(resp) : nullptr;
    if (root) {
        ESP_LOGI(TAG, "ota resp: %.300s", resp);
        // 待绑定：优先展示激活码
        cJSON* act = cJSON_GetObjectItem(root, "activation");
        cJSON* code = act ? cJSON_GetObjectItem(act, "code") : nullptr;
        if (code && cJSON_IsString(code) && code->valuestring && code->valuestring[0]) {
            prefs_set_xz_code(code->valuestring);
            status = 2;
        } else {
            // 已绑定：websocket.url + token
            cJSON* ws = cJSON_GetObjectItem(root, "websocket");
            cJSON* tok = ws ? cJSON_GetObjectItem(ws, "token") : nullptr;
            if (tok && cJSON_IsString(tok) && tok->valuestring && tok->valuestring[0]) {
                prefs_set_xz_token(tok->valuestring);
                // 用服务器下发的 ws 地址覆盖手填地址
                cJSON* u = cJSON_GetObjectItem(ws, "url");
                if (u && cJSON_IsString(u) && u->valuestring && u->valuestring[0]) {
                    NetConfig nc;
                    if (prefs_get_net(&nc)) {
                        snprintf(nc.xz_url, sizeof(nc.xz_url), "%s", u->valuestring);
                        prefs_set_net(&nc);
                    }
                }
                status = 1;
            }
        }
        cJSON_Delete(root);
    } else if (!ok) {
        ESP_LOGE(TAG, "ota perform failed");
    }
    if (resp) free(resp);
    ESP_LOGI(TAG, "ota auth status=%d", status);
    free(c);
    auth_report(status);
    vTaskDelete(nullptr);
}

}  // namespace

void xz_set_callbacks(XzTextFn text, XzPcmFn pcm, XzStateFn state) {
    g.cb_text = text;
    g.cb_pcm  = pcm;
    g.cb_state= state;
}

esp_err_t xz_start(const char* url, const char* token) {
    if (!url || !token) return ESP_ERR_INVALID_ARG;
    // 上一会话任务若还没退出（协作式关闭在途），先等它交回所有权，
    // 杜绝两个 xz_task 并存竞写 g.ws。
    if (g.running || g.task_alive) xz_stop();
    if (!g.lock) g.lock = xSemaphoreCreateRecursiveMutex();
    if (!g.task_done) g.task_done = xSemaphoreCreateBinary();
    if (!g.task_done) return ESP_ERR_NO_MEM;
    // 上行队列常驻（xQueue 线程安全；销毁重建会与仍在 drain 的旧任务竞态）。
    // 新会话前非阻塞排空旧会话残留帧。
    // 上行队列：会话期 xz_task 在读取慢速下行帧时 drain 会暂停（单 socket
    // 单所有者，不能跨任务写），而编码器仍按 60ms/帧持续入队；16 格(=~0.96s)
    // 在"服务器开始回话 + 用户尾音"重叠窗口会被打满丢帧。加深到 24 格(~1.44s)
    // 吸收该窗口，其余 RAM 已由 PSRAM 腾出。
    if (!g.txq) g.txq = xQueueCreate(24, sizeof(XzTxPacket));
    if (!g.txq) return ESP_ERR_NO_MEM;
    XzTxPacket drop;
    while (xQueueReceive(g.txq, &drop, 0) == pdTRUE) {}
    while (xSemaphoreTake(g.task_done, 0) == pdTRUE) {}   // 清残留 give 令牌
    snprintf(g.url, sizeof(g.url), "%s", url);
    snprintf(g.token, sizeof(g.token), "%s", token);
    g.running = true;
    g.active = false;
    g.task_alive = true;
    s_mic16_len = 0;   // 上行攒帧缓冲复位，丢弃上次会话残缺片段
    s_mic_w = s_mic_r = 0;   // mic 环形缓冲复位（此刻 mic_s 未运行/无 active）
    // 编码已迁至独立 xz_encode_task（PSRAM 栈）：xz_task 只剩 TLS/WS 解析。
    // 栈必须内部 RAM（NVS/证书库读取会操作 spi_flash，psram 栈 + flash =
    // cache 禁用窗口栈不可访问 → assert，实测崩溃）。内部堆碎片化实测分
    // 不出 32KB 连续块，故取 20KB（TLS+lwIP+WS 实测峰值可容纳）。
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t stack_words = (int_free > 24576) ? 20480 : 16384;
    ESP_LOGI(TAG, "xz task stack=%lu (internal free=%lu)",
             (unsigned long)stack_words, (unsigned long)int_free);
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
                        xz_task, "xz", stack_words, nullptr, 5, &g.task, 0,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xz task create failed caps internal stack=%lu",
                 (unsigned long)stack_words);
        g.running = false;
        g.task_alive = false;
        return ESP_ERR_NO_MEM;
    }
    // 编码任务：常驻、PSRAM 栈（encoder 不涉 flash → 安全），空闲低耗。
    // opus_encode 在 60ms@16k 帧下实测栈需求 >16KB（曾溢出）→ 扩到 32KB。
    if (!g.enc_alive) {
        // 优先级/核：实测 xz_task(TLS/WS) 与 enc 同优先级(5)挤 core 0，
        // 下行/TLS 重活会把编码饿到 ~40% 实时 → SPSC 环写满 `ringDrop` 持续
        // 增长（日志 `up enc ringDrop` 上行录音残缺/短/粗）。故编码拨高到
        // 优先级 8、独立 core 1（APP_CPU）与 xz_task 分核，保证永远实时排空环。
        BaseType_t ok2 = xTaskCreatePinnedToCoreWithCaps(
                            xz_encode_task, "xz_enc", 32768, nullptr, 8,
                            &g.enc_task, 1,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok2 != pdPASS) {
            ESP_LOGW(TAG, "xz enc task create failed (audio uplink disabled)");
        } else {
            g.enc_alive = true;
            ESP_LOGI(TAG, "xz encoder task started (prio8 core1 psram stack)");
        }
    }
    return ESP_OK;
}

// 停止：只发请求 + 等 task_exit 交回信号。ws/编解码器均由 xz_task 自己
// 释放（socket 与 codec 的单所有者），任何路径都不跨线程 free。
void xz_stop() {
    g.running = false;
    if (g.task_alive && g.task_done) {
        // 稳态 recv 节拍 ≤100ms；帧中途最长 8s。等不到也放手——
        // 任务随后自行 task_exit 清理，绝不触碰其正在使用的资源。
        xSemaphoreTake(g.task_done, pdMS_TO_TICKS(9000));
    }
    g.task = nullptr;
}

bool xz_active() { return g.active; }

// 官方会话控制消息（docs/websocket.md + WebSocketProtocol）：
//   开始拾音           → {"type":"listen","state":"start","mode":"auto"}
//   （detect 是"唤醒词检测上报"语义，须带 text 字段，无 text 时服务端
//     只重置状态不进入拾音会话，别混用）
//   结束本段说话       → {"type":"listen","state":"stop"}
// 一律入上行队列，由 xz_task 写 socket（mbedtls 非线程安全）。
// 长度用 sizeof(literal)-1 编译期求值，避免手数字节出错。
void xz_talk_begin() {
    if (!g.active) return;
    static const char m[] = "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
    tx_enqueue(XZ_WS_OP_TEXT, m, sizeof(m) - 1);
}
void xz_talk_end() {
    if (!g.active) return;
    static const char m[] = "{\"type\":\"listen\",\"state\":\"stop\"}";
    tx_enqueue(XZ_WS_OP_TEXT, m, sizeof(m) - 1);
}

void xz_send_audio(const int16_t* pcm, size_t n) {
    // 生产者上下文 = mic_s 任务（栈仅 3KB）：只允许写环 + 移动写指针，
    // 绝不调用 opus_encode（需 6~10KB+ 栈 → 溢出炸返回地址崩溃）。
    // SPSC 纪律：本函数绝不触碰读指针。环满（256ms 积压）丢新帧——
    // 消费者以 100ms 节拍排空，正常运行不可能满。
    if (!g.active) return;
    size_t w = s_mic_w, r = s_mic_r;
    size_t used = (w - r + MIC_RING) % MIC_RING;
    if (n > MIC_RING - 1 - used) { s_ring_drop = s_ring_drop + 1; return; }   // 满：丢弃（保 live 语义）
    for (size_t i = 0; i < n; ++i) s_mic_ring[(w + i) % MIC_RING] = pcm[i];
    s_mic_w = (w + n) % MIC_RING;   // 数据先落、写指针后动（release 语义）
}

void xz_auth_begin(const char* ws_url, XzAuthFn cb) {
    g.cb_auth = cb;
    if (!ws_url || !ws_url[0]) { if (g.cb_auth) g.cb_auth(0); return; }
    // OTA/激活接口固定在小智官方 OTA 域名 api.tenclass.net（与 ws 对话域名 api.xiaozhi.me 不同源）。
    // 响应含 activation.code（待绑定）或 websocket.url+token（已绑定），成功后回写 xz_url。
    AuthCtx* c = (AuthCtx*)calloc(1, sizeof(AuthCtx));
    if (!c) { if (g.cb_auth) g.cb_auth(0); return; }
    snprintf(c->host, sizeof(c->host), "api.tenclass.net");
    c->port = 443;
    ESP_LOGI(TAG, "ota register -> https://%s/xiaozhi/ota/", c->host);
    BaseType_t ok = xTaskCreate(xz_auth_task, "xz_auth", 10240, c, 5, nullptr);
    if (ok != pdPASS) { free(c); if (g.cb_auth) g.cb_auth(0); }
}

}  // namespace boxpet::bsp