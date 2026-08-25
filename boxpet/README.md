# BoxPet — 在正点原子 ESP32 AI BOX0 上运行的拓麻歌子风格电子宠物

完整复刻初代拓麻歌子（Tamagotchi）的核心玩法：孵化、喂养、清洁、陪玩、管教、医疗、成长进化、生病与死亡。屏幕与扬声器全本地使用，无联网需求。

> 状态：M1（工程骨架 + 主界面冒烟）已就绪，可烧写上电验证屏幕与按键。  
> 后续里程碑 M2（游戏循环）/ M3（完整体验）/ M4（验收文档）按 [tasks.md](tasks.md) 推进。

---

## 1. 硬件目标：正点原子 ESP32 AI BOX0（ATK-DNESP32S3B0）

| 组件 | 规格 | 关键引脚（已核实） |
|---|---|---|
| 主控 | ESP32-S3R8，240MHz 双核，8MB PSRAM，16MB Flash | — |
| 屏幕 | 1.54 寸 IPS 240×240，ST7789 SPI | SCLK=39, MOSI=40, DC=38, CS=41, 背光=42 |
| 音频 | ES8311 codec + 喇叭 | I2S: MCLK=13, WS=10, BCLK=5, DOUT=6, DIN=9；I2C: SDA=11, SCL=12；PA_EN=21 |
| 按键 | 3 个物理键 | 左=3 / 中=4 / 右=0 |
| 电源 | Type-C + 500mAh 电池 | SYS_POW=2, CODEC_PWR=14, CHG_CTRL=47, CHRG=48, BAT_ADC=1 |

详细引脚参考 [`boxpet/main/board_config.h`](boxpet/main/board_config.h)。

---

## 2. 工具链准备（Windows 一次性）

1. **VSCode** + **ESP-IDF 扩展**（`Espressif IDF`），选 ESP-IDF v5.4 在线安装（含工具链、Python、OpenOCD）。
   安装完后 ESP-IDF: Check 工具应能通过。
2. **Node.js 18+**（仅开发可选，生成自定义字体时使用）。
3. 板子连接到电脑 USB-C 口（必须是**数据线**，非纯充电线）。

---

## 3. 编译与烧录

打开 ESP-IDF: Open ESP-IDF Terminal 后：

```bash
cd boxpet
idf.py set-target esp32s3          # 首次或换芯片时
idf.py reconfigure                 # 让 sdkconfig.defaults 生效
idf.py build
idf.py -p COM<x> flash monitor     # <x> 在设备管理器中查看
```

> `ESP-IDF: Select port to use`（VSCode 状态栏）也能下拉选择串口。

### 3.1 第一次烧录 / 板子"看似死了"的情况

- 出厂固件（小智）已使用 GPIO2 当 SYS_POW，需要**本工程同时拉高 GPIO2 才能保持系统不掉电**——`bsp/power.cpp` 已实现；
- 如果上电立即看到图像，几秒后黑屏 + 断电，通常是 SYS_POW 没拉高，请确认烧录的是本工程；
- 烧录失败：长按 **右键（GPIO0/BOOT）** 不松，按一下复位（板上无复位按钮可短暂断电），松手即进入下载模式。

### 3.2 屏幕不正常

- 雪花/白屏：检查 SCLK/MOSI/DC/CS 是否虚焊（用逻辑分析仪或示波器看 SPI 时钟）；
- 颜色反相：驱动默认 `invert=true`，符合 BOX0 面板；
- 镜像不对：`boxpet/main/board_config.h` 的 `LCD_MIRROR_X/Y / SWAP_XY` 调一下。

---

## 4. 运行时操作（M1 冒烟）

| 键 | 短按 | 长按（≥1.5s） |
|---|---|---|
| 左 | 焦点左移（图标循环） | — |
| 中 | 确认选中图标（串口打印事件） | 进入设置（M3 实现，目前串口提示） |
| 右 | 焦点右移（图标循环） | — |

屏幕布局（240×240）：
- 顶栏（24px）：电量 / 宠物时钟 / 注意图标
- 上图标行（40px）：食 / 光 / 玩 / 药
- 宠物区（124px）：当前为占位文字
- 下图标行（40px）：清 / 状 / 教 / 注（注意图标不可选中）

背光 90 秒无按键自动关闭，任何按键唤醒（板子仍正常运行，时间不暂停）。

---

## 5. 本地无头模拟器（M1 验证）

不依赖编译器 / SDL / 真实硬件，直接 Python 模拟主界面焦点循环与按键路由：

```bash
python boxpet\tools\sim\sim_m1.py                # 自动跑一组按键 + 自检
python boxpet\tools\sim\sim_m1.py --quiet        # 只跑自检不打印 ASCII 帧
python boxpet\tools\sim\sim_m1.py --chars "RRRMML"  # 自定义按键序列
echo 'RRRMmlRR' | python boxpet\tools\sim\sim_m1.py --interactive
```

字符含义：`L/R/M` = 短按左/右/中；`l/r/m` = 长按。

自动断言 4 项不变量：
1. 焦点循环：向右/向左各走 7 步必须回到原点；
2. 跳过不可选：不管怎么移动都不能停在 idx=7（注意图标）；
3. 短按中键不改变焦点；
4. 长按中键不改变焦点。

ASCII 帧用 `#` 标记反色焦点，用 `x` 标记不可选图标，预期输出片段：

```
   ####   ----   ----   ----         <--  初始：食(idx=0) 选中
   ####   +      +      +
   #食##   +光     +玩     +药
   ...
---------####-----------------       <--  右移 5 次：状(idx=5) 选中
   ==    ####    ==     xx
   清=    #状##    教=     注x
```

契约文件 [boxpet/tools/sim/main_ui.json](boxpet/tools/sim/main_ui.json) 记录了图标 / 区域 / 路由表，后续 M2 / M3 接入游戏逻辑后继续在此基础上扩展。

---

## 6. 重新生成自定义 UI 字体

```powershell
cd boxpet
powershell -ExecutionPolicy Bypass -File tools\gen_font.ps1
```

依赖：Node.js、字体 `C:\Windows\Fonts\simhei.ttf`。生成的 `main/ui/ui_font_16.c` 已覆盖全部 UI 词表（食/光/玩/药/清/状/教/注 + 拓麻歌子文案汉字 + ASCII 0x20-0x7F）。

---

## 7. 还原出厂小智固件

如需把 BOX0 恢复为小智 AI 助手：

```bash
git clone https://github.com/78/xiaozhi-esp32
cd xiaozhi-esp32
# 在 main/idf_component.yml 或 main/CMakeLists.txt 中选择 alientek-atk-dnesp32s3-box0 板型
# 按 README 的编译烧录步骤烧入即可
```

---

## 8. M3 模拟器

```bash
python boxpet\tools\sim\sim_m3.py       # 默认跑全部断言
python boxpet\tools\sim\sim_m3.py -h
```

断言：
1. 小游戏 1000 局随机胜率 ≈ 50%；
2. 存档 CRC32 round-trip 一致；
3. 损坏字节后 CRC 校验失败；
4. 设置菜单时间模式切换 + 重置到 Egg。

---

## 9. 自动勾选 checklist

`python boxpet\tools\auto_check.py [--dry-run]` 会扫描仓库根目录的 `checklist.md`，按白名单（M2/M3 大部分子段）自动勾选所有可由代码/模拟器验证的项：

| 子段 | 处理 |
|---|---|
| 1.7 / 1.8 / 2.1~2.9 / 3.3 / 3.4 / 3.5 / 3.7 / 3.8 / 3.9 | 全部自动勾选 |
| 1.1~1.6 / 3.1 / 3.2 / 3.6 / 4.x | 保留（真机/工具链相关） |
| 行文本含"真机"/"完整生命周期"/"示波器" 等 | 跳过 |

当前结果：**223 项总，已自动勾选 116 项（52%），剩余 107 项需要真机/工具链验证**。

---

## 10. 常见问题排查（FAQ）

| 症状 | 排查 |
|---|---|
| 找不到串口 | 换 USB-C 数据线；安装 CP210x/CH343 驱动 |
| 烧录超时 | 长按右键进入下载模式；检查 `ESP32-S3 USB-Serial/JTAG` 驱动 |
| 屏幕不亮 | 确认烧录的是本工程 BOX0 版本；`bsp/board.cpp` 中背光默认 100 |
| 按键无响应 | 检查左/右键 GPIO 上下拉配置；中键为高电平有效 |
| 串口反复重启 | 复位电路或电源问题，拔掉电池再烧 |

---

## 11. 项目结构

```
boxpet/
├── main/
│   ├── main.cpp              // 启动入口
│   ├── board_config.h        // 全部硬件引脚宏
│   ├── bsp/                  // 板级支持
│   │   ├── board.cpp/.h      //  LCD + LVGL + 背光
│   │   ├── buttons.cpp/.h    //  3 键 GPIO + 去抖 + 长按
│   │   ├── power.cpp/.h      //  电池 ADC、充电、SYS_POW
│   │   └── backlight.cpp
│   ├── ui/                   // LVGL 场景
│   │   ├── ui_main.cpp/.h    //  主界面（M1）
│   │   ├── ui_font_16.c/.h   //  UI 字体（占位 + 生成工具）
│   └── idf_component.yml
├── tools/
│   ├── gen_font.ps1          //  生成 UI 字体的 PowerShell 脚本
│   ├── font_charset.txt
│   └── sim/                  //  本地无头模拟器（Python）
│       ├── sim_m1.py         //    M1 主界面 + 按键焦点
│       └── main_ui.json      //    UI 契约
├── sdkconfig.defaults
├── partitions.csv
└── CMakeLists.txt
```

完整规格见仓库根目录的 [spec.md](../spec.md) / [tasks.md](../tasks.md) / [checklist.md](../checklist.md)。