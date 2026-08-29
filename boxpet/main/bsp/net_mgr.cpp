// bsp/net_mgr.cpp — WiFi AP 配网 + STA 按需连接
// 参考 xiaozhi-esp32 各板卡 wifi_board 的 AP+HTTPD 配网流程（需求4/需求6）。
// 省电设计：平时 esp_wifi 未初始化，Light Sleep 不受影响；
// 配网/聊天时按需启动，用完 stop 释放 ~50KB 内部 RAM。
#include "net_mgr.h"
#include "prefs.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace boxpet::bsp {

static const char* TAG = "net";
static constexpr int kApTimeoutSec = 300;      // 配网 5 分钟超时（需求4）
static const char* kApPassword = "12345678";   // 固定密码（需求4）

static NetMode s_mode = NetMode::Off;
static bool s_inited = false;          // netif/event loop 已建
static bool s_wifi_inited = false;
static esp_netif_t* s_netif_ap = nullptr;
static esp_netif_t* s_netif_sta = nullptr;
static httpd_handle_t s_httpd = nullptr;
static esp_timer_handle_t s_ap_timeout = nullptr;
static int s_sta_retry = 0;

// ===== 工具 =====
static void url_decode(char* s) {
    char* w = s;
    for (; *s; ++s) {
        if (*s == '+') { *w++ = ' '; continue; }
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *w++ = (char)strtol(hex, nullptr, 16);
            s += 2;
            continue;
        }
        *w++ = *s;
    }
    *w = 0;
}

// 从 form-urlencoded body 提取字段（就地解码）
static bool form_get(char* body, const char* key, char* out, size_t out_len) {
    size_t klen = strlen(key);
    char* p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            char* e = strchr(p, '&');
            size_t n = e ? (size_t)(e - (p + klen + 1)) : strlen(p + klen + 1);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, p + klen + 1, n);
            out[n] = 0;
            url_decode(out);
            return true;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    return false;
}

// ===== HTTP 配置页（需求4：LLM Key/URL/Model + WiFi 扫描/密码）=====
static const char kIndexHtml[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Pet配置</title><style>"
"body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px}"
"label{display:block;margin:10px 0 4px;font-weight:bold}"
"input,select{width:100%;box-sizing:border-box;padding:8px;font-size:15px}"
"button{width:100%;margin-top:16px;padding:12px;font-size:16px;"
"background:#2E86C1;color:#fff;border:0;border-radius:6px}"
"</style></head><body><h2>🐱 Pet 配置</h2><form action='/save' method='post'>"
"<label>WiFi 名称（点输入框弹出扫描列表）</label>"
"<input list=dl name=wifi_ssid required><datalist id=dl></datalist>"
"<label>WiFi 密码</label><input type=password name=wifi_pass>"
"<label>小智云地址（语音对话用）</label>"
"<input name=xz_url placeholder='wss://api.xiaozhi.me/xiaozhi/v1/'>"
"<label>小智 Token：设备首次连接自动获取（无需填写）</label>"
"<button type=submit>保存并重启</button></form>"
"<script>fetch('/scan').then(r=>r.json()).then(l=>{var d=document.getElementById('dl');"
"l.forEach(function(w){var o=document.createElement('option');o.value=w.s;"
"d.appendChild(o);});}).catch(function(){});</script>"
"</body></html>";

// GET / —— 配置表单
static esp_err_t h_index(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

// GET /scan —— 周边 WiFi JSON：[{"s":"ssid","r":-52},...]
static esp_err_t h_scan(httpd_req_t* req) {
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_wifi_scan_start(&cfg, true /*block*/);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 12) n = 12;   // 只回最强的 12 个，控制响应体积
    wifi_ap_record_t* rec = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * n);
    if (!rec) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) { free(rec); n = 0; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    for (int i = 0; i < (int)n; ++i) {
        char item[96];
        // SSID 中可能有引号/反斜杠：只保留可打印非特殊字符（简化转义）
        char ssid[33] = {0};
        for (int j = 0; j < 32 && rec[i].ssid[j]; ++j) {
            char c = rec[i].ssid[j];
            ssid[j] = (c == '"' || c == '\\' || (uint8_t)c < 0x20) ? '_' : c;
        }
        snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"r\":%d}",
                 i ? "," : "", ssid, rec[i].rssi);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, nullptr, 0);
    free(rec);
    return ESP_OK;
}

// POST /save —— 保存 NVS 后重启（需求4：重启后自动读取凭据连 STA）
static esp_err_t h_save(httpd_req_t* req) {
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
    }
    char* body = (char*)malloc(total + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
    int recvd = 0;
    while (recvd < total) {
        int n = httpd_req_recv(req, body + recvd, total - recvd);
        if (n <= 0) { free(body); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); }
        recvd += n;
    }
    body[total] = 0;

    NetConfig c = {};
    form_get(body, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    form_get(body, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    form_get(body, "xz_url",   c.xz_url,   sizeof(c.xz_url));
    free(body);
    if (!c.wifi_ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid empty");
    }
    prefs_set_net(&c);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<meta charset=utf-8>已保存，设备重启中…");
    // 800ms 后重启（让响应送达浏览器）
    esp_timer_handle_t t;
    esp_timer_create_args_t tc = {
        .callback = [](void*) { esp_restart(); },
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cfg_rst", .skip_unhandled_events = true,
    };
    esp_timer_create(&tc, &t);
    esp_timer_start_once(t, 800ULL * 1000);
    return ESP_OK;
}

static const httpd_uri_t kUriIndex = {"/",     HTTP_GET,  h_index, nullptr};
static const httpd_uri_t kUriScan  = {"/scan", HTTP_GET,  h_scan,  nullptr};
static const httpd_uri_t kUriSave  = {"/save", HTTP_POST, h_save,  nullptr};

// ===== WiFi 事件 =====
static void wifi_event_cb(void* arg, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 重试 3 次后放弃（需求5 超时提示"网络不好"）
        if (++s_sta_retry <= 3) {
            esp_wifi_connect();
        } else {
            s_mode = NetMode::StaFailed;
            ESP_LOGW(TAG, "sta connect failed after retries");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_mode = NetMode::StaConnected;
        s_sta_retry = 0;
        // DHCP 下发的 DNS 可能不可用（解析公网域名超时 202），用公网 DNS 兜底
        if (s_netif_sta) {
            esp_netif_dns_info_t dns_main = {}, dns_bak = {};
            dns_main.ip.type = dns_bak.ip.type = ESP_IPADDR_TYPE_V4;
            if (esp_netif_str_to_ip4("223.5.5.5", &dns_main.ip.u_addr.ip4) == ESP_OK &&
                esp_netif_str_to_ip4("8.8.8.8",   &dns_bak.ip.u_addr.ip4)  == ESP_OK) {
                esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN,   &dns_main);
                esp_netif_set_dns_info(s_netif_sta, ESP_NETIF_DNS_BACKUP, &dns_bak);
            }
        }
        ESP_LOGI(TAG, "sta got ip");
    }
}

// ===== 公共 API =====
static esp_err_t wifi_base_init() {
    if (s_wifi_inited) return ESP_OK;
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_cb, nullptr), TAG, "eh_wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_cb, nullptr), TAG, "eh_ip");
    s_wifi_inited = true;
    return ESP_OK;
}

// AP 超时回调：无人配置 → 自动关 AP（需求4）
static void ap_timeout_cb(void*) {
    ESP_LOGW(TAG, "AP provisioning timeout, stopping");
    net_mgr_stop();
}

esp_err_t net_mgr_init() {
    if (s_inited) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop");
    s_inited = true;
    return ESP_OK;
}

bool net_mgr_has_credentials() { return prefs_has_net(); }
NetMode net_mgr_mode() { return s_mode; }

esp_err_t net_mgr_start_ap() {
    ESP_RETURN_ON_ERROR(net_mgr_init(), TAG, "init");
    if (s_mode == NetMode::ApProvisioning) return ESP_OK;

    ESP_RETURN_ON_ERROR(wifi_base_init(), TAG, "wifi_base");
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode");
    wifi_config_t wc = {};
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf((char*)wc.ap.ssid, sizeof(wc.ap.ssid), "Pet-%02X%02X", mac[4], mac[5]);
    wc.ap.ssid_len = strlen((char*)wc.ap.ssid);
    wc.ap.channel = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    strcpy((char*)wc.ap.password, kApPassword);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "cfg");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");

    if (!s_httpd) {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.stack_size = 6144;
        if (httpd_start(&s_httpd, &hc) == ESP_OK) {
            httpd_register_uri_handler(s_httpd, &kUriIndex);
            httpd_register_uri_handler(s_httpd, &kUriScan);
            httpd_register_uri_handler(s_httpd, &kUriSave);
        } else {
            s_httpd = nullptr;
            ESP_LOGE(TAG, "httpd start failed");
        }
    }
    // 5 分钟超时自动关闭（需求4）
    if (!s_ap_timeout) {
        esp_timer_create_args_t tc = {
            .callback = ap_timeout_cb,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "ap_to", .skip_unhandled_events = true,
        };
        esp_timer_create(&tc, &s_ap_timeout);
    }
    esp_timer_stop(s_ap_timeout);   // 未启动时报错，忽略
    esp_timer_start_once(s_ap_timeout, (uint64_t)kApTimeoutSec * 1000000ULL);

    s_mode = NetMode::ApProvisioning;
    ESP_LOGI(TAG, "AP '%s' up (pwd %s), http://192.168.4.1",
             (char*)wc.ap.ssid, kApPassword);
    return ESP_OK;
}

esp_err_t net_mgr_connect_sta() {
    ESP_RETURN_ON_ERROR(net_mgr_init(), TAG, "init");
    NetConfig c;
    if (!prefs_get_net(&c)) return ESP_ERR_INVALID_STATE;

    // 已在配网模式先关掉（AP/STA 复用同一 wifi 句柄，切模式即可）
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = nullptr; }
    if (s_ap_timeout) esp_timer_stop(s_ap_timeout);

    ESP_RETURN_ON_ERROR(wifi_base_init(), TAG, "wifi_base");
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, c.wifi_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, c.wifi_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   // 兼容开放热点
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "cfg");
    s_sta_retry = 0;
    s_mode = NetMode::StaConnecting;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "sta connecting to '%s'...", c.wifi_ssid);
    return ESP_OK;
}

void net_mgr_stop() {
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = nullptr; }
    if (s_ap_timeout) esp_timer_stop(s_ap_timeout);
    if (s_wifi_inited) {
        esp_wifi_stop();
        s_mode = NetMode::Off;
        ESP_LOGI(TAG, "wifi stopped");
    }
}

}  // namespace boxpet::bsp
