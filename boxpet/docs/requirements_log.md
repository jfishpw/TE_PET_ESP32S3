# BoxPet 需求与修改记录（Requirement & Change Log）

> 硬件：正点原子 ATK-DNESP32S3-BOX0（ESP32-S3R8，8MB PSRAM，ST7789 1.54" 240×240，
> ES8311 音频 codec，3 实体按键，锂电池 + Type-C）
> 框架：ESP-IDF v5.4 + LVGL v9 + FreeRTOS
> 参考仓库（全局约定，需求6）：`https://github.com/78/xiaozhi-esp32`
> ——麦克风/I2S 音频配置、WiFi 配网、音频编解码优先参考其 `main/boards/`、`main/audio/`、`main/protocols/`

***

## 历史修改记录（按提交/迭代倒序）

### 迭代 M17 —— 天气系统（联网查询 + 背景形态 + 温度） + 玩/教改 4 小时冷却

- **天气背景系统**（`bsp/weather.*` + `ui/weather_bg.*`）：
  - **8 种形态**：晴/多云/阴/雨/雪/雷阵雨/雾/大风，叠加在既有昼夜背景之上；
    画布绘制（240×124，PSRAM 缓冲），每帧仅一次 `invalidate`，雨天/雪/雾/风/云
    5Hz 动画、雷阵雨每 4 秒闪白；装饰（太阳/月亮/云/星星）随天气联动显隐。
  - **数据源 open-meteo（免 key、HTTP 直连）**，默认长沙坐标
    （`current=temperature_2m,weather_code,wind_speed_10m`）；WMO code → 形态
    （含大风风速判据）；有真实数据时天空左上显示「天气 + 温度」，离线随机不显示温度。
  - **节奏**：每 2 小时一次；失败/无网络 → 随机一种（同样 2 小时换一次）；
    **每次开机（点亮 30s 后）允许补查一次**（深睡醒来/重启后尽快拿到最新天气，
    配网成功后无需等满 2 小时）。天气/温度/上次查询时刻存 NVS（独立键）。
  - **安全时机**：仅屏幕亮着且点亮 ≥30s 才查询；**深睡/熄屏一律不查**；查询在
    独立任务（`wx_task`）完成，不碰 LVGL、不占 UI/宠物 tick 上下文 → 规避看门狗。
  - 坑修复：`save_state` 未同步内存 `s_last_query` → 曾每 300ms 重查（NVS 磨损）。
  - 宠物画布/状态图标画布底色改为**天气感知**（并随昼夜/天气变化强制重绘），
    修掉天气变灰/变暗时露出的“亮方块”（不在一个图层的问题）。
  - 排查辅助：net_mgr 打印 STA 断连原因（15/205=密码错、201=找不到 AP、
    200=信号弱），重试 3→5 次。
- **玩/教 每日次数 → 每个玩法/课程各自 4 小时冷却**：
  - `kPlays[].daily_limit`/`kEduDailyLimit` 改为 `cooldown_hours`
    （节奏与各教育课程 = 4；丢球/自由玩/计数器 = 0 无冷却）；
  - 冷却时间戳存**独立 NVS 键** `play_edu_cd`（不改 PetState 结构 → 旧存档不失效）；
    计时随模式缩放（真实=真实 4 小时；演示=4 宠物小时≈10 分钟）；
    进入游戏/课程即开始计时；拒绝提示改为「刚玩过/学过，4小时后再来」。
- 字库补 晴/阴/雪/雷/阵/雾/° 等字并重新生成 `ui_font_16.c`。

### 迭代 M16 —— 死机根因定位(lv_inv_area) + 事件 UI 迁移 + USB 判据修复

- **死机根因（coredump 实证）**：`ui_tick_task_fn → tick_timer_cb → lv_inv_area →
  lv_display_send_event(LV_EVENT_REFR_REQUEST) → lv_array_front`，数组地址位于 LVGL
  池内 → 遍历损坏的失效区域数据空转 >5s，饿死 IDLE0 → TWDT 复位。诱因是 UI tick
  每 100ms 对宠物画布/品质光效/粒子/图标做几十次 set_pos/set_size/set_style（每次都走
  `lv_inv_area`）。**修复**：
  - 全量**变化门控**：坐标/尺寸/透明度没变不调用 LVGL；隐藏走边沿（不再每 tick
    `add_flag`）；普通品质的宠物一次隐藏后完全不再触碰 FX 对象。
  - **所有事件 UI 从宠物 tick（esp_timer 任务）迁移到独立 UI tick 任务**：特殊事件
    弹窗+亮屏+音效、事件结算提示、病/死/濒死亮屏，宠物 tick 只置标志
    （`s_pending_popup`/`s_pending_resolved`/`s_wake_pending`）。这正是“点亮屏幕→
    弹来访→按键→死机”的高危路径。
- **属性过低提醒（需求）**：精力/饱食/心情/卫生/健康 任一 <20 → 亮屏 + 音效(Call)
  + 提示「X过低！」；仅在“刚跌破 20”的边沿触发一次（注意位 bit7 精力 / bit8 健康
  新增；便便从 bit4 拆出为 bit9，避免误报“卫生过低”）。
- **USB 接入判据修复**：原判据只有充电脚低（充满后变高 → 漏判，插着 USB 仍熄屏）。
  新增 `usb_connected_now()` = 充电脚低 **或** `usb_serial_jtag_is_connected()`；
  统一应用于：插 USB 不息屏、不浅睡、不深睡、深睡自检后退出续睡。

### 迭代 M15 —— 稳定性加固 + 商店修复 + 节奏/音乐玩法重做

- **稳定性排查与加固（连续静默挂死）**：
  - 现象：屏幕冻结/按键无响应；coredump 显示 esp_timer 任务卡在 LVGL tick 的锁
    操作、IDLE0 被饿死；后期出现“无 coredump 的静默挂死”。
  - **RTC 慢时钟源回退**：M13 为修深睡时间漂移改用 `INT_8MD256`，改后设备连续
    极底层死机，回退为出厂默认 `INT_RC`（对照实验；时间漂移后续另寻安全方案）。
  - **UI tick 独立成 FreeRTOS 任务**（原与“宠物 1Hz tick + LVGL 2ms tick”共用
    esp_timer 任务，任一阻塞三者全停 → “时钟停住”）。
  - **补跳保护**（浅睡/长间隔醒来逐秒追补）：补跳期间禁用随机事件 + UI 忽略按键
    （杜绝“按键任务 vs 补跳任务”并发读写 PetState 卡死）；每 32 秒让出 1ms CPU
    防饿死低优先级任务；**补跳完成后按 dt/60 补抽随机事件**（原节奏 3%/分钟），
    避免“长时间浅睡=永远没有随机事件”。
  - **看门狗**：`CONFIG_ESP_TASK_WDT_PANIC=y`（触发即 coredump+复位）；UI 心跳
    停摆 15s → 独立高优先级任务（prio 23 > esp_timer 22）`abort()` 落 coredump；
    pm_sleep 不再挂 TWDT（低优先级被抢占会误报）。
  - **持续渲染的宠物/图标画布回内部 LVGL 池**（128KB 实测够用），消除 PSRAM
    画布这一 M13 后的相关变量。
- **插 USB 不息屏**：充电脚检测为接入时跳过熄屏并保持亮屏（拔线后恢复 10s 超时）。
- **商店购买修复**：买食物只“清冷却”不生效 → 改为库存 +1（`add_food_inv`），并
  刷新主界面顶栏金币（`coin_widget_refresh`）；购药改为挂起 → 主循环
  `ui_shop_poll()` 异步执行（事件级联不在按键任务持锁上下文跑）。
- **玩→节奏**：解锁 Lv5 → **Lv4**；玩法升级为**左/中/右三键三轨**跟拍记忆，
  单步即时反馈，进度显示“跟拍 x/5 步”。
- **教→音乐重做：下落金币节奏游戏**：
  - 金币从上方两轨（关于中心对称，中心 44/196）落下，过底部判定线按对应左右键；
  - 命中 +1 分（高音反馈+判定垫闪光），漏掉不得分；
  - 全程**节拍伴奏**（新增单音合成接口 `audio_play_tone`，五声音阶每拍一音）；
  - 一局 30 秒（60 音符 × 500ms）；金币亮橙金+深描边；金币对象池加槽位归属防互相隐藏；
  - 结算按分数奖励**魔力**（每命中 +0.5）与**金币**（教育奖励公式），并走 edu_end。
- 字库补 体/挂/跟/拍/步/准/线 等字并重新生成 `ui_font_16.c`。

### 迭代 M14 —— 多阶段多分支进化（v5）+ 闲置玩耍 + 挂死诊断加固

- **多阶段多分支进化系统（v5，替换 M12 的"Lv5 单次分支进化"）**：
  - 路线：蛋（**4 种蛋皮随机**：斑点/云纹/星纹/翠叶）→孵化→ 幼生 LV1 →
    **LV2+2 天** 成长 → **LV3+5 天** 成熟 → **LV4+10 天** 完全体 → 60 天 老年
    （寿命/死亡机制不变）。**等级+日龄双门槛**：两条件都满足才进化，防速刷也防卡级。
  - 每阶段 3 形态 = **力/魔/速三分支**，每次进化重判（取三维最大，平局 力>魔>速，
    可换向）；新增 `Stage::Ultimate`、`EvoBranch`/`EvoQuality` 枚举，删除旧
    `EvoStage`(AdultA/B/C/Normal)。
  - **品质档位**（三维和 <90 普通 / 90~150 优秀 / >150 华丽）不占素材：UI 层用
    星光粒子（优秀×2）与呼吸光晕+粒子（华丽×4）表现，进化时定档。
  - **16 形态像素画**由 `sprite_gen2.py` 程序化生成（体型随阶段增大：护腕→拳套→
    金腰带 / 呆毛→星冠→星环长袍 / 尖耳→尾巴→围巾闪电），新增 90 帧 ≈103KB flash。
  - 成长联动改为：主食→力+2、教育/节奏→魔+2、丢球/自由玩→速+2、零食三项+1、
    高级料/最爱→力魔+1（溢出 5:1 转经验沿用）。
- **闲置玩耍系统**：无操作 **2s**（原 10s 可调）触发，蹦跳/追蝴蝶/玩球三选一循环
  （蝴蝶振翅 S 形飞、球弹跳顶球、宠物漫步跟随），任意按键立即中断。替换原
  "8~16s 随机小动作"，漫步/跳跃保留为子动作。
- **挂死诊断加固（连续两次静默挂死排查）**：
  - 现象：特殊事件（商人/下雨）结算后约 5s，屏幕冻结、按键无响应、串口全静默；
    首次 coredump 显示 LVGL 渲染任务读到 NULL/垃圾子指针（对象树损坏）。
  - `CONFIG_ESP_TASK_WDT_PANIC=y`：TWDT 触发即 panic → coredump 落盘 + 复位
    （原配置只打印，日志走已断连的 USB CDC → 证据丢失且永久僵死）。
  - 睡眠任务挂 TWDT（浅睡前退订防误报）+ UI 心跳看门狗：亮屏时 UI tick 停摆
    15s → 停喂 TWDT → panic 落 coredump（全任务回溯栈）后复位。
  - LVGL 渲染任务栈 7168 → 12288；每 5s 打印 `lv_mem`（实测池 128KB 仅用 18%、
    碎片 1%，排除池耗尽）。
  - 商店购药（`medicate` 事件级联）改挂起标记 + 主循环 `ui_shop_poll()` 执行，
    不在按键任务持 LVGL 锁内跑。
  - 字库补 体/挂 等字并重新生成 `ui_font_16.c`。
- **设计文档**：新增 `docs/evolution_design.md`（进化路线/16 形态 ASCII 图鉴/
  品质规则/资源规划/属性提升指南/代码改动映射）。

### 迭代 M13 —— 深睡时钟修复 + 字库补字 + 低状态四角图标

- **深睡后时钟快 1-2 分钟（根修）**：RTC 慢时钟由内部 136kHz RC 切换为主晶振分频
  `INT_8MD256`（≈68kHz）。根因：RC 振荡器随温度漂移（夜间降温变快），深睡期间
  `esp_rtc_get_time_us()` 差值偏大 → 醒来补跳过多 → 时钟快 1-2 分钟（~0.2-0.4%）。
  改后深睡计时与 40MHz 主晶振同精度（整夜误差秒级）；代价：深睡期间 RC_FAST 振荡器
  常开，深睡电流 +约 0.1mA 级（整夜 ~1mAh）。`sdkconfig` + `sdkconfig.defaults` 同步
  （防 sdkconfig 重生成回退）；顺带单次深睡定时上限由 8.8h 提至 17.4h。
- **状态页方块（缺字修复）**：状态页第 2 页多个"方块"为自定义字库缺字（LVGL 把缺的
  字画成空心方块）——进化形态名缺 魔/法/速/度/形、"少年·形态"分隔点缺 ·。
  全量扫描 UI 字符串共补 37 字（含 toast 床/午/前/满/停、聊天面板 绑/板、游戏页 果、
  录音诊断 麦/风/克 等），`tools/font_charset.txt` → `tools/gen_font.ps1` 重新生成
  `ui_font_16.c`。
- **低状态明显体现（需求）**：属性视觉阈值 25~30 → **60**（严格 `<60`）；
  新增 `ui/status_icons.*`——饱食<60 鸡腿 / 心情<60 泪滴 / 卫生<60 臭气+苍蝇 /
  精力<60 "Z"，主界面宠物区四角 24x24（12x12 像素画 @2x，复用全局 16 色调色板），
  可同屏叠加、轻微上下浮动、昼夜天空/草地底色自适应；蛋期/睡眠/死亡不显示，
  菜单/事件浮层打开时被盖住。表情帧联动（bad/scold/sick）保留，比较改严格小于。

### 迭代 M12 —— 升级系统改为多分支进化 + 属性联动外观

- **多分支进化系统（v4，替代原"日龄结算 EvoForm + 纯等级成长"模型）**：

  - 新增分支值 `evo_power/evo_magic/evo_speed`（0..100）与进化阶段枚举 `EvoStage`
    （Egg/Baby/AdultA 力量型/AdultB 魔法型/AdultC 速度型/Normal 普通形态）、
    外观资源ID `evo_look`（`EvoLook`）；

  - **触发**：等级 ≥`kEvolveMinLevel(5)` 且 IDLE（睡觉/生病/动画中不进化，等回 IDLE）；
    少年→成体不再按日龄（day 11 规则删除），始终未进化者 day 60 直接入老年；

  - **分支判定** `evo_decide_branch()`：取三值最大，平局优先 power > magic > speed；
    最大值 <`kEvolveBranchMin(30)` → 普通形态（保持少年外观 teen，即"未进化完全"）；

  - **外观映射**（`anim.cpp kEvoLooks[]`）：力量→adult\_tuan（橙·圆滚壮实）、
    魔法→adult\_star（黄·星光）、速度→adult\_tang（绿·流线扁身）、普通→teen；
    全帧表 48x48 同尺寸，动作帧按 `<base>_<key>` 参数化，无缩放/错位问题；

  - **进化演出**：`EvolveStart` 事件 → Evolve 提示音先行 → 白色光晕覆盖层闪烁
    \~3.6s（LVGL anim，结束自删）+ 精灵新旧形态 300ms 交替（`AnimAction::Evolve`，
    `trigger_evolve` 记旧帧）→ 结束自动定格新形态；EVOLVING 4s 后回 IDLE；

  - **持久化**：分支值/阶段/外观ID 全部入 PetState（NVS blob 整体存取，自动持久）。
    ⚠️ 结构体尺寸变化 → 旧存档一次性失效（CRC/size 校验不过 → 按新蛋开始）。

- **属性成长联动**（`add_growth()` 统一入口，超出 100 溢出按 5:1 转经验）：
  主食→力量、零食→速度、高级料/最爱→魔法；丢球→速度、节奏→魔法、自由玩→力量
  （输了按 40% 折算）；各教育课程→魔法；状态页第 2 页新增"力/魔/速"行。

- **精力损耗下调**：清醒流失 0.25→**0.15/min**（满精力可支撑 \~11h 清醒）。

- **属性联动 idle 外观**（接通从未使用的 `child_bad/teen_bad` 灰暗帧 + scold/sick）：
  醒着 IDLE 时——精力≤25 或 心情≤29 → bad 帧（垂头无神，成体回退 scold）；
  饥饿≤29 → scold（饿肚子委屈）；卫生≤30 → sick（脏得蔫蔫）。
  睡眠/生病/抑郁仍用专属帧，优先级高于属性外观。

- **清理**：删除旧 `EvoForm`/`decide_evolution`/成长统计字段（mood\_sum/hygiene\_sum/
  ticks/feed\_on\_time/perfect\_streak/dipped\_below\_50，只写不读）。

- 涉及：`pet_def.h`（EvoStage/EvoLook/分支表）、`pet.h/pet.cpp`（进化判定/add\_growth/
  喂食玩耍教育联动）、`pet_event.h`（EvoDecided→EvolveStart v1=阶段 v2=旧外观）、
  `anim.h/anim.cpp`（外观映射表/属性 idle/进化动作）、`ui_main.cpp`（进化演出+光效）、
  `ui_status.cpp`（力魔速行/形态显示）。

### 迭代 M11 —— 睡眠逻辑全部重写（睡眠模型 v3）

- **新睡眠/起床模型**（与用户逐条确认）：

  - 入睡：白天精力=0 强制睡；夜间到点强制睡（IDLE 30s 提示、SICK/DEPRESSED 直接睡，保留）；
    手动"光"= 白天/夜间均可哄睡；

  - **夜间（窗口内）绝不醒**：无自动醒、开灯无效（提示"夜里它还在睡…"）；到起床点但精力<100 →
    继续睡（由 SLEEPING 出窗条件+精力门槛保证，恢复 +4/min 实际睡 25 分钟即满）；

  - **白天（窗外）起床两条路**：手动开灯需**精力≥60**（不足提示"精力不足，起不来"，"光"键被吞无效）；
    **精力睡满 100 自动醒+自动开灯**（不区分入睡方式，统一自动醒，WakeUp v1=2 "早上好！"）；

  - **连点唤醒机制废弃**：睡觉时抚摸只"翻个身"，`force_wake` 删除；起床唯一入口=开灯（白天≥60）/自动醒。

- **深休眠扩到全天**：真实模式宠物 SLEEPING + 熄屏 → 一律 Deep Sleep（新增 `kDsReasonNap`：
  白天小睡闹钟=精力恢复满，夜间=到起床点）；深睡期间不触发任何事件（SLEEPING 短路 + 补跳期
  事件开关，需求4 保持）；定时自醒补跳后统一 `power_mgr_deep_sleep_continue_reason()` 决定续睡/开机。
  演示模式不深睡（保证可测试性）。已知代价（用户确认接受）：白天每次小睡醒来都重启一次。

- **精力数值调整**（用户指定）：清醒流失 0.5→**0.25/min**，睡眠恢复 2→**4/min**（0→60 睡 15 分钟、
  0→100 睡 25 分钟）；玩耍/学习扣值不变；新增门槛常量 `kWakeMinEnergy=60`。

- 涉及：`pet_def.h`（速率/门槛）、`pet.cpp`（SLEEPING 判定/toggle\_light 返回码/删 force\_wake/
  新增 in\_sleep\_window\_now、seconds\_to\_energy\_full）、`power_mgr.*`（Nap 原因/续睡判定）、
  `main.cpp`（恢复快路径统一续睡评估）、`ui_main.cpp`（开灯提示/唤醒文案/入睡深睡提示改为
  "晚安～按左右键唤醒"）。

### 迭代 M10 —— 计数器收尾 + 小智默认地址兜底

- **计数器**：删除算珠分组虚线（观感不佳）；取消每日教育次数上限（`can_learn` 豁免 +
  `edu_end` 不再计入 `edu_count_today`，仍每次 +1 智力、低能耗）。

- **小智云默认地址**：默认 `wss://api.tenclass.net:443/xiaozhi/v1/`，在 `prefs_get_net`
  读取兜底——重置存档/首次配网/配网页留空时自动生效，无需手动填写；配网页
  placeholder 同步更新；`ui_chat` 进入判断不再要求手动填过地址。

### 迭代 M9 —— 教育新增"计数器"（亿/万/个级拨珠位值教学）

- **属性**：无解锁等级门槛（`kEdus` 中 Counter `unlock_level=0`，任何等级可玩）；**无每日次数限制**（迭代 M10 起）；
  能耗低（energy 1），结束固定 +1 智力（`edu_end` Counter 分支：不参与对错结算/心情惩罚/技能学习）。

- **玩法**（ui\_game `Mode::Counter`）：左右键在 9 位间切换选中位（个→十→百→千→万→十万→百万→千万→亿），
  中键给当前位 +1（满 999999999 回绕到 0），屏显按"亿级/万级/个级"分组展示数值并高亮当前位名；
  长按中键结束结算"智力 +1"。PlayKind/EduKind 未持久化，存档无需迁移。

- **界面（算盘式竖条算珠）**：计数器页面不显示大宠物——右上角 48x48 迷你宠物图标；中央 9 根
  竖条（左→右 = 亿→个位），每根底部向上亮起该位数值颗彩色珠子（青/绿交替，选中位整列橙色），
  竖条上方显示每列当前位数字，列间以虚线分隔"个级|万级|亿级"。

- **界面修正（v2）**：① 修复顶部位数字与算珠列"左右颠倒"错位（数字标签改为与算珠列同向 亿→个）；
  ② 算珠画布上移，正下方空出区域逐列**中文竖排位名**（个/十/百/千/万/十万/百万/千万/亿，
  两字位名换行竖排）；③ 画布缓冲改为 PSRAM 分配（LVGL 内部堆仅 64KB，48KB 画布+宠物已超限
  导致整页空白）。

- 字库追加：亿/万/器/十/百/千（原字库缺"十百千"导致选中位名无法显示，`tools/font_charset.txt`
  重新生成 `ui_font_16.c`）。

### 迭代 M8 —— 玩法精简：删除捉迷藏 + 解锁等级对齐 5 级

- **删除"捉迷藏"**（与"丢球"同为左右猜、玩法重复）：`PlayKind` 枚举重排（Ball/Rhythm/Free）、
  `kPlays` 删表项、`ui_game` 删 `Mode::HideSeek` 全程分支（标题/出题/题面/作答/配置映射）、
  升级解锁提示删"捉迷藏"项；玩菜单随之自动少一项。

- **解锁等级 8→5**（宠物实际最高 5 级，8 级门槛永远够不到）：玩-节奏（Rhythm）与教-音乐（Music）
  的 `unlock_level` 由 8 改为 5，`kUnlockMusicLv` 同步 8→5。

- 影响面：`pet_def.h`/`pet.cpp`/`ui_game.cpp`/`ui_main.cpp`；`PlayKind` 未持久化，存档无迁移问题。

### 迭代 M7 —— 分层省电：深休眠（夜间+低电量）+ 浅休眠增强 + NVS 写节流

**分层状态落地**（对照省电策略表逐项核对）：

| 状态  | 触发                 | 实现                                                                           |
| :-- | :----------------- | :--------------------------------------------------------------------------- |
| 活跃态 | 亮屏/交互              | 240MHz DFS+WiFi 按需（原已具备）                                                     |
| 浅休眠 | 熄屏（10s 无操作）        | Light Sleep（1\~3mA）+ **新增**熄屏 DFS 40MHz（插 USB 无法深睡的充电空转场景）+ UI tick 10Hz→2Hz |
| 深休眠 | 夜间宠物睡眠 / 电量≤10%未充电 | **新增** `esp_deep_sleep_start`（<50μA 量级）                                      |

- **深休眠（重启式，ESP32-S3）**：

  - 夜间（真实模式 + 宠物 SLEEPING + 处于睡眠窗口）：睡到起床点单个 RTC 闹钟；整夜不触发随机事件；

  - 低电量（≤10% 未充电）：每 30min 自醒自检（插充电即退出）+ 左右键唤醒；

  - 入睡前：`storage_save_if_changed` + 墙钟快照 + 关 PA/CODEC 电源（深睡零静态电流，唤醒=重启由 audio\_init 重建）+ SYS\_POW 锁存重 hold（防断电循环）；

  - 唤醒恢复：`esp_rtc_get_time_us()` 差算实际睡眠秒数（RTC\_NOINIT 保存入睡时刻）→ 逐秒补跳宠物（`tick_one_second` + 同步推进墙钟，保证"到点自然醒/入睡"在补跳中正确触发）→ 按键唤醒/到起床点/已充电 → 正常启动；仍深夜/仍低电 → 续睡；

  - 深休眠唤醒**仅左右键**（中键高电平不支持 EXT1 统一唤醒模式，Light Sleep 阶段三键照常）。

- **浅休眠增强**：ST7789 增加 `0x10 Sleep In`/`0x11 Sleep Out`（原 0x28 只停扫描不断内部 DC/DC，补发后每屏省 2\~5mA，唤醒 120ms 稳定后再开显示）；熄屏时 `esp_pm_configure` DFS 下限 80→40MHz（仅 USB 空转场景有效，电池态 Light Sleep 已全停）；ui\_main 熄屏渲染降频 10Hz→2Hz。

- **随机事件约束**（按需求）：`check_special_events` 在 SLEEPING 状态直接短路（夜间睡觉零事件）；深休眠补跳期间经 `PetCore::set_events_enabled(false)` 关闭随机事件（防低电量 30min 自检补跳时"无 UI 凭空弹事件"）；属性衰减/恢复、生病/死亡等正常状态变化不受影响。

- **NVS 写节流**：新增 `storage_save_if_changed()`（状态 CRC 未变则跳过），5 分钟周期存档与睡眠存档均走变更检测。

- **不适用/已满足项**：SPI 总线释放（Light Sleep 由 `CONFIG_PM_SLP_DISABLE_GPIO=y` 自动隔离；深睡数字域断电）；PSRAM 自刷新（IDF 睡眠流程自动处理）；外部 32k 晶振（本板无，内部 RC 已校准）；Tickless Idle（已启用）；USB-JTAG 漏电（开发期需保留烧录通道，量产可关闭则另行配置）。

- 已知限制：深休眠期间中键不可唤醒（硬件唤醒电平限制）。入睡瞬间（SLEEPING 且真实模式+睡眠窗口+未插电）toast 自动提示"晚安～夜里按左右键唤醒"（字库补"唤"字），避免用户按中键无反应误以为死机。

### 迭代 M6 —— 小智云语音：上行 Opus 采样率根修 + 录音自动停止完善 + 去调试日志

- **服务器录音失真（变粗/变短/嗒嗒声），根因修复**：对照官方 `main/audio/` 源码确认——

  - 官方**上行编码硬编码 16000Hz**（`AS_OPUS_ENC_CONFIG` 恒定 16k/mono/AUDIO/60ms，audio\_service 中
    `encoder_sample_rate_=16000`），服务器 ASR 上行一直按 16k 接收；

  - 此前我们按服务器协商频率（24kHz）上采样编码 → 服务器把 24k 帧当 16k 读 → 录音变粗/变短/嗒嗒；

  - 修复（`main/bsp/xz_client.cpp`）：`opus_codecs_for()` 编码器固定
    `opus_encoder_create(16000,1,AUDIO)` + VBR/DTX + AUTO 码率 + complexity=0（对齐 `AS_OPUS_ENC_CONFIG`）；
    解码器仍跟随服务器协商频率；`xz_encode_task()` 累积 60ms\@16k=960 样本**直接编码、绝不上采样**。

- **录音自动停止完善（3s 无声停止 = 等同按中键）**：

  - `on_mic_frame` 自适应 VAD（`main/ui/ui_chat.cpp`）：首 \~300ms 把噪声底校准到环境电平
    （修旧版钉死 120 → 底噪被当人声、听完不停）；仅在非强语音帧时追踪噪声底（说话中不抬底噪防吞真声、
    停话后缓升抬高阈值）；帧退火 hot 计数滤单帧底噪尖峰；滞回清除防句内抖动；

  - 静默超时 2s→3s；统一 `stop_listening_locked()`（手动/自动共用：`listen.stop` + 停采 + 置"回复中…"），
    彻底杜绝自动停漏发录音/漏发 stop；

  - `xz_on_state`：服务器主动结束会话（state=2 开始回复 / 3 回复完毕）把本机带离 Listening 时强制停
    录音流，防空话无限上行占带宽；进入 Replying 相位显式亮出"回复中…"。

- **移除调试日志**：ST state=%d / TALK begin|end sent / 周期 up enc f=… / xz\_mic 音量阈值等诊断输出。

### 迭代 M5（commit f5a3a09）—— 飞机游戏 + 金币商店 + 音频链路大修

- **飞机打害虫小游戏**：敌人整体放大一倍（12/20/28px）、飞机 3 颗心（HUD 红心）、
  被撞 3 次结束（1.2s 无敌闪烁防连撞）、射击/受伤/胜负音效、按命中数结算金币

- **金币经济系统**（game/coins.\*）：各游戏得分换金币，NVS 持久化

- **商店**（ui/shop.\*）：零食/高级料理/最爱食物/特效药，金币消费，冷却清除

- **识字游戏中文方块修复**：UTF-8 3 字节正确复制（`memcpy(&pool[i*3],3)`），
  字库补齐 移/命/金/币/店/机/需/距/眼/购/足/睛/号/亮/心 等字

- **喇叭完全无声（根因修复）**：I2S DOUT/DIN 引脚对调（错误 DOUT=6/DIN=9），
  按 BOX0 实际接线（xiaozhi-esp32 板级定义）改为 DOUT=9 / DIN=6

- **音效无限重播修复**：I2S DMA 为 6×20ms 环形缓冲，音效播完停止写入后 DMA
  循环播放残留旧数据 → 音频任务空闲时持续写 20ms 静音；写入加 400ms 超时防卡死

- **亮屏破音 + 唤醒后无声修复**：PA 睡眠期间 pad-hold 不掉电；唤醒时 I2S
  disable→enable 完整重启 + ES8311 软复位重初始化 + 恢复音量解除静音

- **飘字动画崩溃修复**：`coin_widget_float_text` 的 `lv_anim_start` 移入 LVGL 锁内

- **按钮/全部互动游戏音效补齐**：状态页翻页、商店焦点、节奏游戏节拍等

- **电量显示**：改用 BOX0 官方实测查表（2951→0%，3019→20%，3037→40%，
  3091→60%，3124→80%，3231→100%）

- 调试工具：serial\_capture.py（断连自动重连抓日志）、extract\_coredump.py

### 迭代 M4（d0107ec）—— Light Sleep 稳定性

- SYS\_POW/CODEC\_PWR pad-hold 防断电循环；场景退出先 load 新屏再删旧屏
  （消除 act\_scr==NULL 竞态崩溃）；唤醒吞键宽限期机制

- 睡眠渲染修复：睡觉帧按 `zzz_<stage>` 查找（原裸名 zzz 不存在导致显示醒着的样子）

### 迭代 M3（28487b5 / cf72de3）—— 唤醒与熄屏

- GPIO 唤醒三键注册（左/右低电平、中键高电平）；左/右唤醒立即响应

- 关灯睡觉与 Light Sleep 冲突、屏幕反复熄灭修复

### 迭代 M2（cc1a334 / 65c9fec）—— 省电

- 熄屏后自动 Light Sleep（独立 pm\_sleep 低优先级任务，esp\_timer RTC 补跳）

- LCD sleep 指令、DFS 80\~240MHz、排泄频率公式修正

### 迭代 M1（初版功能）

- 宠物养成核心：孵化→baby→child→teen→adult(4 进化分支)→senior→死亡
  喂食/清洁/吃药/抚摸/教育(认字/算术/音乐/阅读)/玩耍(丢球/捉迷藏/节奏)
  繁育/生日/访客/流星/噩梦/商人/离家出走等特殊事件

- 状态页（属性/成长/技能日志）、设置页（时间模式/音效/时钟/相亲/重置）

- 精灵系统 tools/sprite\_gen2.py（48×48 4bpp 16 色调色板程序化生成）

- 点阵字库 tools/font\_charset.txt + gen\_font.ps1（ui\_font\_16.c）

- 存档 NVS、时间模式（真实/演示）

***

## 第 N 轮迭代需求（本轮，已实现待烧录验证）

### 需求1：电池电量显示优化 【已实现】

- power.cpp：中值滤波(5) + 滑动均值(8) 管线；迟滞（放电单调递减、单次 ≤2%，
  充电单次 ≤5%）；硬件采样 10 次平均；低电阈值 20%→15%

- ui\_main：数字百分比替换为 5 级电池图标（外框+帽子+5 格，LVGL 自绘），

  > 40% 绿 / 16\~40% 黄 / ≤15% 红+600ms 闪烁；充电显示蓝色格 + "+"

- 采样周期 30s（维持）

### 需求2：宠物入睡/起床逻辑修复 【已实现】

- 根因1修复：真实模式睡眠窗判断改用 wallclock 真实小时（注入
  set\_real\_hour\_provider）；演示模式保持宠物时钟

- 根因2修复：入睡检查扩展到 IDLE/SICK/DEPRESSED（try\_auto\_sleep lambda）

- 根因3修复：SLEEPING 增加到起床点自动醒（WakeUp v1=4，UI 播 Happy
  伸懒腰 1.5s + "早上好！"）；睡醒时若 sick\_since≥0 回 SICK（堵"睡觉治病"漏洞）

- 时间/精力关系统一：窗内到点强制入睡（时间优先）∨ energy=0 任意时刻入睡

- 可配置窗口：设置页新增"作息"项（预设 23-6/21-7/22-7/0-7 循环，
  NVS prefs 持久化，PetCore::set\_sleep\_window）；ui\_main 唤醒预测同步真实钟

### 需求3：失败动画帧颜色不一致 【已实现】

- sprite\_gen2.py：draw\_baby/draw\_scold/draw\_sick 参数化 body\_color，
  新增 scold\_<stage>/sick\_<stage> ×7 stage（精灵 39→53 帧），重新生成

- anim.cpp：Scold/Sick 动作与 SICK/DEPRESSED 常驻帧走 stage 版；
  导出 find\_stage\_sprite()/idle\_frame\_for() 给非 animator 场景

- ui\_game.cpp：失败反馈帧用 find\_stage\_sprite("scold") 颜色随阶段

### 需求4：WiFi AP 配网 + Web 设置页 【已实现】

- bsp/prefs.\*：NVS("boxpet") 存作息 + NetConfig（API Key XOR 混淆落盘）

- bsp/net\_mgr.\*：SoftAP "Pet-XXXX"(MAC后4位, 密码12345678) + esp\_http\_server
  （GET / 配置页 / GET /scan WiFi 扫描 JSON / POST /save 表单解析→NVS→重启）；
  5 分钟超时自动关；STA 异步连接 + 3 次重试 + 状态机；net\_mgr\_stop 释放内存

- ui/ui\_netcfg.\*：配网提示页（热点名/密码/192.168.4.1 指引，长按中键退出）

- 设置页新增"配网（WiFi+AI）"项入口；HTML 页含 API Key/URL/Model/
  WiFi 扫描下拉/密码

- 分区表：factory 1MB→4MB（固件 1.5MB，余量留给 TTS 迭代）

### 需求5：语音对话"聊"按钮 【已实现（文字回复版）】

- bsp/audio：I2S 全双工（TX+RX 同口）；ES8311 ADC 麦克风采集
  （audio\_record\_start/stop，40ms 块读，PSRAM 缓冲，10s 上限，停止时写 WAV 头；
  录音期间 DAC 静音防啸叫，连续读失败保护）

- bsp/llm\_client.\*：/v1/audio/transcriptions（multipart WAV 上传）+
  /v1/chat/completions（cJSON 组包/解析）；Base URL 自动补 /v1；
  TLS crt bundle 校验；超时 20s/连接 5s，失败提示"网络不好"

- ui/ui\_chat.\*：聊天场景状态机（Enter→Connecting→Idle→Listening→
  Recognizing(12KB 栈 worker)→Reply）；倾听=happy 歪头、说话=eat 嘴部开合；
  退出自动关 WiFi（按需开关省电）

- 主界面图标 9→10（"聊"，5+5 两行重排，confirm\_focus 同步）

### 需求6：仓库记忆（全局约束）【已记录并遵循】

所有麦克风/I2S/WiFi/编解码需求优先参考 `https://github.com/78/xiaozhi-esp32`。
已核对 BOX0 板级定义（与本项目 board\_config.h 一致，M5 修复的 DOUT/DIN 即来源于此）。
字体字库追加：连说话听识别试请先里配语失接址初内误浏览器打保备启早网聊

### 路径B：小智云端语音对话（自研轻量客户端）【已实现 v2】

- **背景**：esp\_xiaozhi 组件要求 IDF≥5.5（本机 5.4.4）且注册中心联网受限 → 自研：

  - `components/xz_ws`：轻量 WebSocket 客户端（RFC6455，客户端 mask、分片拼接、wss via esp-tls + crt bundle）

  - `bsp/xz_client`：小智协议引擎（URL 解析、握手头 Bearer/Device-Id/Protocol-Version、hello JSON、
    start/stop、binary 音频帧收/发；当前音频格式声明 **pcm** 16k mono 60ms）

  - `bsp/audio`：新增流式采集（60ms 帧回调上传）与流式播放（会话期独占 TX，音效暂停）

  - `ui/ui_chat`：改为小智会话面板（去掉了快捷问题功能）——连 WiFi→连小智云→中键开/停说话→
    服务器 ASR/LLM/TTS→屏显文本 + 喇叭播 TTS；长按退出；面板期间不熄屏

  - 配网页新增「小智云地址」字段，NVS 持久化

- **Token 自动获取（改用 OTA 注册，去掉手动填 Token）**：官方流程里 Token 不是手动填写、
  也不是 `/v1/device/register`（该接口为自建服务器非官方）。正确做法是固件启动后向 OTA 接口
  `POST https://<同源host>/xiaozhi/ota/`（头带 Device-Id/Client-Id/User-Agent）：

  - 设备未绑定 → 响应含 `activation.code`（6 位激活码），屏幕展示，用户在 xiaozhi.me 控制台绑定；

  - 设备已绑定 → 响应含 `websocket.url` + `websocket.token`，固件把它存 NVS（key `xz_token`）。

  - 新增 `xz_auth_begin()`（`esp_http_client` 异步 POST）解析并存 token/激活码（key `xz_code`）；
    ui\_chat 增加 Auth/AwaitBind 两阶段：无本地 token 时先注册→有激活码则上屏展示、每 8s 轮询
    是否已绑定→绑定成功后自动连接。

  - `NetConfig` 进一步精简为 WiFi + xz\_url（Token 单独持久化，无需手填）。

- **Token 说明**：xiaozhi.me 控制台绑定设备后，由 OTA 接口为本设备下发 Token。固件以
  `Authorization: Bearer <Token>` + `Device-Id: <设备MAC>` 鉴权，据此把会话关联到控制台里
  配好的 ASR(Whisper)/LLM/TTS → **设备端无需、也无法另配 LLM**。

- **v2 关键修复（详见迭代 M6）**：

  - 上行 Opus 编码采样率**固定 16kHz**（对齐官方 `AS_OPUS_ENC_CONFIG`），不再跟随服务器协商频率——
    修复服务器侧录音失真（变粗/变短/嗒嗒声）；引入本地 `components/opus` 组件并将
    `repacketizer.c` 纳入构建（`opus_repacketizer_*`/`opus_packet_pad`）；

  - 录音自动停止完善：自适应噪声底 VAD + 3s 静默结束（等同按中键），统一手动/自动停止路径；

  - 服务器主动结束会话时停录音流、进入回复显式亮"回复中…"；已清除全部调试日志。

- **待办**：稳定核实多轮连续对话的降级/重连表现；播放长 TTS 时的 i2s 写入偶发告警有待复查。
  （Opus 编解码已通过本地 opus 组件解决，不再受限于 libopus 外部下载源。）

