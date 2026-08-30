# BoxPet 需求与修改记录（Requirement & Change Log）

> 硬件：正点原子 ATK-DNESP32S3-BOX0（ESP32-S3R8，8MB PSRAM，ST7789 1.54" 240×240，
> ES8311 音频 codec，3 实体按键，锂电池 + Type-C）
> 框架：ESP-IDF v5.4 + LVGL v9 + FreeRTOS
> 参考仓库（全局约定，需求6）：`https://github.com/78/xiaozhi-esp32`
> ——麦克风/I2S 音频配置、WiFi 配网、音频编解码优先参考其 `main/boards/`、`main/audio/`、`main/protocols/`

---

## 历史修改记录（按提交/迭代倒序）

### 迭代 M8 —— 玩法精简：删除捉迷藏 + 解锁等级对齐 5 级
- **删除"捉迷藏"**（与"丢球"同为左右猜、玩法重复）：`PlayKind` 枚举重排（Ball/Rhythm/Free）、
  `kPlays` 删表项、`ui_game` 删 `Mode::HideSeek` 全程分支（标题/出题/题面/作答/配置映射）、
  升级解锁提示删"捉迷藏"项；玩菜单随之自动少一项。
- **解锁等级 8→5**（宠物实际最高 5 级，8 级门槛永远够不到）：玩-节奏（Rhythm）与教-音乐（Music）
  的 `unlock_level` 由 8 改为 5，`kUnlockMusicLv` 同步 8→5。
- 影响面：`pet_def.h`/`pet.cpp`/`ui_game.cpp`/`ui_main.cpp`；`PlayKind` 未持久化，存档无迁移问题。

### 迭代 M7 —— 分层省电：深休眠（夜间+低电量）+ 浅休眠增强 + NVS 写节流

**分层状态落地**（对照省电策略表逐项核对）：
| 状态 | 触发 | 实现 |
| :--- | :--- | :--- |
| 活跃态 | 亮屏/交互 | 240MHz DFS+WiFi 按需（原已具备） |
| 浅休眠 | 熄屏（10s 无操作）| Light Sleep（1~3mA）+ **新增**熄屏 DFS 40MHz（插 USB 无法深睡的充电空转场景）+ UI tick 10Hz→2Hz |
| 深休眠 | 夜间宠物睡眠 / 电量≤10%未充电 | **新增** `esp_deep_sleep_start`（<50μA 量级） |

- **深休眠（重启式，ESP32-S3）**：
  - 夜间（真实模式 + 宠物 SLEEPING + 处于睡眠窗口）：睡到起床点单个 RTC 闹钟；整夜不触发随机事件；
  - 低电量（≤10% 未充电）：每 30min 自醒自检（插充电即退出）+ 左右键唤醒；
  - 入睡前：`storage_save_if_changed` + 墙钟快照 + 关 PA/CODEC 电源（深睡零静态电流，唤醒=重启由 audio_init 重建）+ SYS_POW 锁存重 hold（防断电循环）；
  - 唤醒恢复：`esp_rtc_get_time_us()` 差算实际睡眠秒数（RTC_NOINIT 保存入睡时刻）→ 逐秒补跳宠物（`tick_one_second` + 同步推进墙钟，保证"到点自然醒/入睡"在补跳中正确触发）→ 按键唤醒/到起床点/已充电 → 正常启动；仍深夜/仍低电 → 续睡；
  - 深休眠唤醒**仅左右键**（中键高电平不支持 EXT1 统一唤醒模式，Light Sleep 阶段三键照常）。
- **浅休眠增强**：ST7789 增加 `0x10 Sleep In`/`0x11 Sleep Out`（原 0x28 只停扫描不断内部 DC/DC，补发后每屏省 2~5mA，唤醒 120ms 稳定后再开显示）；熄屏时 `esp_pm_configure` DFS 下限 80→40MHz（仅 USB 空转场景有效，电池态 Light Sleep 已全停）；ui_main 熄屏渲染降频 10Hz→2Hz。
- **随机事件约束**（按需求）：`check_special_events` 在 SLEEPING 状态直接短路（夜间睡觉零事件）；深休眠补跳期间经 `PetCore::set_events_enabled(false)` 关闭随机事件（防低电量 30min 自检补跳时"无 UI 凭空弹事件"）；属性衰减/恢复、生病/死亡等正常状态变化不受影响。
- **NVS 写节流**：新增 `storage_save_if_changed()`（状态 CRC 未变则跳过），5 分钟周期存档与睡眠存档均走变更检测。
- **不适用/已满足项**：SPI 总线释放（Light Sleep 由 `CONFIG_PM_SLP_DISABLE_GPIO=y` 自动隔离；深睡数字域断电）；PSRAM 自刷新（IDF 睡眠流程自动处理）；外部 32k 晶振（本板无，内部 RC 已校准）；Tickless Idle（已启用）；USB-JTAG 漏电（开发期需保留烧录通道，量产可关闭则另行配置）。
- 已知限制：深休眠期间中键不可唤醒（硬件唤醒电平限制）。入睡瞬间（SLEEPING 且真实模式+睡眠窗口+未插电）toast 自动提示"晚安～夜里按左右键唤醒"（字库补"唤"字），避免用户按中键无反应误以为死机。

### 迭代 M6 —— 小智云语音：上行 Opus 采样率根修 + 录音自动停止完善 + 去调试日志
- **服务器录音失真（变粗/变短/嗒嗒声），根因修复**：对照官方 `main/audio/` 源码确认——
  - 官方**上行编码硬编码 16000Hz**（`AS_OPUS_ENC_CONFIG` 恒定 16k/mono/AUDIO/60ms，audio_service 中
    `encoder_sample_rate_=16000`），服务器 ASR 上行一直按 16k 接收；
  - 此前我们按服务器协商频率（24kHz）上采样编码 → 服务器把 24k 帧当 16k 读 → 录音变粗/变短/嗒嗒；
  - 修复（`main/bsp/xz_client.cpp`）：`opus_codecs_for()` 编码器固定
    `opus_encoder_create(16000,1,AUDIO)` + VBR/DTX + AUTO 码率 + complexity=0（对齐 `AS_OPUS_ENC_CONFIG`）；
    解码器仍跟随服务器协商频率；`xz_encode_task()` 累积 60ms@16k=960 样本**直接编码、绝不上采样**。
- **录音自动停止完善（3s 无声停止 = 等同按中键）**：
  - `on_mic_frame` 自适应 VAD（`main/ui/ui_chat.cpp`）：首 ~300ms 把噪声底校准到环境电平
    （修旧版钉死 120 → 底噪被当人声、听完不停）；仅在非强语音帧时追踪噪声底（说话中不抬底噪防吞真声、
    停话后缓升抬高阈值）；帧退火 hot 计数滤单帧底噪尖峰；滞回清除防句内抖动；
  - 静默超时 2s→3s；统一 `stop_listening_locked()`（手动/自动共用：`listen.stop` + 停采 + 置"回复中…"），
    彻底杜绝自动停漏发录音/漏发 stop；
  - `xz_on_state`：服务器主动结束会话（state=2 开始回复 / 3 回复完毕）把本机带离 Listening 时强制停
    录音流，防空话无限上行占带宽；进入 Replying 相位显式亮出"回复中…"。
- **移除调试日志**：ST state=%d / TALK begin|end sent / 周期 up enc f=… / xz_mic 音量阈值等诊断输出。

### 迭代 M5（commit f5a3a09）—— 飞机游戏 + 金币商店 + 音频链路大修
- **飞机打害虫小游戏**：敌人整体放大一倍（12/20/28px）、飞机 3 颗心（HUD 红心）、
  被撞 3 次结束（1.2s 无敌闪烁防连撞）、射击/受伤/胜负音效、按命中数结算金币
- **金币经济系统**（game/coins.*）：各游戏得分换金币，NVS 持久化
- **商店**（ui/shop.*）：零食/高级料理/最爱食物/特效药，金币消费，冷却清除
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
- 调试工具：serial_capture.py（断连自动重连抓日志）、extract_coredump.py

### 迭代 M4（d0107ec）—— Light Sleep 稳定性
- SYS_POW/CODEC_PWR pad-hold 防断电循环；场景退出先 load 新屏再删旧屏
  （消除 act_scr==NULL 竞态崩溃）；唤醒吞键宽限期机制
- 睡眠渲染修复：睡觉帧按 `zzz_<stage>` 查找（原裸名 zzz 不存在导致显示醒着的样子）

### 迭代 M3（28487b5 / cf72de3）—— 唤醒与熄屏
- GPIO 唤醒三键注册（左/右低电平、中键高电平）；左/右唤醒立即响应
- 关灯睡觉与 Light Sleep 冲突、屏幕反复熄灭修复

### 迭代 M2（cc1a334 / 65c9fec）—— 省电
- 熄屏后自动 Light Sleep（独立 pm_sleep 低优先级任务，esp_timer RTC 补跳）
- LCD sleep 指令、DFS 80~240MHz、排泄频率公式修正

### 迭代 M1（初版功能）
- 宠物养成核心：孵化→baby→child→teen→adult(4 进化分支)→senior→死亡
  喂食/清洁/吃药/抚摸/教育(认字/算术/音乐/阅读)/玩耍(丢球/捉迷藏/节奏)
  繁育/生日/访客/流星/噩梦/商人/离家出走等特殊事件
- 状态页（属性/成长/技能日志）、设置页（时间模式/音效/时钟/相亲/重置）
- 精灵系统 tools/sprite_gen2.py（48×48 4bpp 16 色调色板程序化生成）
- 点阵字库 tools/font_charset.txt + gen_font.ps1（ui_font_16.c）
- 存档 NVS、时间模式（真实/演示）

---

## 第 N 轮迭代需求（本轮，已实现待烧录验证）

### 需求1：电池电量显示优化 【已实现】
- power.cpp：中值滤波(5) + 滑动均值(8) 管线；迟滞（放电单调递减、单次 ≤2%，
  充电单次 ≤5%）；硬件采样 10 次平均；低电阈值 20%→15%
- ui_main：数字百分比替换为 5 级电池图标（外框+帽子+5 格，LVGL 自绘），
  >40% 绿 / 16~40% 黄 / ≤15% 红+600ms 闪烁；充电显示蓝色格 + "+"
- 采样周期 30s（维持）

### 需求2：宠物入睡/起床逻辑修复 【已实现】
- 根因1修复：真实模式睡眠窗判断改用 wallclock 真实小时（注入
  set_real_hour_provider）；演示模式保持宠物时钟
- 根因2修复：入睡检查扩展到 IDLE/SICK/DEPRESSED（try_auto_sleep lambda）
- 根因3修复：SLEEPING 增加到起床点自动醒（WakeUp v1=4，UI 播 Happy
  伸懒腰 1.5s + "早上好！"）；睡醒时若 sick_since≥0 回 SICK（堵"睡觉治病"漏洞）
- 时间/精力关系统一：窗内到点强制入睡（时间优先）∨ energy=0 任意时刻入睡
- 可配置窗口：设置页新增"作息"项（预设 23-6/21-7/22-7/0-7 循环，
  NVS prefs 持久化，PetCore::set_sleep_window）；ui_main 唤醒预测同步真实钟

### 需求3：失败动画帧颜色不一致 【已实现】
- sprite_gen2.py：draw_baby/draw_scold/draw_sick 参数化 body_color，
  新增 scold_<stage>/sick_<stage> ×7 stage（精灵 39→53 帧），重新生成
- anim.cpp：Scold/Sick 动作与 SICK/DEPRESSED 常驻帧走 stage 版；
  导出 find_stage_sprite()/idle_frame_for() 给非 animator 场景
- ui_game.cpp：失败反馈帧用 find_stage_sprite("scold") 颜色随阶段

### 需求4：WiFi AP 配网 + Web 设置页 【已实现】
- bsp/prefs.*：NVS("boxpet") 存作息 + NetConfig（API Key XOR 混淆落盘）
- bsp/net_mgr.*：SoftAP "Pet-XXXX"(MAC后4位, 密码12345678) + esp_http_server
  （GET / 配置页 / GET /scan WiFi 扫描 JSON / POST /save 表单解析→NVS→重启）；
  5 分钟超时自动关；STA 异步连接 + 3 次重试 + 状态机；net_mgr_stop 释放内存
- ui/ui_netcfg.*：配网提示页（热点名/密码/192.168.4.1 指引，长按中键退出）
- 设置页新增"配网（WiFi+AI）"项入口；HTML 页含 API Key/URL/Model/
  WiFi 扫描下拉/密码
- 分区表：factory 1MB→4MB（固件 1.5MB，余量留给 TTS 迭代）

### 需求5：语音对话"聊"按钮 【已实现（文字回复版）】
- bsp/audio：I2S 全双工（TX+RX 同口）；ES8311 ADC 麦克风采集
  （audio_record_start/stop，40ms 块读，PSRAM 缓冲，10s 上限，停止时写 WAV 头；
  录音期间 DAC 静音防啸叫，连续读失败保护）
- bsp/llm_client.*：/v1/audio/transcriptions（multipart WAV 上传）+
  /v1/chat/completions（cJSON 组包/解析）；Base URL 自动补 /v1；
  TLS crt bundle 校验；超时 20s/连接 5s，失败提示"网络不好"
- ui/ui_chat.*：聊天场景状态机（Enter→Connecting→Idle→Listening→
  Recognizing(12KB 栈 worker)→Reply）；倾听=happy 歪头、说话=eat 嘴部开合；
  退出自动关 WiFi（按需开关省电）
- 主界面图标 9→10（"聊"，5+5 两行重排，confirm_focus 同步）

### 需求6：仓库记忆（全局约束）【已记录并遵循】
所有麦克风/I2S/WiFi/编解码需求优先参考 `https://github.com/78/xiaozhi-esp32`。
已核对 BOX0 板级定义（与本项目 board_config.h 一致，M5 修复的 DOUT/DIN 即来源于此）。
字体字库追加：连说话听识别试请先里配语失接址初内误浏览器打保备启早网聊

### 路径B：小智云端语音对话（自研轻量客户端）【已实现 v2】
- **背景**：esp_xiaozhi 组件要求 IDF≥5.5（本机 5.4.4）且注册中心联网受限 → 自研：
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
    ui_chat 增加 Auth/AwaitBind 两阶段：无本地 token 时先注册→有激活码则上屏展示、每 8s 轮询
    是否已绑定→绑定成功后自动连接。
  - `NetConfig` 进一步精简为 WiFi + xz_url（Token 单独持久化，无需手填）。
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
