// ui/ui_chat.cpp — 小智云端语音对话面板（路径B，自研轻量客户端）
// 流程：主界面点"聊"→面板→连WiFi→连接小智云(WS)→中键说话→按流式上传
//   →服务器 ASR/LLM/TTS →文本显示+音频播放→再按中键结束说话→长按退出
// 音频：上传=采集流(16k PCM帧)；接收=服务器PCM→直接播放（扬声器）
#include "ui_chat.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "bsp/net_mgr.h"
#include "bsp/prefs.h"
#include "bsp/xz_client.h"
#include "bsp/power_mgr.h"
#include "anim.h"
#include "lvgl_sprite.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>

namespace boxpet::ui {

namespace {

using game::PetCore;

// 会话状态（映射 xz state 回调）
enum class Phase : uint8_t {
    Enter,        // 进入：检查凭据 → WiFi 连接
    Connecting,   // STA 连接中
    Auth,         // 无 token：OTA 注册中（自动获取）
    AwaitBind,    // 待绑定：显示激活码，等控制台绑定并轮询
    XzConnecting, // WebSocket/hello 进行中
    Idle,         // 就绪：按中键说话
    Listening,    // 正在说话（采集中）
    Replying,     // 服务器回话中（文本+音频）
    Err,          // 错误提示（显示后回 Idle/可退出）
};

struct ChatUi {
    lv_obj_t* panel      = nullptr;
    lv_obj_t* canvas     = nullptr;   // 96×96
    lv_color_t* cbuf     = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* reply_lbl  = nullptr;
    lv_obj_t* hint_lbl   = nullptr;
    Phase     phase      = Phase::Enter;
    esp_timer_handle_t tick = nullptr;
    int64_t   anim_phase = 0;
    int64_t   reply_until_ms = 0;
    char      disp_text[160] = {0};   // 最近一次服务器文本
    PetCore*  pet        = nullptr;
    bool      stt_error  = false;
    // 注册/激活流程
    bool      auth_pending = false;   // 本地尚无 token，需走 OTA 注册
    bool      auth_busy    = false;   // OTA 请求进行中
    bool      audio_ready  = false;   // 回调/音频流已就绪
    bool      code_shown   = false;   // 激活码是否已上屏
    int64_t   auth_last_ms = 0;       // 上次 OTA 检查时间
    int64_t   last_retry_ms = 0;      // Err 状态重连计时起点
    Phase     prev_phase = Phase::Enter;   // 上一拍相位（检测切到 Idle/Listening）
    volatile int64_t last_sound_ms = 0;    // 最近捕捉到人声的时刻（VAD 自动结束）
    volatile int  noise_floor = 0;         // 自适应噪声底（VAD 阈值随环境漂移）
    volatile bool voice_on    = false;     // 当前帧处于"有声音"状态（滞回）
    volatile int  hot         = 0;         // 语音命中帧计数（滤单帧底噪尖峰）
    volatile int64_t start_ms = 0;         // 本次拾音开始时刻（初始噪声校准用）
    bool      token_bad  = false;     // 连续失败≥2 次才置位 → 下次重注册自愈
    int       fail_count = 0;         // 连续连接失败计数（防单次抖动误触发重注册）
    volatile int auth_result = 0;     // OTA 结果延迟处理：0=无 1=已绑定
                                      //（auth 任务上下文禁止触碰 LVGL，
                                      //  置标志后由持锁的 tick_cb 消费）
};
static ChatUi s;

// ===== 服务器回调（xyz 任务上下文：文本直接存缓冲，屏显由本 tick 刷新）=====
// 纯语音对话模式：不显示服务器文本（TTS 回复直接播音频）。
static void xz_on_text(const char* t) {
    (void)t;   // 屏显文本已按需求关闭，仅保留语音通道
}
static void xz_on_pcm(const int16_t* pcm, size_t n) {
    bsp::audio_stream_pcm(pcm, n);
}
static void xz_on_state(int state) {
    const Phase old = s.phase;
    switch (state) {
        case 0: s.phase = Phase::Err; s.stt_error = true; break;   // 断开/连接失败
        case 1: s.phase = Phase::Idle;
                s.fail_count = 0; s.token_bad = false; break;      // 连上即视为 token 有效
        case 2: s.phase = Phase::Replying; break;
        case 3: s.phase = Phase::Idle; break;
    }
    // 服务器主动结束会话（state=2 开始回复 / state=3 回复完毕）把本机相位带离
    // Listening：但录音流可能仍开着 → mic 继续采集并无限上行（实测不说话的
    // "up enc f=" 仍在涨）。此时须停掉录音流（等同"中键停止"的收尾），否则
    // 空话刷屏、占上行带宽。state=1/0 属连接层，不在此收尾。此函数运行于
    // xz 任务、不涉 LVGL 操作，audio_record_stream_stop() 线程安全可直接调。
    if (old == Phase::Listening && (state == 2 || state == 3)) {
        bsp::audio_record_stream_stop();
        s.last_sound_ms = 0;
        s.hot = 0;
    }
}

// 采集帧回调（mic 任务）：上传给服务器 + 自适应 VAD（更新人声时刻）
static void on_mic_frame(const int16_t* pcm, size_t n) {
    bsp::xz_send_audio(pcm, n);
    if (n == 0) return;
    const int64_t now = esp_timer_get_time() / 1000;
    // 帧平均幅值（比单峰值对噪声免疫更强）
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) { int64_t a = pcm[i]; if (a < 0) a = -a; sum += a; }
    int avg = (int)(sum / n);

    // 初始 ~300ms：把噪声底校准到当前环境电平（取最小能量，若用户一按就说话
    // 也会取到话音间隙的安静帧）。修复根因：旧版常从 0 起步、噪声底被钉死在
    // 120 → 阈值过低，把环境底噪一路当人声 → last_sound_ms 不停刷新 →
    // "听完一直不停"。
    if (now - s.start_ms < 300) {
        if (s.noise_floor == 0 || avg < s.noise_floor) s.noise_floor = avg;
        if (s.noise_floor < 80)  s.noise_floor = 80;
        if (s.noise_floor > 20000) s.noise_floor = 20000;
        return;   // 校准期内不判语音、不推进静默
    }

    // 自适应噪声底：仅在"当前帧非强语音"时向 avg 追踪。
    // 强语音 = avg ≥ 1.8×噪声底。由此：
    //  - 说话中：avg 远超噪声底 → 噪声底保持低位不涨（修复旧"噪声底被抬到
    //    万级→吞掉真语音→过早停"）；只有帧幅值掉回噪声附近才会慢速跟随。
    //  - 停话后：avg 回落到环境电平，不再 ≥1.8×噪声底 → 噪声底缓升至环境
    //    级，thr 随之抬高，2s 内判无人声 → 自动结束（修复"听完一直不停"）。
    if (avg < (int32_t)(s.noise_floor * 5 / 2)) {
        int32_t d = avg - s.noise_floor;
        int32_t step = (d > 0) ? (d >> 4) : (d >> 2);   // 升1/16缓、降1/4快
        s.noise_floor += step;
    }
    if (s.noise_floor < 80)     s.noise_floor = 80;
    if (s.noise_floor > 20000)  s.noise_floor = 20000;
    const int thr = (int32_t)(s.noise_floor * 2) + 120;   // 阈值=2×噪声底+底线

    // 语音判定：幅值 > 阈值且近窗≥2 帧命中（滤单帧底噪尖峰）
    if (avg > thr) {
        if (++s.hot >= 2) {
            if (!s.voice_on) s.voice_on = true;
            s.last_sound_ms = now;                       // 刷新最近人声时刻
        }
    } else {
        if (s.hot > 0) --s.hot;   // 退火：句内短回落不清零，长停顿才衰竭
    }
    // 滞回清除：明显回落（<半阈值）才判无声
    if (s.voice_on && avg < (thr >> 1)) s.voice_on = false;
}

static void render_pet_locked() {
    if (!s.canvas || !s.cbuf || !s.pet) return;
    const sprites::Sprite* f = nullptr;
    switch (s.phase) {
        case Phase::Listening:
            f = find_stage_sprite("happy", s.pet->state());   // 倾听
            break;
        case Phase::Replying:
            f = find_stage_sprite("eat", s.pet->state());     // 说话
            break;
        default:
            f = idle_frame_for(s.pet->state());
            break;
    }
    if (f) {
        lv_color_t bg = lv_color_hex(0x25315F);
        render_pet_sprite(s.canvas, f, bg);
        lv_obj_set_x(s.canvas, (232 - 96) / 2);
    }
}

static void set_status(const char* text) {
    if (s.status_lbl) lv_label_set_text(s.status_lbl, text);
}

static void show_text_locked(const char* t) {
    if (s.reply_lbl) {
        lv_label_set_text(s.reply_lbl, t ? t : "");
        lv_obj_clear_flag(s.reply_lbl, LV_OBJ_FLAG_HIDDEN);
        s.reply_until_ms = esp_timer_get_time() / 1000 + 8000;
    }
}

// 结束本段说话 → 转"回复中"。自动(VAD 静默)与手动(中键)共用同一路径，
// 保证两者对服务器行为完全一致（等同按中键停止），杜绝自动停漏发录音/漏发
// listen.stop。调用方须已持 LVGL 锁。
static void stop_listening_locked() {
    s.last_sound_ms = 0;
    bsp::xz_talk_end();               // 通知服务器结束拾音
    bsp::audio_record_stream_stop();  // 停采、恢复喇叭（内部会刷新静音）
    set_status("回复中…");
    s.phase = Phase::Replying;
}

// OTA 注册回调（auth 任务上下文，禁止调用任何 LVGL/持锁 UI 函数）：
// 0=失败 1=已绑定 2=待绑定
static void xz_auth_cb(int status) {
    s.auth_busy = false;
    s.auth_last_ms = esp_timer_get_time() / 1000;
    switch (status) {
        case 0:
            s.phase = Phase::Err;
            snprintf(s.disp_text, sizeof(s.disp_text), "注册失败（检查网络/小智地址）");
            s.stt_error = false;
            break;
        case 1:  // 已绑定，token 就绪 → 置标志，由 tick_cb 持锁连接
            s.auth_pending = false;
            s.auth_result  = 1;
            break;
        case 2:
            s.phase = Phase::AwaitBind;
            s.code_shown = false;   // 重新上屏
            break;
    }
}

}  // namespace（内部状态与工具，闭）

// 200ms 状态机（esp_timer 任务：持锁）
static void tick_cb(void*) {
    if (!s.panel) return;
    bsp::power_mgr_on_user_input();   // 防黑屏：面板期间不熄不睡
    if (!lvgl_port_lock(80)) return;
    int64_t now = esp_timer_get_time() / 1000;

    switch (s.phase) {
        case Phase::Enter: {
            boxpet::bsp::NetConfig c;
            // xz_url 已有默认兜底（prefs_get_net 自动填充官方地址），
            // 这里只需判 WiFi 凭据是否就绪
            if (!bsp::prefs_get_net(&c)) {
                set_status("未配置小智云（设置配网）");
                s.phase = Phase::Err;
                break;
            }
            char tok[160];
            bool has_token = bsp::prefs_get_xz_token(tok, sizeof(tok));
            // 旧设备兼容：若持久化了 token 但没有配对的 Client-Id（升级前的
            // 注册流程未稳定保存身份），二者必然不匹配会导致 hello 无人应答。
            // 此时强制重新注册一次，让注册与连接使用同一个 Client-Id。
            char cid[40];
            bool has_client = bsp::prefs_get_xz_client(cid, sizeof(cid));
            // 仅有 token && client 且非此前验证失败时才复用（跳过注册）；
            // 否则清栈触发重注册。token 只在"验证失败/失效"后自愈重取，日常直达。
            s.auth_pending = !has_token || !has_client || s.token_bad;
            if (s.auth_pending) bsp::prefs_set_xz_token("");
            s.token_bad = false;
            bsp::net_mgr_connect_sta();
            s.phase = Phase::Connecting;
            break;
        }
        case Phase::Connecting: {
            bsp::NetMode m = bsp::net_mgr_mode();
            if (m == bsp::NetMode::StaConnected) {
                if (!s.audio_ready) {   // 回调/音频流只就绪一次
                    bsp::xz_set_callbacks(xz_on_text, xz_on_pcm, xz_on_state);
                    bsp::audio_stream_begin();
                    s.audio_ready = true;
                }
                boxpet::bsp::NetConfig c;
                if (!bsp::prefs_get_net(&c)) { s.phase = Phase::Err; break; }
                if (s.auth_pending) {
                    // 无有效 token → 走 OTA 注册：让服务器按 (Device-Id,
                    // Client-Id) 刷新并返回合法 token；未绑定时给激活码。
                    set_status("设备注册…");
                    s.auth_busy = true;
                    bsp::xz_auth_begin(c.xz_url, xz_auth_cb);
                    s.phase = Phase::Auth;
                } else {
                    // 有持久化 token，直接复用连接，避免每次聊天重复注册
                    char tok[160];
                    if (bsp::prefs_get_xz_token(tok, sizeof(tok)) && tok[0]) {
                        set_status("连接小智云…");
                        bsp::xz_start(c.xz_url, tok);
                        s.phase = Phase::XzConnecting;
                    } else { s.phase = Phase::Err; }
                }
            } else if (m == bsp::NetMode::StaFailed) {
                set_status("网络不好");
                s.phase = Phase::Err;
            }
            break;
        }
        case Phase::Auth:
            // OTA 注册进行中；auth 任务只置标志，LVGL 操作在此（已持锁）执行
            if (s.auth_result == 1) {
                s.auth_result = 0;
                boxpet::bsp::NetConfig c;
                char tok[160];
                if (bsp::prefs_get_net(&c) && bsp::prefs_get_xz_token(tok, sizeof(tok))) {
                    set_status("连接小智云…");
                    bsp::xz_start(c.xz_url, tok);
                    s.phase = Phase::XzConnecting;
                } else {
                    s.phase = Phase::Err;
                }
            }
            break;
        case Phase::AwaitBind: {
            if (!s.code_shown) {
                char code[16];
                if (bsp::prefs_get_xz_code(code, sizeof(code)) && code[0]) {
                    set_status("去xiaozhi.me绑定后自动连");
                    char buf[40];
                    snprintf(buf, sizeof(buf), "绑定码: %s", code);
                    show_text_locked(buf);
                } else {
                    show_text_locked("绑定后重进本面板");
                }
                s.code_shown = true;
                s.auth_last_ms = now;   // 重启轮询计时
            } else if (!s.auth_busy && now >= s.auth_last_ms + 8000) {
                // 每 8s 轮询是否已绑定
                boxpet::bsp::NetConfig c;
                if (bsp::prefs_get_net(&c) && c.xz_url[0]) {
                    s.auth_busy = true;
                    bsp::xz_auth_begin(c.xz_url, xz_auth_cb);
                }
            }
            break;
        }
        case Phase::XzConnecting:
            // 由 xz_on_state 切走
            break;
        case Phase::Idle:
            // 切到空闲态：提示按中键说话（仅切换瞬间更新一次）
            if (s.prev_phase != Phase::Idle) set_status("请按中键说话");
            if (s.disp_text[0]) {   // 显示最近文本（未读）
                show_text_locked(s.disp_text);
                s.disp_text[0] = 0;
            }
            break;
        case Phase::Replying:
            // 进入回复态时显式亮出"回复中…"（服务器触发的结束不会再经过
            // stop_listening_locked 那条设文本路径，这里按转移瞬间补一次）。
            if (s.prev_phase != Phase::Replying) set_status("回复中…");
            if (s.disp_text[0]) {
                show_text_locked(s.disp_text);
                s.disp_text[0] = 0;
            }
            break;
        case Phase::Listening:
            // 无人声 ≥3s 自动结束说话（等同按中键停止）；手动结束仍由按键处理。
            if (s.last_sound_ms && now - s.last_sound_ms >= 3000)
                stop_listening_locked();
            break;
        case Phase::Err:
            // 清屏显文本后 disp_text 恒空：Err 不再依赖文本退出，改由超时自动
            // 重连（服务器从 OTA 注册到可用有延迟，直接再发起一次）。
            if (s.stt_error && s.last_retry_ms == 0) s.last_retry_ms = now;
            if (s.stt_error && now >= s.last_retry_ms + 4000) {
                s.last_retry_ms = 0;
                s.stt_error = false;
                // 用有效 token 重连；仅当"连续失败≥2 次"才判定 token 失效，
                // 下次 Enter 强制重注册自愈——避免单次网络抖动就频繁重注册。
                if (++s.fail_count >= 2) { s.token_bad = true; s.fail_count = 0; }
                s.phase = Phase::Enter;   // 重走：检查凭据→连WiFi→连小智云
            } else if (s.reply_until_ms && now >= s.reply_until_ms) {
                s.reply_until_ms = 0;
            }
            break;
    }
    s.prev_phase = s.phase;
    // 动画（每 400ms）
    if ((now / 200) != s.anim_phase) {
        s.anim_phase = now / 200;
        render_pet_locked();
    }
    lvgl_port_unlock();
}

void chat_panel_close() {
    if (!s.panel) return;
    if (s.tick) {
        esp_timer_stop(s.tick);
        esp_timer_delete(s.tick);
        s.tick = nullptr;
    }
    if (bsp::audio_recording()) bsp::audio_record_stop();
    bsp::audio_record_stream_stop();
    bsp::xz_stop();                  // 断开小智云（内部收尾）
    bsp::audio_stream_end();         // 恢复音效
    bsp::net_mgr_stop();
    if (s.panel && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.panel);
        lvgl_port_unlock();
    }
    s.panel = nullptr;
    s.canvas = nullptr;
    s.cbuf = nullptr;
    s.phase = Phase::Enter;
    s.disp_text[0] = 0;
    s.auth_pending = false;
    s.auth_busy = false;
    s.audio_ready = false;
    s.code_shown = false;
    s.auth_last_ms = 0;
    s.last_retry_ms = 0;
    s.stt_error = false;
}

bool chat_panel_visible() { return s.panel != nullptr; }

// ===== 面板按键（btn_scan：持锁）=====
bool chat_panel_key(bsp::KeyId id, bsp::KeyEvent evt) {
    if (!s.panel) return false;
    if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        chat_panel_close();
        return true;
    }
    if (evt != bsp::KeyEvent::ShortPress || id != bsp::KeyId::Mid) return false;
    if (!lvgl_port_lock(80)) return true;

    if (s.phase == Phase::Idle || s.phase == Phase::Replying) {
        // 开始说话：启动流式采集（帧回调→上传）；重置自适应 VAD 状态
        bsp::audio_record_stream_stop();
        if (bsp::audio_record_stream_start(on_mic_frame)) {
            bsp::xz_talk_begin();
            const int64_t t0 = esp_timer_get_time() / 1000;
            s.start_ms = t0;                       // VAD 初始噪声校准起点
            s.last_sound_ms = t0;                  // 从此刻起算 VAD 静默
            s.voice_on = false;
            s.hot = 0;
            s.noise_floor = 0;                     // 由前 ~300ms 帧校准到环境电平
            set_status("在听…");
            s.phase = Phase::Listening;
        }
    } else if (s.phase == Phase::Listening) {
        // 结束说话（与自动静默停止完全同一路径）
        stop_listening_locked();
    }
    lvgl_port_unlock();
    return true;
}

// ===== 面板构建 =====
void chat_panel_open(PetCore* pet) {
    if (s.panel) return;
    if (!pet) return;
    s.pet = pet;
    if (!lvgl_port_lock(200)) return;
    lv_obj_t* root = lv_scr_act();
    s.panel = lv_obj_create(root);
    lv_obj_set_size(s.panel, 232, 146);
    lv_obj_set_pos(s.panel, 4, 56);
    lv_obj_set_style_bg_color(s.panel, lv_color_hex(0x1A2333), 0);
    lv_obj_set_style_bg_opa(s.panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s.panel, lv_color_hex(0x8ED6F0), 0);
    lv_obj_set_style_border_width(s.panel, 2, 0);
    lv_obj_set_style_radius(s.panel, 8, 0);
    lv_obj_set_style_pad_all(s.panel, 0, 0);
    lv_obj_clear_flag(s.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s.panel);

    s.cbuf = (lv_color_t*)heap_caps_malloc(96 * 96 * sizeof(lv_color_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.cbuf) {
        s.canvas = lv_canvas_create(s.panel);
        lv_canvas_set_buffer(s.canvas, s.cbuf, 96, 96, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s.canvas, (232 - 96) / 2, 2);
        lv_obj_set_style_bg_opa(s.canvas, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(s.canvas, LV_OBJ_FLAG_SCROLLABLE);
        render_pet_locked();
    }
    s.status_lbl = lv_label_create(s.panel);
    lv_label_set_text(s.status_lbl, "连接中…");
    lv_obj_set_style_text_color(s.status_lbl, lv_color_hex(0xFFD966), 0);
    lv_obj_set_style_text_font(s.status_lbl, ui_font_16, 0);
    lv_obj_align(s.status_lbl, LV_ALIGN_TOP_MID, 0, 98);
    s.reply_lbl = lv_label_create(s.panel);
    lv_label_set_long_mode(s.reply_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s.reply_lbl, 220);
    lv_label_set_text(s.reply_lbl, "");
    lv_obj_set_style_text_color(s.reply_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s.reply_lbl, ui_font_16, 0);
    lv_obj_align(s.reply_lbl, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_add_flag(s.reply_lbl, LV_OBJ_FLAG_HIDDEN);
    s.hint_lbl = lv_label_create(s.panel);
    lv_label_set_text(s.hint_lbl, "中键说话/结束 长按退");
    lv_obj_set_style_text_color(s.hint_lbl, lv_color_hex(0x90A0B0), 0);
    lv_obj_set_style_text_font(s.hint_lbl, ui_font_16, 0);
    lv_obj_align(s.hint_lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lvgl_port_unlock();

    s.phase = Phase::Enter;
    s.anim_phase = 0;
    s.disp_text[0] = 0;
    esp_timer_create_args_t cfg = {
        .callback = tick_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "chat_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &s.tick);
    esp_timer_start_periodic(s.tick, 200000);
    bsp::audio_play(bsp::Sound::Beep);
}

}  // namespace boxpet::ui