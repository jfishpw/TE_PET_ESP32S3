// ui/ui_game_plane.cpp — 飞机打害虫游戏
//   渲染：lv_canvas 240×120（顶栏 24 + 底栏 24）
//   物理：esp_timer 30Hz tick；飞机按住左右键 3 px/帧
//   子弹：自动 300ms/发（=每 9 帧一发 @ 30Hz）
//   害虫：3 种（1/3/5 分；12/20/28 尺寸，速度和分值随尺寸递减），每 0.5~1.2s 刷一只
//   生命：3 颗心，飞机撞中害虫 -1 心（1.2s 无敌闪烁），0 心立即结束
//   难度：每 10 秒全局速度 ×1.2（上限 ×2.0）
//   时长：60 秒，到点结算（存活则通关）；
//   退出：中键长按 >1s
#include "ui_game_plane.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "bsp/power_mgr.h"
#include "game/coins.h"
#include "lvgl_sprite.h"   // 复用 lv_canvas API
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "plane";

namespace boxpet::ui {

namespace {

constexpr int kW = 240, kH = 120;        // 游戏画布（不含顶/底栏24，留中部）
// 注意：lvgl v9 在没有 PSRAM 的情况下 lv_malloc(240×216×2=100KB) 会导致 OOM。
// 本项目 SPIRAM_MALLOC_ALWAYSINTERNAL=4096 几乎所有 malloc 都走内部 RAM，
// 故画布尺寸限制为 240×120（≈57KB），留出顶部 24 + 底部 96 给顶栏 + 底栏 + 余量。
constexpr int kPlaneW = 16, kPlaneH = 12; // 飞机大小
constexpr int kPlaneY = kH - 20;          // 飞机固定底部 y
constexpr int kPlaneMinX = 4, kPlaneMaxX = kW - kPlaneW - 4;
constexpr int kPlaneSpeed = 3;            // 每帧移动像素
constexpr int kBulletW = 2, kBulletH = 4;
constexpr int kBulletSpeed = 4;        // 每帧向上像素（kH 较小，减速避免飞太快出屏）
constexpr int kBulletPeriodMs = 300;     // 自动发射周期
constexpr int kEnemyMax = 12;            // 同时存在数量上限
constexpr int kGameSec = 60;             // 时长
constexpr int kMaxLives = 3;             // 生命数（3 颗心）
constexpr int kInvulnMs = 1200;          // 受伤后无敌时间（避免连撞秒死）

// 害虫 3 种：分数 / 速度 / 大小 / 颜色
// 尺寸按用户要求"扩大一倍"：小 6→12，中 10→20，大 14→28
enum class EnemyKind : uint8_t { Small=0, Mid=1, Big=2 };
struct Enemy {
    int   x, y, w, h;
    int   vy;          // 像素/帧
    int   score;
    lv_color_t color;  // lv_color_t 类型（不是 uint16_t）
    bool  alive;
};

// 子弹
struct Bullet {
    int x, y;
    bool alive;
};

// 飞机图像（三角 + 矩形机身；像素坐标：18×12）
static void draw_plane(lv_obj_t* canvas, int cx, int cy, lv_color_t c) {
    // 简化：三角（5 列）+ 方形机身
    // 行 0 (top)：机尖
    for (int dx = -1; dx <= 1; ++dx)
        lv_canvas_set_px(canvas, cx + dx, cy - 6, c, LV_OPA_COVER);
    // 行 1：机翼
    for (int dx = -5; dx <= 5; ++dx)
        lv_canvas_set_px(canvas, cx + dx, cy - 5, c, LV_OPA_COVER);
    for (int dx = -7; dx <= 7; ++dx)
        lv_canvas_set_px(canvas, cx + dx, cy - 4, c, LV_OPA_COVER);
    // 行 2-3：机身
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -5; dx <= 5; ++dx)
            lv_canvas_set_px(canvas, cx + dx, cy + dy, c, LV_OPA_COVER);
    // 行 4：机尾
    for (int dx = -3; dx <= 3; ++dx)
        lv_canvas_set_px(canvas, cx + dx, cy + 4, c, LV_OPA_COVER);
}

static void fill_rect(lv_obj_t* canvas, int x, int y, int w, int h, lv_color_t c) {
    // 边界裁剪：lv_canvas_set_px 传入负坐标或超出画布坐标可能崩溃或越界。
    // 害虫初始 y=-h（屏幕外）时也可能进入此函数。
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > kW) x1 = kW;
    if (y1 > kH) y1 = kH;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            lv_canvas_set_px(canvas, xx, yy, c, LV_OPA_COVER);
}

// 生命状态
struct PlaneUi {
    lv_obj_t* root      = nullptr;
    lv_obj_t* canvas    = nullptr;
    lv_color_t* buf     = nullptr;  // 240×120 缓冲
    lv_obj_t* top_label = nullptr;  // 剩余时间 / 命中数
    esp_timer_handle_t tick_timer = nullptr;

    int   plane_x = kW / 2;
    int   plane_y = kPlaneY;
    int   plane_speed_boost = 0;  // 长按是否持续加速（可后续做长按加速）

    Bullet  bullets[16];
    Enemy   enemies[kEnemyMax];

    int  elapsed_ms = 0;
    int  last_bullet_ms = 0;
    int  last_spawn_ms = 0;
    int  hits = 0;
    int  lives = kMaxLives;          // 剩余生命（3 颗心）
    int  last_damage_ms = -100000;   // 上次受伤时刻（0=从未），用于无敌闪烁
    bool game_over = false;
    bool time_up = false;  // 60s 自然结束（vs 玩家中途退出）
    int  last_hit_flash_ms = 0;  // 命中（击杀/受伤）后短暂高亮提示

    bool want_leave = false;
};
static PlaneUi s;

// 心形 5×5 点阵（画布 HUD 用）
static void draw_heart(lv_obj_t* canvas, int x, int y, lv_color_t c) {
    static const int8_t kPts[][2] = {
        {1,0},{3,0},
        {0,1},{1,1},{2,1},{3,1},{4,1},
        {0,2},{1,2},{2,2},{3,2},{4,2},
        {1,3},{2,3},{3,3},
        {2,4},
    };
    for (size_t i = 0; i < sizeof(kPts) / sizeof(kPts[0]); ++i)
        lv_canvas_set_px(canvas, x + kPts[i][0], y + kPts[i][1], c, LV_OPA_COVER);
}

static void spawn_enemy() {
    for (int i = 0; i < kEnemyMax; ++i) {
        if (s.enemies[i].alive) continue;
        // 随机种类
        int r = esp_random() % 10;
        EnemyKind k;
        if (r < 6) k = EnemyKind::Small;       // 60%
        else if (r < 9) k = EnemyKind::Mid;    // 30%
        else k = EnemyKind::Big;               // 10%
        Enemy& e = s.enemies[i];
        switch (k) {
            case EnemyKind::Small:
                e.w = 12; e.h = 12; e.vy = 2;
                e.score = 1; e.color = lv_color_hex(0xFFD700);  // 金
                break;
            case EnemyKind::Mid:
                e.w = 20; e.h = 20; e.vy = 1;
                e.score = 3; e.color = lv_color_hex(0xEC5F8E);  // 深粉
                break;
            case EnemyKind::Big:
                e.w = 28; e.h = 28; e.vy = 1;
                e.score = 5; e.color = lv_color_hex(0xC00000);  // 红
                break;
        }
        e.x = (int)(esp_random() % (kW - e.w));
        e.y = -e.h;
        e.alive = true;
        return;
    }
}

// 子弹-害虫碰撞：命中返 true，害虫死亡，子弹消失
static bool check_bullet_hit(int bx, int by, int bw, int bh) {
    for (int i = 0; i < kEnemyMax; ++i) {
        Enemy& e = s.enemies[i];
        if (!e.alive) continue;
        if (bx + bw < e.x || bx > e.x + e.w) continue;
        if (by + bh < e.y || by > e.y + e.h) continue;
        e.alive = false;
        s.hits += e.score;
        s.last_hit_flash_ms = s.elapsed_ms;
        bsp::audio_play(bsp::Sound::Correct);
        return true;
    }
    return false;
}

static void render() {
    if (!s.canvas || !s.buf) return;
    // 清空背景（天空色）
    lv_canvas_fill_bg(s.canvas, lv_color_hex(0x000814), LV_OPA_COVER);
    // 生命值 HUD（画布左上角；红=存活，灰=已失去）
    for (int i = 0; i < kMaxLives; ++i) {
        bool on = i < s.lives;
        draw_heart(s.canvas, 6 + i * 10, 6,
                   on ? lv_color_hex(0xFF2D55) : lv_color_hex(0x3A3A3A));
    }
    // 害虫
    for (int i = 0; i < kEnemyMax; ++i) {
        Enemy& e = s.enemies[i];
        if (!e.alive) continue;
        fill_rect(s.canvas, e.x, e.y, e.w, e.h, lv_color_hex(0x8B0000));
        // 害虫"眼睛"（白色高亮）
        fill_rect(s.canvas, e.x + e.w/3, e.y + e.h/3, e.w/3, e.h/4,
                  lv_color_hex(0xFFFFFF));
    }
    // 子弹
    for (int i = 0; i < 16; ++i) {
        Bullet& b = s.bullets[i];
        if (!b.alive) continue;
        fill_rect(s.canvas, b.x, b.y, kBulletW, kBulletH, lv_color_hex(0xFFFF00));
    }
    // 飞机（击杀后短暂绿色高亮；受伤无敌期红白闪烁）
    bool hit_flash = (s.elapsed_ms - s.last_hit_flash_ms) < 80;
    bool invuln = (s.elapsed_ms - s.last_damage_ms) < kInvulnMs;
    bool blink_hide = invuln && ((s.elapsed_ms / 110) % 2) == 0;
    if (!blink_hide) {
        lv_color_t pc = hit_flash ? lv_color_hex(0x00FF00)
                       : invuln    ? lv_color_hex(0xFF4D4D)
                                   : lv_color_hex(0x00BFFF);
        draw_plane(s.canvas, s.plane_x + kPlaneW/2, s.plane_y, pc);
    }
    // 顶栏文字：剩余时间 + 命中
    if (s.top_label) {
        char buf[32];
        int remain = (kGameSec * 1000 - s.elapsed_ms) / 1000;
        if (remain < 0) remain = 0;
        snprintf(buf, sizeof(buf), "时间%d 命中%d", remain, s.hits);
        lv_label_set_text(s.top_label, buf);
    }
    lv_obj_invalidate(s.canvas);
}

static void physics_tick() {
    if (s.game_over) return;
    s.elapsed_ms += 33;  // 30Hz
    // 游戏进行中保持亮屏：每帧视为一次用户输入，重置 10s 熄屏计时。
    // 否则熄屏 → Light Sleep → 唤醒后死机（画布/音频/定时器在睡眠路径
    // 上的组合问题），且对局中黑屏本身也是错误体验。退出后 tick 停止，
    // 正常超时熄屏恢复。
    bsp::power_mgr_on_user_input();
    // 1. 飞机移动（按住左/右键持续移动）
    if (bsp::buttons_is_held(bsp::KeyId::Left)) {
        s.plane_x -= kPlaneSpeed;
    }
    if (bsp::buttons_is_held(bsp::KeyId::Right)) {
        s.plane_x += kPlaneSpeed;
    }
    if (s.plane_x < kPlaneMinX) s.plane_x = kPlaneMinX;
    if (s.plane_x > kPlaneMaxX) s.plane_x = kPlaneMaxX;

    // 2. 子弹自动发射
    if (s.elapsed_ms - s.last_bullet_ms >= kBulletPeriodMs) {
        s.last_bullet_ms = s.elapsed_ms;
        for (int i = 0; i < 16; ++i) {
            if (!s.bullets[i].alive) {
                s.bullets[i].x = s.plane_x + kPlaneW/2 - 1;
                s.bullets[i].y = s.plane_y - 4;
                s.bullets[i].alive = true;
                bsp::audio_play(bsp::Sound::Shoot);  // 射击 pew
                break;
            }
        }
    }
    // 3. 子弹飞行 + 碰撞
    for (int i = 0; i < 16; ++i) {
        Bullet& b = s.bullets[i];
        if (!b.alive) continue;
        b.y -= kBulletSpeed;
        if (b.y < -kBulletH) { b.alive = false; continue; }
        if (check_bullet_hit(b.x, b.y, kBulletW, kBulletH)) {
            b.alive = false;
        }
    }

    // 4. 害虫生成（按难度：每 800ms 减少 30ms，下限 200ms）
    int spawn_period = 800 - (s.elapsed_ms / 10000) * 30;
    if (spawn_period < 200) spawn_period = 200;
    if (s.elapsed_ms - s.last_spawn_ms >= spawn_period) {
        s.last_spawn_ms = s.elapsed_ms;
        spawn_enemy();
    }
    // 5. 害虫下移（速度随难度 ×1.2/10s，上限 ×2.0）
    float difficulty = 1.0f + 0.2f * (s.elapsed_ms / 10000);
    if (difficulty > 2.0f) difficulty = 2.0f;
    for (int i = 0; i < kEnemyMax; ++i) {
        Enemy& e = s.enemies[i];
        if (!e.alive) continue;
        e.y += (int)(e.vy * difficulty);
        if (e.y > kH) e.alive = false;  // 飞过底部移除
    }

    // 5.5 飞机-害虫碰撞：撞中 -1 心（1.2s 无敌），0 心立即结束
    bool invuln = (s.elapsed_ms - s.last_damage_ms) < kInvulnMs;
    if (!invuln) {
        // 飞机受击盒：x∈[plane_x, plane_x+kPlaneW]，y∈[kPlaneY-6, kPlaneY+4]
        int hx0 = s.plane_x, hx1 = s.plane_x + kPlaneW;
        int hy0 = s.plane_y - 6, hy1 = s.plane_y + 5;
        for (int i = 0; i < kEnemyMax; ++i) {
            Enemy& e = s.enemies[i];
            if (!e.alive) continue;
            if (e.x + e.w < hx0 || e.x > hx1) continue;
            if (e.y + e.h < hy0 || e.y > hy1) continue;
            e.alive = false;                       // 撞上的害虫消失
            s.lives--;
            s.last_damage_ms = s.elapsed_ms;
            s.last_hit_flash_ms = s.elapsed_ms;    // 触发受伤闪烁
            bsp::audio_play(bsp::Sound::Reject);   // 受伤音
            if (s.lives <= 0) {
                s.game_over = true;
                s.time_up   = false;               // 被撞死不算通关
                s.want_leave = true;
                bsp::audio_play(bsp::Sound::Lose); // 游戏结束音
            }
            break;                                 // 每帧只受一次伤
        }
    }

    // 6. 时长判定（活着撑到 60s 通关）
    if (s.elapsed_ms >= kGameSec * 1000) {
        s.game_over = true;
        s.time_up   = true;
        s.want_leave = true;
        bsp::audio_play(bsp::Sound::Win);          // 通关音
    }
}

static void tick_timer_cb(void* /*arg*/) {
    physics_tick();
    if (!lvgl_port_lock(50)) return;
    render();
    lvgl_port_unlock();
}

static void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    // 短按左/右键：单次 3px 推进（与持续按住并存，按住是更平滑）
    if (evt == bsp::KeyEvent::ShortPress && !s.game_over) {
        if (id == bsp::KeyId::Left) {
            s.plane_x -= 6;  // 短按单次更大步长
        } else if (id == bsp::KeyId::Right) {
            s.plane_x += 6;
        }
        if (s.plane_x < kPlaneMinX) s.plane_x = kPlaneMinX;
        if (s.plane_x > kPlaneMaxX) s.plane_x = kPlaneMaxX;
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        // 长按中键 >1s 退出
        s.game_over = true;
        s.time_up = false;  // 玩家主动退出，不算通关
        s.want_leave = true;
    }
}

static lv_obj_t* build_ui() {
    // Root
    s.root = lv_obj_create(nullptr);
    lv_obj_set_size(s.root, 240, 240);
    lv_obj_set_style_bg_color(s.root, lv_color_hex(0x000814), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏
    lv_obj_t* top = lv_obj_create(s.root);
    lv_obj_set_size(top, 240, 24);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    // 关键：清默认 padding——LVGL v9 默认主题会给 lv_obj 容器加内边距，
    // 否则 label 内容区被下推、行高 19 的字底超出 24px 条被裁剪
    // （实测表现：时间/命中文本只有顶部一小条可见）。
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    s.top_label = lv_label_create(top);
    lv_obj_set_style_text_color(s.top_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s.top_label, ui_font_16, 0);
    lv_obj_align(s.top_label, LV_ALIGN_LEFT_MID, 4, 0);

    // 画布（240×120，y=60；上下各留 60 像素给顶栏/底栏）
    // 优先用 PSRAM：100KB 画布超内部 RAM，OOM 会导致死机
    s.buf = (lv_color_t*)heap_caps_malloc(kW * kH * sizeof(lv_color_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.buf) {
        // PSRAM 不可用，回退 internal
        s.buf = (lv_color_t*)lv_malloc(kW * kH * sizeof(lv_color_t));
    }
    if (!s.buf) {
        ESP_LOGE(TAG, "plane canvas buf alloc failed (%d bytes)", kW * kH * 2);
        return s.root;  // 创建失败但仍返回（tick 不启动，画面黑屏）
    }
    s.canvas = lv_canvas_create(s.root);
    lv_canvas_set_buffer(s.canvas, s.buf, kW, kH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s.canvas, 0, 60);
    lv_obj_set_style_bg_opa(s.canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s.canvas, LV_OBJ_FLAG_SCROLLABLE);

    // 底栏
    lv_obj_t* bot = lv_obj_create(s.root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hint = lv_label_create(bot);
    lv_label_set_text(hint, "左右移动 长按退");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(hint, ui_font_16, 0);
    lv_obj_center(hint);

    return s.root;
}

}  // namespace

lv_obj_t* ui_plane_create() {
    memset(&s, 0, sizeof(s));
    s.plane_x = kW / 2;
    s.plane_y = kPlaneY;
    s.elapsed_ms = 0;
    s.lives = kMaxLives;
    s.last_damage_ms = -100000;
    s.last_bullet_ms = -kBulletPeriodMs;  // 立即可发射
    s.last_spawn_ms  = -500;               // 0.5s 后第一只害虫

    lv_obj_t* root = build_ui();
    // 启动 30Hz tick
    esp_timer_create_args_t cfg = {
        .callback = tick_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "plane_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &s.tick_timer);
    esp_timer_start_periodic(s.tick_timer, 33000ULL);  // ~30Hz
    bsp::buttons_set_callback(on_key);
    return root;
}

bool ui_plane_wants_to_leave() { return s.want_leave; }
void ui_plane_clear_leave_flag() { s.want_leave = false; }
int  ui_plane_last_hits() { return s.hits; }
bool ui_plane_last_time_up() { return s.time_up; }

void ui_plane_close() {
    if (s.tick_timer) {
        esp_timer_stop(s.tick_timer);
        esp_timer_delete(s.tick_timer);
        s.tick_timer = nullptr;
    }
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);
        lvgl_port_unlock();
    }
    s.root = nullptr;
    s.canvas = nullptr;
    s.buf = nullptr;
}

}  // namespace boxpet::ui