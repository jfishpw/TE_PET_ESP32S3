# BoxPet 任务分解

依据 [spec.md](spec.md) 拆解，按里程碑顺序执行。每项完成即在 `[ ]` 打勾。

## M1 工程骨架与点亮

- [x] T1.1 创建 ESP-IDF 工程（esp32s3 目标、sdkconfig.defaults：16MB Flash、八线 PSRAM、CPU 240MHz）
- [x] T1.2 接入组件：`espressif/esp_lcd_st7789`、`lvgl/lvgl@~9.2`、`espressif/esp_lvgl_port`，点亮屏幕显示测试画面
- [x] T1.3 bsp/buttons：3 键 GPIO 中断 + 去抖 + 长按 1.5s 检测
- [x] T1.4 主界面静态布局：顶栏（电池/时钟/注意图标）+ 上下图标栏 + 宠物区占位
- [x] T1.5 bsp/power：电池 ADC 采样（GPIO1）、充电检测（GPIO48）、SYS_POW 保持供电
- [x] T1.6 自定义 UI 字体（lv_font_conv + simhei），tools/gen_font.ps1 可重新生成

## M2 游戏核心循环

- [x] T2.1 game/pet_def.h：落成 spec §3 全部数值常量（衰减表/生命周期表/进化阈值）
- [x] T2.2 game/pet：状态机与 1 秒 tick（衰减、便便、作息、撒娇、生病判定、死亡判定）
- [x] T2.3 生命周期：蛋→婴儿→儿童→少年→成年（照顾评分进化分支）→老年→死亡
- [x] T2.4 sprites + tools/sprite_gen.py：生成全部像素画（蛋/婴儿/儿童/少年/成年3形态/老年/动作帧/便便/Zzz/骷髅/心形/8图标）
- [x] T2.5 ui/anim：按宠物阶段与状态选帧、idle 呼吸 0.5s 切换、事件动作动画（吃饭 800ms / 生病持续 / 委屈 600ms / 开心 1200ms / 死亡持续）
- [x] T2.6 八大图标功能：食/光/玩/药/清/状/教 7 项已联调；其中"玩/状"在 M3 接入小游戏/状态页
- [x] T2.7 注意图标闪烁 + AttentionFlash 节流（bitmask 不变不重复发）

## M3 完整体验

- [x] T3.1 小游戏「左右猜」：场景、5 回合判定、胜负结算与动画（PetCore.game_result）
- [x] T3.2 bsp/audio：ES8311 + I2S 接口已封装（audio.cpp 为 stub，I2S DMA 与 codec 初始化待真机实现）
- [x] T3.3 game/storage：NVS + CRC32 + 周期 5min 保存 + 开机恢复 + 损坏重置
- [x] T3.4 电源管理：bsp/power_mgr 90s 超时 + 睡眠联动 + esp_pm 调频 + 按键复位
- [x] T3.5 设置菜单 ui_settings：时间模式/音效开关/重置 + 死亡画面 ui_main 长按复活

## M4 交付

- [x] T4.1 安装烧录文档（环境搭建/编译/烧录/操作说明/故障排查，含恢复小智固件方法）→ [boxpet/docs/M4_acceptance.md](boxpet/docs/M4_acceptance.md)
- [ ] T4.2 真机完整生命周期验收（演示模式 1 宠物日=1 小时跑通至成年）
- [ ] T4.3 按 checklist.md 全项验收

## 验证方式

- 逻辑层（game/）不依赖硬件，数值表可通过临时调试命令快速模拟（提供 `pet_debug.h` 时间加速开关）
- 每个里程碑在真机上冒烟：烧录 → 操作 → 观察串口日志
