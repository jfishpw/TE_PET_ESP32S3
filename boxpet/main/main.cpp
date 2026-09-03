// main.cpp — BoxPet 入口 + 场景调度器
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "board_config.h"

#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/wallclock.h"
#include "bsp/power.h"
#include "bsp/power_mgr.h"
#include "bsp/audio.h"
#include "bsp/prefs.h"
#include "bsp/net_mgr.h"
#include "ui/ui_main.h"
#include "ui/ui_game.h"
#include "ui/ui_status.h"
#include "ui/ui_settings.h"
#include "ui/ui_shop.h"
#include "ui/ui_game_plane.h"
#include "ui/ui_netcfg.h"
#include "ui/coin_widget.h"
#include "game/pet.h"
#include "game/pet_def.h"
#include "game/coins.h"
#include "game/storage.h"

static const char* TAG = "main";

using boxpet::bsp::board_load_screen;
using boxpet::bsp::buttons_init;
using boxpet::bsp::board_init;
using boxpet::bsp::power_init;
using boxpet::bsp::power_start_monitor;
using boxpet::bsp::audio_init;
using boxpet::ui::ui_main_create;
using boxpet::ui::ui_main_attach_key;
using boxpet::ui::ui_main_attach_pet;
using boxpet::ui::ui_main_start_tick;
using boxpet::ui::ui_main_consume_want_game;
using boxpet::ui::ui_main_consume_want_status;
using boxpet::ui::ui_main_consume_want_settings;
using boxpet::ui::ui_main_consume_want_shop;
using boxpet::ui::ui_main_consume_want_plane;
using boxpet::game::PetCore;

static PetCore g_pet;
static esp_timer_handle_t g_pet_tick = nullptr;

static void pet_tick_cb(void* /*arg*/) { g_pet.tick_real_second(); }

// 5 分钟周期存档（5*60*1000ms），带变更检测：状态无变化则跳过（省 Flash 磨损）
static esp_timer_handle_t g_save_timer = nullptr;
static void save_tick_cb(void* /*arg*/) {
    boxpet::game::storage_save_if_changed(g_pet.state());
}

// 深休眠恢复快路径（app_main 最早期调用，board/UI 初始化前；可能直接续睡不返回）。
// 流程：① 读 RTC 睡眠时长 + 原因 + 充电状态 → ② 逐秒补跳宠物并把墙钟拨到当前
// → ③ 决策：按键唤醒/到起床点/已充电 → 正常启动；仍深夜/仍低电量 → 续睡。
static void deep_sleep_resume_early() {
    int64_t elapsed = 0;
    uint8_t reason  = 0;
    bool    charging = false;
    if (!boxpet::bsp::power_mgr_deep_sleep_resume(&elapsed, &reason, &charging)) return;
    ESP_LOGI(TAG, "deep sleep resume: elapsed=%llds reason=%d charging=%d",
             (long long)elapsed, (int)reason, (int)charging);

    // 深睡期间不触发随机事件（夜间 + 补跳期统一关闭，醒来后恢复）
    g_pet.set_events_enabled(false);
    // 逐秒补跳：属性衰减/精力恢复/生病死亡判定在睡眠期间照常推进；同时逐秒
    // 推进墙钟，让"到点自然醒/到点入睡"等按真实时刻的判定在补跳中正确触发。
    for (int64_t i = 0; i < elapsed; ++i) {
        g_pet.tick_one_second();
        boxpet::bsp::wallclock_advance_by(1);
    }
    boxpet::game::storage_save_if_changed(g_pet.state());
    g_pet.set_events_enabled(true);

    // 用户按键唤醒（左键 EXT1）→ 先验证按键仍被按住再放行正常启动。
    // GPIO3 是 strapping 脚，深睡入睡/唤醒瞬间电平易毛刺（与右键 GPIO0 同类
    // 问题，实测表现"熄屏1秒后重启亮屏、循环"）——毛刺 <1ms，真人按住
    // ≥200ms：等 80ms 后再采样，松开即判定为毛刺假唤醒，直接续睡。
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        // 此时尚未 buttons_init：单独配一次左键输入上拉（左/右键低电平有效）
        gpio_config_t btn_cfg = {};
        btn_cfg.mode = GPIO_MODE_INPUT;
        btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        btn_cfg.pin_bit_mask = (1ULL << boxpet::BTN_LEFT_PIN);
        gpio_config(&btn_cfg);
        vTaskDelay(pdMS_TO_TICKS(80));
        if (gpio_get_level(boxpet::BTN_LEFT_PIN) == 0) {   // 低=仍按住：真人
            ESP_LOGI(TAG, "deep sleep woke by button -> normal boot");
            return;
        }
        ESP_LOGW(TAG, "EXT1 glitch (BTN_LEFT released) -> re-enter deep sleep");
        uint8_t cont = boxpet::bsp::power_mgr_deep_sleep_continue_reason();
        if (cont != boxpet::bsp::kDsReasonNone) {
            boxpet::game::storage_save_if_changed(g_pet.state());
            // no_btn_wake=true：掐断 EXT1 源头，防止同一毛刺反复假唤醒循环
            boxpet::bsp::power_mgr_reenter_deep_sleep(cont, true);
            // 不返回
        }
        return;   // 不满足续睡条件（如已插电）→ 正常启动兜底
    }
    // 定时自醒：补跳后重新评估——宠物仍睡（夜间未到精力满/白天精力未满/低电
    // 未充电）→ 续睡；已在补跳中自动醒（白天精力满）或已充电 → 正常启动。
    uint8_t cont = boxpet::bsp::power_mgr_deep_sleep_continue_reason();
    if (cont != boxpet::bsp::kDsReasonNone) {
        boxpet::game::storage_save_if_changed(g_pet.state());
        boxpet::bsp::power_mgr_reenter_deep_sleep(cont);
        // 不返回
    }
}

// 场景调度：主界面 / 游戏 / 状态页 / 设置 / 商店 / 飞机 / 配网
enum class Scene : uint8_t {
    Main, Game, Status, Settings, Shop, Plane, NetCfg, Death
};
static Scene g_scene = Scene::Main;

// 死亡画面：墓碑 + 长按中键 3s 孵化新蛋
static esp_timer_handle_t g_death_timer = nullptr;
static int64_t g_death_longpress_ms = 0;

static void on_main_long_press_mid() {
    // 长按中键 1.5s 进入设置
    g_scene = Scene::Settings;
    lvgl_port_lock(1000);
    lv_obj_t* s = boxpet::ui::ui_settings_create();
    boxpet::ui::ui_settings_set_pet(&g_pet);
    lvgl_port_unlock();
    board_load_screen(s);
}

extern "C" void app_main(void) {
    // 固件版本 banner：用本文件编译的 __DATE__/__TIME__（改动必重编译、时间
    // 必更新），可靠自证"当前运行的固件是否包含最新修复"。esp_app_desc 的
    // date/time 是首次 configure 写死的宏，增量构建不更新，不可用于此目的。
    ESP_LOGI(TAG, "==== boxpet v2-20260829 built %s %s ====", __DATE__, __TIME__);
    // 调试：打印复位原因（1=POWERON 3=PANIC 4=INT_WDT 5=TASK_WDT 7=BROWNOUT 8=...）
    ESP_LOGI(TAG, "reset reason=%d", (int)esp_reset_reason());
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    // ===== 深休眠恢复快路径所需的最小初始化（board/UI 之前）=====
    boxpet::bsp::prefs_init();
    boxpet::bsp::wallclock_init();
    ESP_ERROR_CHECK(boxpet::game::storage_init());
    // 需求2：注入真实时钟 + 加载作息窗口（真实模式睡眠判断用真实时间）
    g_pet.set_real_hour_provider([]() {
        int h, m, s;
        boxpet::bsp::wallclock_now(&h, &m, &s);
        return h;
    });
    {
        int h0 = 23, h1 = 6;
        boxpet::bsp::prefs_get_sleep_window(&h0, &h1);
        g_pet.set_sleep_window(h0, h1);
    }
    // 尝试读取存档
    {
        boxpet::game::PetState st;
        if (boxpet::game::storage_load(&st)) {
            g_pet.load_state(st);
            ESP_LOGI(TAG, "loaded save: stage=%d", (int)st.stage);
        } else {
            ESP_LOGW(TAG, "no valid save → start with Egg");
        }
    }
    // 深休眠恢复：从深睡唤醒 → 补跳宠物时间并决策（按键/到点/已充电 → 正常
    // 启动；仍深夜/仍低电 → 直接续睡，不返回）。
    deep_sleep_resume_early();

    // ===== 正常启动 =====
    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(buttons_init());
    // 音频失败不致命（无喇叭/驱动问题不应挡住游戏）
    esp_err_t audio_ret = boxpet::bsp::audio_init();
    if (audio_ret != ESP_OK) ESP_LOGW("main", "audio_init failed: %s", esp_err_to_name(audio_ret));
    boxpet::game::coins_init();
    ESP_ERROR_CHECK(boxpet::bsp::power_mgr_init(&g_pet));

    lvgl_port_lock(1000);
    lv_obj_t* main_scr = ui_main_create();
    ui_main_attach_pet(&g_pet);
    ui_main_attach_key(nullptr);
    lvgl_port_unlock();
    board_load_screen(main_scr);
    ui_main_start_tick(8, 0);

    // 异常复位诊断：非上电/深度睡眠复位时，屏显复位原因 6 秒。
    // （用于区分"设置页自动退出"等 bug 是否由设备重启引起）
    {
        const char* rst_name = nullptr;
        switch (esp_reset_reason()) {
            case ESP_RST_PANIC:      rst_name = "程序崩溃复位!"; break;
            case ESP_RST_INT_WDT:
            case ESP_RST_TASK_WDT:
            case ESP_RST_WDT:        rst_name = "看门狗复位!"; break;
            case ESP_RST_BROWNOUT:   rst_name = "欠压复位!"; break;
            case ESP_RST_USB:        rst_name = "USB复位!"; break;
            case ESP_RST_PWR_GLITCH: rst_name = "电源毛刺复位!"; break;
            default: break;   // POWERON / DEEPSLEEP / SW 等不提示
        }
        if (rst_name) {
            ESP_LOGW(TAG, "abnormal reset: reason=%d (%s)",
                     (int)esp_reset_reason(), rst_name);
            boxpet::ui::ui_main_show_toast(rst_name, 6000);
        }
    }

    // 1Hz pet tick
    esp_timer_create_args_t tcfg = {
        .callback = pet_tick_cb, .dispatch_method = ESP_TIMER_TASK,
        .name = "pet_tick", .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&tcfg, &g_pet_tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_pet_tick, 1000000ULL));

    // 5 分钟存档
    esp_timer_create_args_t scfg = {
        .callback = save_tick_cb, .dispatch_method = ESP_TIMER_TASK,
        .name = "save_tick", .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&scfg, &g_save_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(g_save_timer, 5ULL * 60 * 1000 * 1000));

    ESP_ERROR_CHECK(power_start_monitor());

    // CPU 调频 80~240MHz（DFS）。注意：不能开 PM 自动 Light Sleep——
    // 它与 power_mgr 睡眠任务的手动 esp_light_sleep_start 冲突：
    // PM 空闲自动入睡会隔离 GPIO（CONFIG_PM_SLP_DISABLE_GPIO），
    // 按键电平唤醒失效（短按唤不醒，只能靠长按碰运气），且 esp_timer
    // 补跳不走手动路径 → 睡眠期间时钟冻结。睡眠统一由 pm_sleep 任务负责。
    esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    esp_err_t pm_err = esp_pm_configure(&pm);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure skipped (err=0x%x) — check CONFIG_PM_ENABLE", pm_err);
    } else {
        ESP_LOGI(TAG, "DFS enabled 80~240MHz (auto light sleep OFF)");
    }

    ESP_LOGI(TAG, "BoxPet boot OK (M3)");

    // 主循环：轮询场景切换与死亡判定
    while (true) {
        // 死亡判定
        if (g_pet.state().stage == boxpet::game::Stage::Dead && g_scene != Scene::Death) {
            // 进入死亡场景：保存状态 + 简单显示墓碑
            g_scene = Scene::Death;
            boxpet::game::storage_save(g_pet.state());
            // 用主界面但把 pet canvas 替换成墓碑精灵（UI 内部已处理）
        }

        switch (g_scene) {
            case Scene::Main: {
                if (boxpet::ui::ui_main_consume_want_resurrect()) {
                    g_pet.reset_to_new_egg();
                    boxpet::game::storage_save(g_pet.state());
                }
                if (ui_main_consume_want_game()) {
                    g_scene = Scene::Game;
                    bool is_edu = boxpet::ui::ui_main_pending_is_edu();
                    uint8_t kind = boxpet::ui::ui_main_pending_kind();
                    lvgl_port_lock(1000);
                    boxpet::ui::ui_game_configure(is_edu, kind);
                    lv_obj_t* s = boxpet::ui::ui_game_create();
                    boxpet::ui::ui_game_set_pet(&g_pet);
                    lvgl_port_unlock();
                    board_load_screen(s);
                } else if (ui_main_consume_want_status()) {
                    g_scene = Scene::Status;
                    lvgl_port_lock(1000);
                    lv_obj_t* s = boxpet::ui::ui_status_create();
                    boxpet::ui::ui_status_set_pet(&g_pet);
                    lvgl_port_unlock();
                    board_load_screen(s);
                } else if (ui_main_consume_want_settings()) {
                    g_scene = Scene::Settings;
                    lvgl_port_lock(1000);
                    lv_obj_t* s = boxpet::ui::ui_settings_create();
                    boxpet::ui::ui_settings_set_pet(&g_pet);
                    lvgl_port_unlock();
                    board_load_screen(s);
                } else if (ui_main_consume_want_shop()) {
                    g_scene = Scene::Shop;
                    lvgl_port_lock(1000);
                    lv_obj_t* s = boxpet::ui::ui_shop_create();
                    boxpet::ui::ui_shop_set_pet(&g_pet);
                    lvgl_port_unlock();
                    board_load_screen(s);
                } else if (ui_main_consume_want_plane()) {
                    g_scene = Scene::Plane;
                    lvgl_port_lock(1000);
                    lv_obj_t* s = boxpet::ui::ui_plane_create();
                    lvgl_port_unlock();
                    board_load_screen(s);
                }
                break;
            }
            // 注意退出顺序：必须先 board_load_screen(main_scr) 再 ui_*_close()。
            // close 内部 lv_obj_delete_async 删除的是当前活动屏，LVGL 会把
            // act_scr 置 NULL（lv_obj_tree.c: "the active screen was deleted"）；
            // 若先删后加载，存在 act_scr==NULL 空窗——taskLVGL 刷新定时器恰在
            // 此窗口执行 lv_obj_update_layout(NULL) → 读 NULL+0x2a 崩溃重启
            // （coredump 实证，表现为"设置页神秘退出/时钟回到 00:00"）。
            // 先加载主屏后删除旧屏，act_scr 全程非 NULL，竞态消除。
            case Scene::Game: {
                if (boxpet::ui::ui_game_wants_to_leave()) {
                    boxpet::ui::ui_game_clear_leave_flag();
                    board_load_screen(main_scr);
                    boxpet::ui::ui_game_close();
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);  // 恢复主界面按键回调
                }
                break;
            }
            case Scene::Status: {
                if (boxpet::ui::ui_status_wants_leave()) {
                    boxpet::ui::ui_status_clear_leave_flag();
                    board_load_screen(main_scr);
                    boxpet::ui::ui_status_close();
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);  // 恢复主界面按键回调
                }
                break;
            }
            case Scene::Settings: {
                if (boxpet::ui::ui_settings_wants_netcfg()) {
                    boxpet::ui::ui_settings_clear_netcfg_flag();
                    g_scene = Scene::NetCfg;
                    lvgl_port_lock(1000);
                    lv_obj_t* s = boxpet::ui::ui_netcfg_create();
                    lvgl_port_unlock();
                    board_load_screen(s);
                    break;
                }
                if (boxpet::ui::ui_settings_wants_reset()) {
                    boxpet::ui::ui_settings_clear_reset_flag();
                    boxpet::game::storage_erase();
                    g_pet.reset_to_new_egg();
                }
                if (boxpet::ui::ui_settings_wants_leave()) {
                    boxpet::ui::ui_settings_clear_leave_flag();
                    board_load_screen(main_scr);
                    boxpet::ui::ui_settings_close();
                    boxpet::game::storage_save(g_pet.state());
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);  // 恢复主界面按键回调
                }
                break;
            }
            case Scene::Shop: {
                if (boxpet::ui::ui_shop_wants_to_leave()) {
                    boxpet::ui::ui_shop_clear_leave_flag();
                    board_load_screen(main_scr);
                    boxpet::ui::ui_shop_close();
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);
                }
                break;
            }
            case Scene::Plane: {
                if (boxpet::ui::ui_plane_wants_to_leave()) {
                    boxpet::ui::ui_plane_clear_leave_flag();
                    // 游戏退出金币结算
                    int hits = boxpet::ui::ui_plane_last_hits();
                    bool time_up = boxpet::ui::ui_plane_last_time_up();
                    int32_t reward = boxpet::game::calc_plane_reward(hits, time_up);
                    if (reward > 0) {
                        boxpet::game::coins_add(reward);
                        boxpet::ui::coin_widget_float_text((int)reward);
                    }
                    board_load_screen(main_scr);
                    boxpet::ui::ui_plane_close();
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);
                }
                break;
            }
            case Scene::NetCfg: {
                if (boxpet::ui::ui_netcfg_wants_leave()) {
                    boxpet::ui::ui_netcfg_clear_leave_flag();
                    board_load_screen(main_scr);
                    boxpet::ui::ui_netcfg_close();  // 内部关 AP
                    g_scene = Scene::Main;
                    ui_main_attach_key(nullptr);
                }
                break;
            }
            case Scene::Death:
                // 死亡后由 ui_main 渲染墓碑并接收 wants_resurrect
                if (boxpet::ui::ui_main_consume_want_resurrect()) {
                    g_pet.reset_to_new_egg();
                    boxpet::game::storage_save(g_pet.state());
                    g_scene = Scene::Main;
                }
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}