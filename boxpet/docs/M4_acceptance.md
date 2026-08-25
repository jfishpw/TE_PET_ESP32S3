# M4 真机验收手册 — BoxPet（正点原子 ATK-DNESP32S3B0）

> 本文档是 [tasks.md](../tasks.md) M4 的交付物。  
> 对应 [checklist.md](../checklist.md) §4 真机交付验收。  
> 适用环境：Windows + ESP-IDF v5.4 + ESP32-S3 板（ATK-DNESP32S3B0 / "AI BOX0"）。

---

## 1. 验收目的

1. 确认 [boxpet/](../boxpet/) 工程可在真机上完整烧录并稳定运行；
2. 验证 spec §3 全部 8 大图标功能、生命周期、存档、电源管理与硬件协同；
3. 提供故障排查脚本与数据记录表模板，便于回归与交付。

---

## 2. 验收前准备

### 2.1 硬件清单

| 序号 | 物料 | 数量 | 备注 |
|---|---|---|---|
| 1 | 正点原子 ATK-DNESP32S3B0（AI BOX0）主板 | 1 | 板上 ESP32-S3R8 + 1.54寸 ST7789 + 3键 + ES8311 + 喇叭 + 500mAh 电池 |
| 2 | USB-C 数据线 | 1 | **必须支持数据传输**（非纯充电线） |
| 3 | Windows PC（10/11） | 1 | 含 USB-A 或 USB-C 接口 |
| 4 | 万用表（可选） | 1 | 验证 GPIO 电平 / 电池电压 |
| 5 | 逻辑分析仪（可选） | 1 | 调试 SPI / I2S 时序 |
| 6 | 串口助手（PuTTY / MobaXterm） | 1 | 看 IDF monitor 输出 |

### 2.2 软件清单

| 名称 | 版本 | 安装方式 |
|---|---|---|
| ESP-IDF | **v5.4.x**（≥ 5.4.0） | VSCode 扩展 ESP-IDF 在线安装 |
| 工具链 | esp32s3 | `idf.py set-target esp32s3` 时自动下载 |
| CP210x / CH343 驱动 | 视板载 USB-Serial/JTAG | 设备管理器 → 检查"USB 串行设备" |
| Python | ≥ 3.10 | 用于无头模拟器回归 |

### 2.3 环境自检（在 PowerShell 终端）

```powershell
# ESP-IDF 是否可用
idf.py --version
# 期望：≥ 5.4

# Git 是否可用
git --version

# Python 与模拟器回归
python d:\pwg\zdyz\boxpet\tools\sim\sim_m1.py --quiet
python d:\pwg\zdyz\boxpet\tools\sim\sim_m2.py --quiet --hours 1
python d:\pwg\zdyz\boxpet\tools\sim\sim_m3.py
# 三者都应 "[OK] ALL PASS"
```

### 2.4 文档配套

- [boxpet/README.md](../boxpet/README.md) — 编译/烧录步骤
- [spec.md](../spec.md) — 游戏设计依据
- [checklist.md](../checklist.md) §4 — 验收条目

---

## 3. 硬件连接与准备

### 3.1 出厂状态检查

1. 取出 BOX0，确认无外观损伤（屏幕/按键/USB-C 口/电池仓）；
2. 第一次上电前**不要插入电池**，仅使用 USB-C 供电即可；
3. 长按 BOOT（右键=GPIO0），同时短按 RST（板上无 RST 按钮，可短断电 1s）→ 进入下载模式（屏幕无显示是正常的）。

### 3.2 USB-C 连接

| PC 端口 | 板端接口 | 说明 |
|---|---|---|
| USB-A / USB-C | USB-C（OTG） | 板载 USB-Serial/JTAG 自动枚举，无需外接串口模块 |

#### Windows 设备管理器期望

```
通用串行总线设备
  √ USB JTAG/serial debug unit (COMx)        ← ESP32-S3 内置 USB-Serial/JTAG
端口 (COM & LPT)
  √ USB Serial Device (COMx)                  ← 可能同时出现 CP210x / CH343
```

**若未识别**：
1. 换 USB-C 数据线（必须支持数据）；
2. 安装对应驱动：CP210x VCP / CH343；
3. 检查 ESP-IDF 是否带 `idf-usb-jtag`（v5.4 自带）。

### 3.3 按键位置确认

| 键 | GPIO | 板上丝印 | 按下电平 |
|---|---|---|---|
| 左 | GPIO3 | K1（左侧） | 低（L） |
| 中 | GPIO4 | K2（中间） | 高（H） |
| 右 | GPIO0 | K3（右侧，即 BOOT） | 低（L） |

> 长按中键 ≥1.5s 进入设置；死亡时长按中键孵化新蛋。  
> 右键（GPIO0/BOOT）在下载模式期间需要按住再通电。

### 3.4 屏幕 / 喇叭 / 电池

| 资源 | 默认状态 | 备注 |
|---|---|---|
| LCD | 出厂默认关屏（需要本工程拉高背光 GPIO42） | 烧录前显示原厂小智固件 / 烧录后显示 BoxPet 启动画面 |
| 喇叭 | 默认静音（ES8311 需 I2C 初始化） | 本工程未实现 I2S 播放，故无声 |
| 电池 | 出厂 50% 左右 | 接入 USB 时自动充电（CHRG=GPIO48 拉低表示充电中） |

### 3.5 静电与短路防护

1. 操作前触摸金属释放静电；
2. 不要在通电情况下插拔屏幕排线；
3. 长时间烧录建议使用带防过流的 USB 集线器。

---

## 4. ESP-IDF 编译与烧录

### 4.1 拉取 / 同步代码

```powershell
cd d:\pwg\zdyz\boxpet
git pull           # 若团队协作
```

### 4.2 打开 ESP-IDF 终端

**VSCode**：状态栏 → ESP-IDF: Open ESP-IDF Terminal（PowerShell with ESP-IDF 环境）。  
**独立终端**：执行 `C:\Users\<user>\esp\esp-idf\export.ps1`。

### 4.3 编译

```powershell
cd d:\pwg\zdyz\boxpet
idf.py set-target esp32s3          # 首次或更换芯片时执行
idf.py reconfigure                # 让 sdkconfig.defaults 生效
idf.py build
```

**预期输出**（关键行）：

```
Project boxpet has been compiled successfully.
Total sizes ... app: <X> KB, ...
```

如果出现 warning：
- `esp_lcd_panel_*` 未声明 → 检查 `idf_component.yml` 是否包含 `esp_lcd_st7789`；
- `lvgl_port_*` 未声明 → 检查 `esp_lvgl_port`；
- `esp_codec_dev` 未声明 → 检查 `espressif__esp_codec_dev` 名称（带双下划线）。

### 4.4 烧录

```powershell
# 自动识别串口
idf.py -p COM3 flash monitor       # <COMx> 改成设备管理器中看到的串口号
# 也可在 VSCode: ESP-IDF: Select port to use → 选 USB JTAG/serial debug unit
```

**若烧录失败**（典型原因与对策）：

| 现象 | 对策 |
|---|---|
| `A fatal error occurred: Failed to connect to ESP32-S3:` | 长按右键（GPIO0/BOOT）后再短断电重试，确保进入下载模式 |
| `Timed out waiting for packet header` | 1) 数据线只供电不传数据 → 换线  2) 串口被占用 → 关闭 monitor / 串口助手 |
| `Wrong boot mode detected (0x13)!` | 重复"按住右键+短按 RST"操作 |
| 烧录 99% 后报"verification failed" | Flash 接触不良；按 RST 重新进入下载模式再试 |

### 4.5 monitor 输出

烧录成功后自动进入 monitor。期望看到：

```
I (xxx) main: BoxPet boot OK (M3 active: 1Hz pet + ui ticks)
I (xxx) buttons: buttons init done
I (xxx) power: power init: pct=xx charging=x
I (xxx) storage: storage ready
```

**退出 monitor**：`Ctrl + ]`。

---

## 5. 首次上电验证（冒烟测试）

按 [checklist.md](../checklist.md) §4.1 逐项验证。每项测试前确保从 USB 拔电重启。

### 5.1 屏幕冒烟

| # | 操作 | 期望 |
|---|---|---|
| 5.1.1 | 重新插上 USB-C | 上电 1~2s 后屏幕显示破壳动画/婴儿 sprite（不在纯白/纯黑） |
| 5.1.2 | 屏幕是否 240×240 居中 | 顶栏在屏幕顶部，图标栏分布在屏幕中部与底部 |
| 5.1.3 | 颜色是否正常（不含默认绿/红色背景） | 显示拓麻歌子风格的浅绿底 + 深灰顶栏 |
| 5.1.4 | 顶栏文本 | 左：电量百分比；中：时钟 00:00 起跳；右：注意图标（默认隐藏） |

### 5.2 按键冒烟

| # | 按键 | 期望 |
|---|---|---|
| 5.2.1 | 按左键 | 焦点从"食"移到"光"或"教"（方向决定），选中反色 |
| 5.2.2 | 按右键 | 焦点右移，选中反色 |
| 5.2.3 | 焦点在"注"（idx=7）按左/右键 | 焦点跳过"注"，不能停留 |
| 5.2.4 | 按中键（焦点在"食"） | 饥饿心形 +1（看心形数）；串口日志显示 `FeedOk` |
| 5.2.5 | 长按中键 ≥1.5s | 进入设置菜单（屏幕切换到 3 项列表） |

### 5.3 屏幕睡眠与唤醒

| # | 操作 | 期望 |
|---|---|---|
| 5.3.1 | 静置 90s 无按键 | 背光熄灭（屏幕变黑），但游戏继续运行 |
| 5.3.2 | 任意按键 | 背光立即恢复 |
| 5.3.3 | 设置菜单"灯亮"+ 模拟 22:00 时间 | 30s 后背光熄灭 |

### 5.4 电源冒烟

| # | 操作 | 期望 |
|---|---|---|
| 5.4.1 | 接入 USB-C，串口查看充电标志 | `CHRG=0` → 充电状态图标亮（顶栏） |
| 5.4.2 | 拔掉 USB-C，仅电池供电 | 顶栏电池百分比显示，`CHRG=1` |
| 5.4.3 | 长按右键 + 短断电 | 系统进入下载模式（屏幕黑屏） |

---

## 6. 完整生命周期验收（演示模式 1 宠物日=1 小时）

> ⚠️ 全流程耗时 ~13 小时。建议分批执行 + 自动存档保护：每 5 分钟存档一次，断电重连自动恢复。

### 6.1 短流程冒烟（~2 小时）

| 时间窗 | 阶段 | 验证点 | 期望 |
|---|---|---|---|
| 0:00–0:01 | Egg | 屏幕 sprite 是蛋形晃动 | ✓ |
| 0:01–1:00 | Baby | 1 小时后屏幕 sprite 切换为婴儿形态 | ✓ |
| 1:00–3:00 | Child | 2 小时后切换为儿童形态 | ✓ |
| 1:00–3:00 | 衰减 | 每 12 分钟饥饿心形 -1；每 15 分钟快乐心形 -1 | ✓ |
| 0:00–3:00 | 便便 | 每 18 分钟生成 1 个便便（婴儿阶段更频繁） | ✓ |
| 0:00–3:00 | 清洁 | 选"清"+中键，便便清零 | ✓ |
| 0:00–3:00 | 管教 | 等到宠物发出"呼叫"（注意图标闪烁），按"教"+中键 | discipline +10% |
| 0:00–3:00 | 医疗 | 让宠物生病（饥饿=0 持续 30min）；按"药"+中键多次 | 1~4 剂内治愈 |
| 任意 | 玩耍 | 选"玩"+中键 → 进入"左右猜"游戏；中键开始；左/右选方向；中键确认 | 5 回合后显示胜负 |
| 任意 | 状态 | 选"状"+中键 → 3 页信息（左右翻页） | 看到当前 hunger/happy/discipline 等 |
| 任意 | 设置 | 长按中键 → 切时间模式/音效/重置 | 切换后立即生效 |

### 6.2 长时间冒烟（~13 小时）

按上表继续到 Teen → Adult → Senior → Died：

| 时间窗 | 阶段 | 关键验证 |
|---|---|---|
| 3:00–5:00 | Teen | sprite 切换为少年形态 |
| 5:00–10:00 | Adult | sprite 切换为成年形态（看 Star/Tuan/Tang） |
| 10:00–13:00 | Senior | sprite 切换为老年形态（动作变慢） |
| 任意 | 死亡 | sprite 切换为墓碑（dead_grave） |
| 死亡后 | 重置 | 长按中键 → 孵化新蛋 → 回到 Egg |

### 6.3 真实模式（1 宠物日=24 小时）— 选做

- 设置菜单切换为"真实"模式
- 重置存档开始新游戏
- 5 分钟后 Egg 孵化
- 每天 24 小时前进一个阶段

---

## 7. 关键事件观测点

打开 `idf.py -p COMx monitor`，记录以下日志（每条都应可见）：

```
[OK]   pet:              StageChanged v1=1     (Egg→Baby)
[OK]   pet:              Hungry v1=3
[OK]   pet:              Happy v1=3
[OK]   pet:              PoopAdded v1=1
[OK]   pet:              Sick v1=3
[OK]   pet:              Medicated v1=1 v2=3
[OK]   pet:              Healed
[OK]   pet:              AttentionFlash v1=8     (注意位变化时打印)
[OK]   pet:              StageChanged v1=2     (Baby→Child)
[OK]   ui_main:           StageChanged → v1=2
[OK]   ui_main:           CallForCare count=1
[OK]   pet:              AdultEvolved form=2    (Tuan)
```

按事件类型在 monitor 中检索：
```powershell
# 过滤关键事件
python -c "import re,sys;[print(l) for l in sys.stdin if re.search(r'(Stage|Sick|Healed|Died|Evolved|Hungry|Happy|Poop|Call)', l)]" < monitor.log
```

---

## 8. 故障排查表

| 症状 | 可能原因 | 对策 |
|---|---|---|
| 屏幕纯白 | SPI 接触不良 / 时钟过高 / 字节序错 | 1) 重新插排线  2) `pclk_hz` 降到 20MHz  3) 关闭 `swap_bytes` 重试 |
| 屏幕纯黑 | 背光未拉高 / 屏未 disp_on | 检查 `board.cpp::backlight_ledc_init()`；`esp_lcd_panel_disp_on_off(panel, true)` 是否调用 |
| 屏幕反色 | `invert_color` 取反 | 调整 `board.cpp` 中 `invert_color` 入参 |
| 中键无反应 | GPIO4 没拉高 / 软消抖时间过长 | 万用表量 GPIO4 按下电平；检查 `buttons.cpp::kDebounceMs=20` |
| 左/右键无反应 | GPIO3/0 没拉低 / 板上拉电阻被屏蔽 | 确认 `gpio_pullup_en` 已配置 |
| 设置菜单进不去 | 长按阈值过高 | 调低 `buttons.cpp::kLongPressMs=1500` → 800 |
| 喂饭无反应 | 饥饿=4 已满 / 生病中 | 看串口 `FeedRejected` 事件 |
| 吃药无效 | 不是生病状态 | 看串口 `FeedRejected(1)` |
| 存档加载失败 | CRC 错 / NVS 损坏 | 设置菜单"重置"；`nvs_flash_erase()` 后重烧 |
| 屏幕花屏/抖动 | LVGL 缓冲不足 / PSRAM 未启用 | 检查 sdkconfig 中 `CONFIG_SPIRAM=y`、`CONFIG_LV_MEM_SIZE_KILOBYTES` |
| 长时间运行后崩溃 | LVGL 内存泄漏 | 看串口 `Guru Meditation`，截图发 issue |
| 看门狗复位 | 长任务阻塞调度器 | 用 `esp_task_wdt_add()` 注册；超过 30s 任务切到后台线程 |

---

## 9. 数据记录表（验收日报）

> 每个验收周期打印一份，附在交付报告后。

### 9.1 基本信息

| 项 | 值 |
|---|---|
| 板卡 SN | _____ |
| 工程 commit | `git rev-parse HEAD` 输出 |
| 烧录时间 | _____ |
| 烧录人 | _____ |
| 验收时间 | _____ |
| 验收人 | _____ |

### 9.2 冒烟测试结果

| # | 测试点 | 结果 | 备注 |
|---|---|---|---|
| 5.1.1 | 上电显示 | ☐ 通过 ☐ 不通过 | |
| 5.1.2 | 屏幕方向 | ☐ 通过 ☐ 不通过 | |
| 5.1.3 | 颜色 | ☐ 通过 ☐ 不通过 | |
| 5.2.1 | 左键 | ☐ 通过 ☐ 不通过 | |
| 5.2.2 | 右键 | ☐ 通过 ☐ 不通过 | |
| 5.2.3 | 跳过"注" | ☐ 通过 ☐ 不通过 | |
| 5.2.4 | 短按中键喂饭 | ☐ 通过 ☐ 不通过 | |
| 5.2.5 | 长按中键进入设置 | ☐ 通过 ☐ 不通过 | |
| 5.3.1 | 90s 熄屏 | ☐ 通过 ☐ 不通过 | |
| 5.3.2 | 按键唤醒 | ☐ 通过 ☐ 不通过 | |
| 5.4.1 | 充电图标 | ☐ 通过 ☐ 不通过 | |
| 5.4.2 | 电池模式 | ☐ 通过 ☐ 不通过 | |

### 9.3 短流程冒烟（2 小时）

| 时间 | 阶段 | 饥饿 | 快乐 | 便便 | 体重 | 阶段切换 | 备注 |
|---|---|---|---|---|---|---|---|
| 0:01 | Egg→Baby | 4 | 4 | 0 | 30g | ✓ | |
| 0:13 | - | 3 | 4 | 1 | 31g | | 12min 饥饿 -1 |
| 0:30 | - | 2 | 3 | 2 | 33g | | 18min 便便 +1 |
| 1:00 | Baby→Child | 1 | 2 | 2 | 35g | ✓ | 1h 阶段切换 |
| 1:30 | - | 0 | 1 | 3 | 36g | | 触发饥饿态 |
| 2:00 | - | - | - | - | - | (若未治疗) | sick 触发 |
| 2:30 | 治愈 | 0 | 2 | 0 | 35g | | 给药 3 剂后 Healed |

### 9.4 故障记录

| 时间 | 现象 | 排查过程 | 解决 |
|---|---|---|---|
| | | | |
| | | | |

### 9.5 验收结论

- [ ] 所有冒烟测试通过
- [ ] 短流程 2 小时无异常
- [ ] 长流程（按需）12+ 小时无异常
- [ ] 串口日志无异常堆栈
- [ ] 存档可恢复

签字：___________ 日期：___________

---

## 10. 自动化辅助

### 10.1 模拟器回归

任何真机验收前先跑：

```powershell
python d:\pwg\zdyz\boxpet\tools\sim\sim_m1.py --quiet
python d:\pwg\zdyz\boxpet\tools\sim\sim_m2.py --quiet --hours 1
python d:\pwg\zdyz\boxpet\tools\sim\sim_m3.py
python d:\pwg\zdyz\boxpet\tools\auto_check.py --dry-run
```

期望：M1/M2/M3 三个全 `[OK]` + auto-checked 项数 ≥ 100。

### 10.2 自动勾选

```powershell
python d:\pwg\zdyz\boxpet\tools\auto_check.py
```

会自动把 [checklist.md](../checklist.md) 中可验证的项打勾。  
仅剩 ~107 项需要真机/工具链/音频实机确认。

### 10.3 串口日志收集

```powershell
# 启动 monitor 并保存到文件
idf.py -p COM3 monitor > boxpet_run.log 2>&1

# 另开终端，解析事件
Select-String -Path boxpet_run.log -Pattern "(StageChanged|AdultEvolved|Sick|Healed|Died|AttentionFlash)" | Out-File events.log
```

---

## 11. 已知限制

以下为本期实现的局限（不影响核心 8 大图标功能，但真机验收时需注意）：

- 音频接口已封装（[boxpet/main/bsp/audio.cpp](../boxpet/main/bsp/audio.cpp)），但 ES8311 codec 寄存器初始化与 I2S DMA 通道仍需真机补充；
- 死亡画面未显示享年统计（spec §3.6 提到）；
- 老年后动作变慢/睡眠变长效果未实现（spec §3.2 提及）；
- 真实模式 24 小时长跑回归未做（需 12+ 天连续运行）。

这些项目登记在 [checklist.md](../checklist.md) §"已知问题 / 待办"。

---

## 附录 A：常见命令速查

```powershell
# 设置目标芯片
idf.py set-target esp32s3

# 仅清理 build 目录
idf.py clean

# 完全清理（含 sdkconfig）
idf.py fullclean

# 烧录 + 监视
idf.py -p COM3 flash monitor

# 仅监视（不烧录）
idf.py -p COM3 monitor

# 烧录指定分区
idf.py -p COM3 flash app

# 查看分区表
idf.py partition-table
```

## 附录 B：按键丝印速查

```
[ 板子顶部（屏幕朝上，按键朝下） ]

  K1(GPIO3 左)    K2(GPIO4 中)    K3(GPIO0 右)
      [•]            [•]              [•]
```

## 附录 C：屏幕排线方向

```
[ST7789 1.54寸面板]

  PIN 1 ─→ 连接到板子 FPC 座 PIN 1
  PIN 2 ... 24
  红色线 → 板子 FPC 座丝印"1"端
```

## 附录 D：交付物索引

| 文件 | 用途 |
|---|---|
| [boxpet/](../boxpet/) | 完整工程 |
| [boxpet/README.md](../boxpet/README.md) | 编译/烧录速查 |
| [boxpet/tools/](../boxpet/tools/) | 精灵、字体、模拟器、自动勾选脚本 |
| [spec.md](../spec.md) | 设计规格 |
| [checklist.md](../checklist.md) | 验收清单（含 auto-check 自动勾选结果） |
| [tasks.md](../tasks.md) | 任务分解 |
| **本文件 [docs/M4_acceptance.md](M4_acceptance.md)** | 真机验收手册 |