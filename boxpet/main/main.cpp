// main.cpp — BoxPet 入口 + 场景调度器
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/wallclock.h"
#include "bsp/power.h"
#include "bsp/power_mgr.h"
#include "bsp/audio.h"
#include "ui/ui_main.h"
#include "ui/ui_game.h"
#include "ui/ui_status.h"
#include "ui/ui_settings.h"
#include "game/pet.h"
#include "game/pet_def.h"
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
using boxpet::game::PetCore;

static PetCore g_pet;
static esp_timer_handle_t g_pet_tick = nullptr;

static void pet_tick_cb(void* /*arg*/) { g_pet.tick_real_second(); }

// 5 分钟周期存档（5*60*1000ms）
static esp_timer_handle_t g_save_timer = nullptr;
static void save_tick_cb(void* /*arg*/) {
    boxpet::game::storage_save(g_pet.state());
}

// 场景调度：主界面 / 游戏 / 状态页 / 设置
enum class Scene : uint8_t { Main, Game, Status, Settings, Death };
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
    // 调试：打印复位原因（1=POWERON 3=PANIC 4=INT_WDT 5=TASK_WDT 7=BROWNOUT 8=...）
    ESP_LOGI(TAG, "reset reason=%d", (int)esp_reset_reason());
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(buttons_init());
    // 音频失败不致命（无喇叭/驱动问题不应挡住游戏）
    esp_err_t audio_ret = boxpet::bsp::audio_init();
    if (audio_ret != ESP_OK) ESP_LOGW("main", "audio_init failed: %s", esp_err_to_name(audio_ret));
    boxpet::bsp::wallclock_init();
    ESP_ERROR_CHECK(boxpet::game::storage_init());
    ESP_ERROR_CHECK(boxpet::bsp::power_mgr_init(&g_pet));

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