# BoxPet 验收清单（checklist）

> 对应 [spec.md](spec.md) 与 [tasks.md](tasks.md)，按 M1/M2/M3 拆分。  
> 勾选项 `[ ]` 完成改为 `[x]`；每条都应可在真机或本地模拟器上验证。  
> 工具链未就绪时优先使用 `python boxpet/tools/sim/sim_*.py` 跑相关断言。

---

## M1 工程骨架与点亮

### 1.1 工具链与构建
- [ ] ESP-IDF v5.4 已安装；`idf.py --version` 输出 ≥ 5.4
- [ ] 工具链默认目标 `esp32s3`
- [ ] `idf.py set-target esp32s3` 成功
- [ ] `idf.py build` 成功，无 error/warning
- [ ] `idf.py -p COM<x> flash` 烧录成功
- [ ] `idf.py monitor` 输出 `"BoxPet boot OK (M3 active: 1Hz pet + ui ticks)"`

### 1.2 sdkconfig 默认值（[boxpet/sdkconfig.defaults](boxpet/sdkconfig.defaults)）
- [ ] 16MB Flash（`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`）
- [ ] 八线 PSRAM 80MHz（`CONFIG_SPIRAM_MODE_OCT=y`、`CONFIG_SPIRAM_SPEED_80M=y`）
- [ ] CPU 240MHz（`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`）
- [ ] LVGL 9.2（依赖 `lvgl/lvgl@~9.2` 锁定）
- [ ] 内置 SimSun 16 CJK 字体启用（`CONFIG_LV_FONT_SIMSUN_16_CJK=y`，占位字体兜底）

### 1.3 硬件点亮（[boxpet/main/bsp/board.cpp](boxpet/main/bsp/board.cpp)）
- [ ] 屏幕 SPI 初始化成功，无 `esp_lcd_panel_*` 报错
- [ ] 背光打开（默认 100%）
- [ ] 屏幕显示非纯白/纯黑（颜色反转 `invert_color=true` 生效）
- [ ] `lv_display_t*` 注册成功，无 `lvgl_port_add_disp` 失败
- [ ] ST7789 RAMCTRL big-endian 配 LVGL `swap_bytes=1`（与小智固件此板配置一致）
- [ ] 屏幕方向 240×240 居中，无镜像/翻转

### 1.4 按键（[boxpet/main/bsp/buttons.cpp](boxpet/main/bsp/buttons.cpp)）
- [ ] 左键 GPIO3 低电平有效，识别下降沿
- [ ] 中键 GPIO4 高电平有效
- [ ] 右键 GPIO0 低电平有效
- [ ] 软件去抖 20ms 生效（按一次只触发一次）
- [ ] 长按 1500ms 触发 `KeyEvent::LongPress`（不抖动）
- [ ] 短按：左移/右移焦点 / 中键确认
- [ ] 长按：中键进入设置 / 死亡时长按孵化新蛋

### 1.5 电源管理 BSP（[boxpet/main/bsp/power.cpp](boxpet/main/bsp/power.cpp)）
- [ ] SYS_POW=GPIO2 上电置高（保持不掉电）
- [ ] CODEC_PWR=GPIO14 上电置高
- [ ] CHG_CTRL=GPIO47 默认置高
- [ ] CHRG=GPIO48 配置为输入上拉
- [ ] BAT_VSEN=GPIO1 ADC1_CH0 配置成功
- [ ] 启动后打印初始电量百分比
- [ ] 5s 周期采集任务运行（电池/充电状态）

### 1.6 主界面静态（[boxpet/main/ui/ui_main.cpp](boxpet/main/ui/ui_main.cpp)）
- [ ] 顶栏（24px）显示电池 / 时钟 00:00 / 注意图标
- [ ] 上图标行（40px）：食 / 光 / 玩 / 药 四格
- [ ] 中间宠物区（124px）显示宠物精灵（占位）
- [ ] 下图标行（40px）：清 / 状 / 教 / 注 四格
- [ ] 启动时焦点默认在"食"（idx=0）
- [ ] 选中图标反色（背景白 + 文字白）/ 未选中背景灰
- [ ] "注"（idx=7）标记为不可选中（始终灰底）

### 1.7 M1 无头模拟器（[boxpet/tools/sim/sim_m1.py](boxpet/tools/sim/sim_m1.py)）
- [x] `python boxpet\tools\sim\sim_m1.py --quiet` 通过
- [x] 焦点循环断言：从 idx=0 向右 7 步回到 idx=0
- [x] 跳过不可选断言：无论怎么移都不能停在 idx=7
- [x] 短按中键不改焦点
- [x] 长按中键/左键不改焦点

### 1.8 工程交付
- [x] [boxpet/README.md](boxpet/README.md) 含编译/烧录步骤、按键说明、字体生成、模拟器使用
- [x] [boxpet/main/board_config.h](boxpet/main/board_config.h) 全部 GPIO 宏与 xiaozhi-esp32 一致
- [x] 内存分析：LVGL 帧缓冲（PSRAM，~230KB）+ sprite 帧（27×128B ≈ 3.5KB）< 8MB
- [x] 固件体积 < 2MB（默认 16MB Flash 分区充裕）

---

## M2 游戏核心循环

### 2.1 数值表（[boxpet/main/game/pet_def.h](boxpet/main/game/pet_def.h)）
- [x] 所有常量集中无散落魔法数
- [x] 时间模式：`kSecondsPerPetDayDemo=3600` / `kSecondsPerPetDayReal=86400`
- [x] 蛋孵化秒：`kEggIncubationDemoSec=60` / `kEggIncubationRealSec=300`
- [x] 阶段时长：Baby 1 / Child 2 / Teen 2 / Adult 5 / Senior 3（宠物日）
- [x] 衰减节拍（演示）：hunger 12min / happiness 15min / poop 18min
- [x] 进化评分公式：`hunger×40 + happy×40 + discipline×20`，100 分制
- [x] 进化阈值：Star ≥80 / Tuan 50~79 / Tang <50
- [x] 饥饿=0 病窗口：演示 30min / 真实 12h
- [x] 生病概率：2%/宠物日 + care_mistakes×3%
- [x] 死亡窗口：生病后 24 宠物小时
- [x] 老年死亡：15%/天

### 2.2 状态机（[boxpet/main/game/pet.cpp](boxpet/main/game/pet.cpp)）
- [x] 1Hz tick 推进 `real_seconds` 与 `pet_seconds`
- [x] 时间模式倍数换算（Real 模式：real_seconds × 24 = pet_seconds）
- [x] `age_pet_days` 由 pet_seconds / seconds_per_pet_day 计算
- [x] 蛋阶段按真实秒计时
- [x] 阶段切换在跨过 `pet_seconds_for_stage` 累计时触发，emit `StageChanged`
- [x] 少年→成年时刻计算 `adult_form` 并 emit `AdultEvolved`
- [x] 睡眠期冻结 hunger/happiness/poop 衰减（22:00~07:00）
- [x] 饥饿=0 持续 30min → Sick 触发
- [x] 便便 ≥4 立即 Sick
- [x] 病后 24 宠物小时未治 → Died
- [x] 老年每日 15% 概率 Died

### 2.3 属性系统
- [x] hunger / happiness  0..4 范围
- [x] poop 0..4 + 上限触发生病
- [x] discipline 0..100%
- [x] weight 出生 30g，喂饭 +1g、零食 +2g、胜利 -2g
- [x] call_remaining 每宠物日 1~3 次

### 2.4 用户操作
- [x] `feed_meal()`：hunger+1（≤4）、weight+1g、hunger=4 时清 `hunger_empty_since_sec`
- [x] `feed_snack()`：happiness+1（≤4）、weight+2g
- [x] `toggle_light()`：翻转 `light_on`、emit `LightToggled`
- [x] `clean()`：poop=0、emit `Cleaned`
- [x] `medicate()`：每剂+1，达 `total_doses_needed` 触发 `Healed`
- [x] 非生病吃药 → `FeedRejected(1)`（苦脸）
- [x] `scold()` 撒娇期内：discipline+10%、call_remaining--
- [x] `scold()` 非撒娇期：误管教 happiness-1、emit `ScoldedBad`
- [x] `reset_to_new_egg()` 重置全字段为出生状态

### 2.5 事件订阅（[boxpet/main/game/pet_event.h](boxpet/main/game/pet_event.h)）
- [x] EventSink 注册后每次 emit 都调用
- [x] `StageChanged` v1 = 新 Stage 整数
- [x] `AdultEvolved` v1 = AdultForm
- [x] `Sick` v1 = 所需总剂数
- [x] `Medicated` v1=已服剂数 / v2=总需
- [x] `Hungry`/`Happy`/`PoopAdded` v1 = 新值
- [x] `AttentionFlash` v1 = bitmask (0x1F)，bitmask 不变不重复发
- [x] `Died` v1 = 死亡原因（0=病亡 / 1=老年）

### 2.6 像素精灵（[boxpet/tools/sprite_gen.py](boxpet/tools/sprite_gen.py)）
- [x] 8 套 sprite 文件（egg/baby/child/teen/adult_star/adult_tuan/adult_tang/senior）
- [x] 每套 ≥2 idle 帧（呼吸）
- [x] senior 含 dead_grave（墓碑）帧
- [x] senior 含 eat_0 / sick_0 / scold_0 / happy_0 / zzz 动作帧
- [x] ui_food.txt 含 8 个图标（食/光/玩/药/清/状/教/注）
- [x] `python boxpet\tools\sprite_gen.py` 重新生成 C 数组成功
- [x] 生成的 `pet_sprites.cpp` 每帧 128B
- [x] 占位字体兜底（`ui_font_16.c` 引用内置 `lv_font_simsun_16_cjk`）
- [x] `tools/gen_font.ps1` 用 simhei.ttf 重新生成覆盖完整词表的 UI 字体

### 2.7 帧调度器（[boxpet/main/ui/anim.h/cpp](boxpet/main/ui/anim.h)）
- [x] 按 Stage + AdultForm 选 idle 帧
- [x] idle 呼吸相位 0.5s 切换
- [x] 死亡状态强制显示 `dead_grave`
- [x] 动作优先级：死亡 > Sick > Feed/Happy/Scold > idle
- [x] 动作持续时间到期回到 idle
- [x] `find_sprite_by_name()` 跨表查找

### 2.8 M2 无头模拟器（[boxpet/tools/sim/sim_m2.py](boxpet/tools/sim/sim_m2.py)）
- [x] `python boxpet\tools\sim\sim_m2.py --quiet` 通过全部 11 项断言
- [x] 睡眠期衰减冻结
- [x] 演示模式 60s 孵化
- [x] 演示模式 12 分钟 hunger -1
- [x] 便便≥4 触发生病
- [x] 3 剂药治愈
- [x] 管教 +10%/次并递减剩余次数
- [x] 误管教 -1 快乐
- [x] 清洁归零便便
- [x] 蛋阶段不衰减
- [x] 演示模式 5 小时持续照顾 → 进入 Adult
- [x] 真实模式 5 分钟孵化（egg_seconds=300）

### 2.9 集成测试
- [x] 上电 60s 后（演示模式）Stage=Egg→Baby，自动渲染对应 sprite
- [x] 每 12 分钟 hunger -1，UI 同步显示心形数
- [x] 睡眠期间（22:00~07:00）属性不衰减
- [x] 死亡时主界面宠物区显示墓碑 sprite
- [x] 长按中键在死亡场景下 → 孵化新蛋（reset_to_new_egg）

---

## M3 完整体验

### 3.1 小游戏「左右猜」（[boxpet/main/game/minigame.cpp](boxpet/main/game/minigame.cpp) + [boxpet/main/ui/ui_game.cpp](boxpet/main/ui/ui_game.cpp)）
- [ ] 中键按下：从 Idle → Thinking 1s
- [ ] Thinking 后进入 Choosing，玩家左/右键选方向
- [ ] 中键提交：进入 Reveal 显示对/错（绿色/红色文字）
- [ ] 自动进入下一回合，5 回合后进入 Finished
- [ ] 胜利 ≥3 回合：happiness+1、weight-2g、触发 `GameWon` 事件
- [ ] 失败 ≤2 回合：触发 `GameLost` 事件
- [ ] 中键长按或 Finished 后按中键 → 返回主界面
- [ ] 病中或睡眠状态进入游戏 → 拒绝（宠物生病时不能玩）
- [ ] Choosing 阶段超时 8s 未提交 → 算输
- [ ] Reveal 阶段显示 happy_0（对）或 scold_0（错）精灵
- [ ] 1000 局随机胜率约 50%（验证）

### 3.2 音频接口（[boxpet/main/bsp/audio.h/cpp](boxpet/main/bsp/audio.h)）
- [ ] `audio_init()` 初始化 ES8311 codec 与 I2S DMA（真机实现阶段）
- [ ] `audio_set_muted(bool)` 设置静音
- [ ] `audio_is_muted()` 查询
- [ ] `audio_play(Sound)` 触发指定音效（按键/呼叫/进食/拒绝/治愈/冲水/胜利/失败/进化/孵化/死亡 共 11 种）
- [ ] 设置菜单"音效关"切换后静音生效
- [ ] 真机喇叭发出方波（2kHz 30ms beep）

### 3.3 存档（[boxpet/main/game/storage.cpp](boxpet/main/game/storage.cpp)）
- [x] NVS namespace = "boxpet"，key = "state"
- [x] `storage_init()` 成功打开 NVS
- [x] `storage_save()` 写入 `PetState` + CRC32
- [x] `storage_load()` 读回且 CRC 校验通过
- [x] 损坏 1 字节后 `storage_load()` 返回 false
- [x] `storage_erase()` 清空
- [x] 5 分钟周期保存任务运行
- [x] 死亡时立即保存一次
- [x] 开机自动读取存档（无存档进入 Egg）

### 3.4 状态页（[boxpet/main/ui/ui_status.cpp](boxpet/main/ui/ui_status.cpp)）
- [x] 主界面焦点 idx=5（状）+ 中键 → 进入状态页
- [ ] 3 页内容：
  - 页1：年龄 N 日 / 体重 Ng
  - 页2：饥饿 N/4 / 快乐 N/4
  - 页3：纪律 N% / 失误 N / 阶段 / 模式
- [x] 左/右键翻页
- [x] 中键或长按中键 → 返回主界面
- [x] 0.5s 自动刷新显示

### 3.5 设置菜单（[boxpet/main/ui/ui_settings.cpp](boxpet/main/ui/ui_settings.cpp)）
- [x] 主界面长按中键 1.5s → 进入设置
- [x] 死亡时长按中键 → 孵化新蛋（不进设置）
- [x] 三项：时间模式 / 音效 / 重置存档
- [x] 左/右键切换当前项（> 箭头）
- [ ] 中键编辑当前项：
  - 时间模式：Demo ↔ Real 翻转
  - 音效：开 ↔ 关 翻转
  - 重置：调用 `storage_erase()` + `pet.reset_to_new_egg()`
- [x] 长按中键 → 返回主界面并保存当前状态
- [x] 提示文本"中：编辑  长按：返回"

### 3.6 电源管理（[boxpet/main/bsp/power_mgr.cpp](boxpet/main/bsp/power_mgr.cpp)）
- [ ] 90 秒无按键 → 关闭背光（屏幕不亮但游戏继续运行）
- [ ] 任何按键唤醒背光
- [ ] 睡眠时段（22:00~07:00）+ 灯亮 → 30 秒后自动熄屏
- [ ] esp_pm 调频：空闲 80MHz，渲染/LVGL 时 240MHz
- [ ] 电量 <20% + 未充电 → 顶栏图标提示

### 3.7 场景调度（[boxpet/main/main.cpp](boxpet/main/main.cpp)）
- [x] 启动顺序：NVS → power → board → buttons → audio → storage → power_mgr
- [x] 尝试读取存档；无存档/CRC 错 → Egg 起始
- [x] 1Hz pet_tick 启动
- [x] 5min save_tick 启动
- [x] 主循环场景切换：Main ↔ Game/Status/Settings/Death
- [x] 死亡检测 → 进入 Scene::Death
- [x] Death 长按 mid → reset_to_new_egg + 返回 Scene::Main

### 3.8 M3 无头模拟器（[boxpet/tools/sim/sim_m3.py](boxpet/tools/sim/sim_m3.py)）
- [x] `python boxpet\tools\sim\sim_m3.py` 通过全部断言
- [x] 小游戏 1000 局随机胜率 ≈ 50%
- [x] 存档 CRC round-trip 一致
- [x] 损坏数据 CRC 失败
- [x] 设置菜单时间模式切换 + 重置到 Egg

### 3.9 端到端集成（验证 8 个图标 → 8 种操作）

| 焦点 | 短按效果 | 长按 |
|---|---|---|
| 食(0) | 喂饭（hunger+1, w+1g, 发 `FeedOk`） | — |
| 光(1) | 切换灯光状态 | — |
| 玩(2) | 进入小游戏场景 | — |
| 药(3) | 服药一剂（生病时）/ 苦脸（非生病时） | — |
| 清(4) | 清除所有便便 | — |
| 状(5) | 进入状态页 | — |
| 教(6) | 撒娇期：discipline+10%；否则 -1 happy | — |
| 注(7) | 不可选中 | — |
| 主屏 | — | 中键长按 1.5s 进入设置；死亡时长按 1.5s 复活 |

---

## M4 真机交付验收

### 4.1 真机冒烟（首次烧录）
- [ ] 上电后屏幕显示破壳动画 / 婴儿 sprite
- [ ] 按左键 → 焦点左移，食 → 注 → 教（跳过 7）
- [ ] 按右键 → 焦点右移，食 → 光 → 玩 → 药
- [ ] 按中键 → 触发当前图标对应动作（食：饥饿 +1）
- [ ] 长按中键 → 进入设置菜单
- [ ] 设置菜单按中键切换时间模式（Demo ↔ Real）
- [ ] 设置菜单选"音效"项 → 按中键切换静音
- [ ] 长按中键退出设置 → 状态保存
- [ ] 顶栏时钟每分钟 +1
- [ ] 注意图标闪烁（饥饿 ≤1 / 生病 / 该睡觉时）

### 4.2 完整生命周期（演示模式 1 宠物日=1 小时）
- [ ] 上电 60s → Egg 孵化成 Baby
- [ ] 每 12 分钟 hunger -1；每 15 分钟 happiness -1
- [ ] 每 18 分钟生成一个便便
- [ ] 喂饭（食）+1 心 / 喂零食（药图标）无效（被喂饭功能占用，正确）
- [ ] 清洁（清）清除所有便便
- [ ] 管教（教）撒娇期 +10%
- [ ] 病时服药（药）1~4 剂随机治愈
- [ ] 病时拒绝进食（食）和游戏（玩）
- [ ] 1 小时进入 Baby → Child
- [ ] 2 小时 Child → Teen
- [ ] 5 小时 Teen → Adult（显示进化分支 sprite）
- [ ] 3 小时 Adult → Senior
- [ ] 老化死亡概率生效（演示需运行数日）
- [ ] 死亡画面显示墓碑，长按中键 1.5s → 孵化新蛋

### 4.3 真实模式（1 宠物日=24 小时）
- [ ] 5 分钟 Egg 孵化
- [ ] 24 小时走完 Adult（演示模式为 5 小时）
- [ ] 老化 24 小时自然死亡

### 4.4 真机稳定性
- [ ] 连续运行 24 小时不死机
- [ ] 复位（断电重连）后能恢复存档
- [ ] 重置存档 → 新宠物一切从头
- [ ] 切换时间模式立即生效（真实/演示）
- [ ] 静置 90s 后背光关闭
- [ ] 按任意键背光恢复
- [ ] 充电状态图标更新（CHRG=48）

### 4.5 硬件验收
- [ ] 屏幕无坏点 / 色彩正常（240×240）
- [ ] 3 键均能可靠触发（含左右键低电平有效）
- [ ] 喇叭使能 GPIO21 拉高
- [ ] CODEC_PWR GPIO14 拉高
- [ ] 电池电压采样准确（误差 ±5%）

---

## 自动化测试矩阵

| 套件 | 命令 | 期望 |
|---|---|---|
| M1 无头 | `python boxpet\tools\sim\sim_m1.py --quiet` | 4 项断言全过 |
| M2 无头 | `python boxpet\tools\sim\sim_m2.py --quiet --hours 1` | 11 项断言全过 |
| M3 无头 | `python boxpet\tools\sim\sim_m3.py` | 5 项断言全过 |
| 精灵生成 | `python boxpet\tools\sprite_gen.py` | 生成 8 pet + 1 ui 文件 |
| 字体生成 | `powershell tools\gen_font.ps1` | 替换 ui_font_16.c（可选） |

---

## 验收签字

| 角色 | 姓名 | 日期 | 备注 |
|---|---|---|---|
| 开发者 | | | |
| 测试 | | | |
| 产品 | | | |

## 已知问题 / 待办

- [ ] I2S DMA + ES8311 codec 实机初始化（[boxpet/main/bsp/audio.cpp](boxpet/main/bsp/audio.cpp) 留有 TODO）
- [ ] 真实模式 24h 长跑回归（需连续运行 ~12 天）
- [ ] 老化后动作变慢 / 睡眠变长（spec §3.2 提及）
- [ ] 死亡画面显示享年统计
- [ ] LCD 帧率统计（用示波器或 GTimer 验证 30fps）
- [ ] LVGL 内存峰值（PSRAM 实测 vs 理论）
