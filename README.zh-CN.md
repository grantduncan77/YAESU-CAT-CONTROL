# YAESU CAT CONTROL 中文说明

基于 Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B 的 YAESU FT-710 外置触摸控制器项目。

本仓库包含当前 ESP-IDF/LVGL 主控固件、FT-710 CAT 通信验证程序、ESP32-DevKitC 无线桥接实验、OLED/I2C/旋钮测试程序，以及同步自 Obsidian 的项目设计、试验和开发记录。

## 项目目标

本项目当前目标是制作一个 7 寸横屏触摸控制器，用于控制 YAESU FT-710 的常用 CAT 功能，并为后续扩展到更多 YAESU 设备预留架构。

当前主控界面已经覆盖：

- VFO-A / VFO-B 双频率显示
- VFO-A / VFO-B 频率输入与写入
- A/B 输入目标选择
- A/B 工作 VFO 切换
- 模式选择
- 波段默认频率写入
- RF Power 功率控制
- DNR 开关与等级控制
- WIDTH 带宽控制
- CAT / BLE / WiFi / RX 状态栏
- WiFi 设置页面
- 联网后本地时间 / UTC 时间显示与触摸切换
- 外接 EC11 旋钮控制频率、功率、DNR、WIDTH
- 5 个 0.96 寸 SSD1306 OLED 辅助显示频率、功率、DNR、WIDTH 等状态
- 5 组“旋钮 + OLED”的硬件槽位与功能绑定抽象
- 通过右上角现有 `MENU` 字样进入配置页，配置 5 组外接旋钮/OLED 和主屏右侧 4 个功能区域
- 当前可配置功能包含 RF Power、DNR Level、WIDTH、Band、Mode、Noise Blanker、Notch、Mic Gain

当前版本刻意不实现 PTT 和自动发射控制。后续扫频/SWR 测量功能会单独加入安全限制。

## 当前硬件

主控：

- 主板：Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B
- 主控 MCU：ESP32-P4
- 无线协处理器：板载 ESP32-C6
- 显示屏：7 寸 1024 x 600 MIPI-DSI LCD
- 触摸：GT911 电容触摸
- 开发框架：ESP-IDF
- UI：LVGL 9

CAT 通信：

- 当前稳定链路：ESP32-P4 USB Host -> 外置 CH9102 USB-TTL -> FT-710 CAT-3 TTL UART
- FT-710 CAT-3 参数：38400, 8N1
- FT-710 后置 USB 直连 CP2105 路线已测试，但在当前 ESP-IDF USB Host/Hub TT 路径下不可作为稳定方案

外设：

- I2C GPIO 扩展：MCP23017
- I2C 复用器：TCA9548A
- OLED：0.96 寸 128 x 64 SSD1306 I2C OLED
- 旋钮：EC11 带按键旋转编码器

## 当前接线

ESP32-P4 辅助 I2C：

- SDA：GPIO7
- SCL：GPIO8
- I2C 速率：100 kHz

MCP23017：

- 已验证地址：`0x27`
- 旋钮输入使用 MCP23017 内部上拉

TCA9548A：

- 默认地址：`0x70`
- SSD1306 OLED 地址：`0x3C`
- OLED 屏 1：TCA9548A 通道 3
- OLED 屏 2：TCA9548A 通道 2
- OLED 屏 3：TCA9548A 通道 4
- OLED 屏 4：TCA9548A 通道 5
- OLED 屏 5：TCA9548A 通道 6

当前 EC11 旋钮定义：

- 旋钮 1：A -> PA0，B -> PA1，按键/S -> PA2
- 旋钮 2：A -> PA3，B -> PA4，按键/S -> PA5
- 旋钮 3：A -> PA6，B -> PA7，按键/S -> PB1
- 旋钮 4：A -> PB7，B -> PB6，按键/S -> PB5
- 旋钮 5：A -> PB4，B -> PB3，按键/S -> PB2

当前 5 组“旋钮 + OLED”模块定义：

- 组 1：旋钮 1 + OLED 屏 1，当前用于频率输入/显示
- 组 2：旋钮 2 + OLED 屏 2，当前用于 DNR
- 组 3：旋钮 3 + OLED 屏 3，当前用于 RF Power
- 组 4：旋钮 4 + OLED 屏 4，当前用于 WIDTH
- 组 5：旋钮 5 + OLED 屏 5，当前预留给后续可配置功能槽位；现阶段旋转时 OLED 数字递增/递减，按键清零，用于验证硬件输入和显示

后续目标是每组功能都可通过主控配置界面选择。

当前固件已经用 `control_slot_t` 表描述这 5 组硬件。每个槽位包含 MCP23017 A/B/S 引脚、对应 OLED 指针，以及频率、DNR、RF Power、WIDTH、预留测试等功能枚举。这是后续主控配置界面的第一步。

固件还新增了 `s_feature_catalog[]` 功能目录雏形，用于登记后续计划功能的 CAT 命令、显示名称、读写能力、推荐 UI 形式、是否需要确认、是否已经实现。当前 `MENU` 配置页已经按这个方向接入，可配置 5 组外接旋钮/OLED 和主屏右侧 4 个功能面板。第一批可配置功能为 RF Power、DNR Level、WIDTH、Band、Mode、Noise Blanker、Notch、Mic Gain。

## 软件环境

当前基线：

- ESP-IDF：v5.5.5
- Target：`esp32p4`
- 主控开发串口：`COM4`
- 本地 ESP-IDF 环境脚本：`D:\CAT CONTROL\idf-v5.5.5.ps1`

激活开发环境：

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"
```

构建主控固件：

```powershell
cd "D:\CAT CONTROL\ft710_controller"
idf.py -B build_v555 build
```

烧写主控固件：

```powershell
idf.py -B build_v555 -p COM4 flash
```

串口监视：

```powershell
idf.py -B build_v555 -p COM4 monitor
```

`build/` 和 ESP-IDF 自动生成的 `managed_components/` 目录不进入版本库。依赖由 ESP-IDF 根据 `idf_component.yml` 和 `dependencies.lock` 还原。

## 仓库结构

- `ft710_controller/`
  当前主控固件。包含 LVGL 触摸 UI、CH9102 USB-TTL CAT 通信、命令队列、VFO/模式/功率/DNR/WIDTH 控制、WiFi 设置页面、软键盘、时间显示、MCP23017 旋钮输入、TCA9548A 后级 SSD1306 OLED 显示。

- `ft710_ch9102_usb_probe/`
  ESP32-P4 USB Host + CH9102 USB-TTL + FT-710 CAT-3 的验证程序。

- `ft710_usb_probe/`
  FT-710 后置 USB CP2105 直连验证程序，用于记录该路线在当前 USB Host/Hub 场景下的问题。

- `ft710_ji1fgx_probe/`
  参考 JI1FGX Yaesu CP2105 ESP32-S3 项目的 Hub/CP2105 实验工程，包含测试时使用的本地 `espressif__usb` 组件副本。

- `ft710_uart_probe/`
  CAT-3 直连 UART 验证程序。

- `oled_i2c_test/`
  0.96 寸 SSD1306 OLED、TCA9548A、MCP23017、RF Power 旋钮的独立测试程序。

- `esp32_devkitc_tests/`
  ESP32-DevKitC-32E WiFi/BLE/CAT 桥接实验。当前 DevKitC 桥接阶段因硬件故障暂缓。

- `esp32_c6_hosted_slave_recovery_20260830/`
  ESP32-P4 板载 ESP32-C6 ESP-Hosted 恢复与参考工程。

- `docs/obsidian/`
  从 Obsidian 项目资料同步来的设计、测试、决策、图片和过程记录。

- `docs/code_review/`
  当前核心控制代码的逐段注释与审核说明。

## 已验证结果

已经确认：

- ESP32-P4 rev v1.3 可在 ESP-IDF v5.5.5 下正常启动
- PSRAM、MIPI-DSI 屏、EK79007 LCD、GT911 触摸、LVGL 初始化正常
- Windows 端通过 FT-710 Enhanced COM Port 可正常进行 CAT 通信
- Windows 端可接收 FT-710 USB Audio
- ESP32-P4 可识别 CH9102 USB-TTL，VID:PID 为 `1A86:55D4`
- CH9102 + FT-710 CAT-3 在 `38400 8N1` 下可读写 CAT
- 已验证 CAT 读命令：`ID;`、`FA;`、`FB;`、`MD0;`、`MD1;`
- 已验证 CAT 写命令：VFO-A/VFO-B 频率、VFO-A/VFO-B 模式、DNR 开关、DNR 等级、功率、WIDTH
- 双 VFO 约 100 ms 轮询可用
- 主控制界面已在 1024 x 600 横屏上运行
- 功率、DNR、WIDTH 支持触摸控制和旋钮控制
- 功率与 DNR 控制已改为先本地即时反馈，再由 CAT 读回修正
- 主屏右侧四个功能区现在可通过右上角现有 `MENU` 字样进入配置页重新分配，5 组外接旋钮/OLED 也可在同一页面重新分配
- 模式按钮已修正为跟随当前选择的 A/B 输入目标
- MCP23017 + EC11 旋钮在地址 `0x27` 下可稳定工作
- OLED 经 TCA9548A 通道 `3/2/4/5/6` 对应 1-5 号屏，当前程序已按 5 组槽位进行初始化
- WIDTH OLED 已按 FT-710 `SH` 表将 0-23 索引映射为实际带宽，例如 `400`、`800`、`3500`，默认显示 `DEF`
- WiFi 页面可启动 ESP-Hosted WiFi、扫描 2.4 GHz AP，并显示可滚动热点列表
- 软键盘已修正 `CLEAR`、`BACK`、`SPACE` 等控制键行为

待实机继续确认：

- 新增 Noise Blanker `NB/NL`、Notch `BC/BP`、Mic Gain `MG` 的完整 FT-710 响应和读回同步

## WiFi 说明

ESP32-P4 本身不带 WiFi 射频。本开发板通过板载 ESP32-C6 和 ESP-Hosted / `esp_wifi_remote` 实现 WiFi。

当前应用设计：

- 只需要 STA 模式连接路由器/网络
- 不需要 WiFi Host
- 不需要 AP 模式作为主要功能
- WiFi 用于主控联网、获取时间、后续读取频道列表/台站列表等数据库数据

联网后，状态栏显示时间。触摸时间区域可在本地时间和 UTC 时间之间切换。

## 主控与从机架构规划

后续通信架构会引入从机桥接模块：

- 主控：ESP32-P4 7 寸触摸屏
- 从机：ESP32-S3 3.49 寸触摸屏桥接模块
- 从机连接 FT-710 CAT-3
- 主控与从机之间优先使用 WiFi 或 BLE

链路优先级规划：

1. WiFi Bridge
2. BLE Bridge
3. USB-S3 Bridge
4. Direct USB-CAT3

也就是说，USB 是最后兜底方案。只有 WiFi 和 BLE 都不可用，或者从机故障时，才使用直接 USB-CAT-3 连接线。

从机连接 WiFi 的额外目的：

- 支持未来手机 App 配置主控/从机
- 支持配置同步
- 支持远程频道列表、台站列表、参数模板等数据

## UI 设计

主控 UI 使用暗色电台控制台风格：

- 横屏 1024 x 600
- 青色高亮
- 细边框面板
- 大字号频率显示
- 大触摸区域
- 顶部固定状态栏

主界面包含：

- 状态栏
- 双 VFO 频率显示
- 直接频率输入键盘
- RF Power 面板
- DNR 面板
- Mode 面板
- Band 面板

WiFi 页面包含：

- 左侧可滚动 AP 扫描列表
- 右侧连接状态：SSID、IP、网关、DNS
- 右侧配置输入：SSID、密码
- 连接 / 断开连接按钮
- 独立软键盘页面

## SWR 扫频功能规划

计划加入扫频测量天线驻波数据功能。

基本思路：

- 用户选择可发射频段内的起止频率
- 固定 AM 模式
- 固定低功率，例如 5W 或 10W
- 按设定步进扫频，例如 0.01 MHz
- 每个频点短暂发射并读取 SWR
- 记录频率和 SWR
- 在横坐标为频率、纵坐标为 SWR 的图表中绘制曲线

该功能涉及自动发射，必须增加明确的安全边界：

- 限制只能在允许发射的频段内运行
- 发射前二次确认
- 限制功率
- 限制每个频点发射时间
- 支持随时停止
- 失败或通信异常时立即停止

## 当前限制

- `ft710_controller/components/app_controller/app_controller.c` 目前仍集中包含 UI、CAT、WiFi、时间、状态、旋钮、OLED 等逻辑，后续需要拆分模块
- CH9102 热插拔和重连逻辑还需要加强
- EC11 方向、步进、按键短按/长按功能还需要长期实机调校
- WiFi 连接失败处理、保存凭据、自动重连流程还需要完善
- AP 扫描缓存数量有限
- FT-710 后置 USB CP2105 直连仍受当前 USB Host/Hub TT 支持限制
- Band 按钮目前写入的是项目定义的默认频率，不是 FT-710 原生 Band Stack
- DevKitC BLE/WiFi CAT 桥接实验因硬件故障暂缓
- 长时间稳定性测试和真实 RF 环境测试仍需继续

## 后续重构方向

建议将主控工程逐步拆分为：

- `cat_transport_ch9102`
- `cat_transport_ble`
- `cat_transport_wifi`
- `ft710_cat`
- `radio_state`
- `ui_main`
- `ui_wifi`
- `ui_keyboard`
- `wifi_manager`
- `time_service`
- `settings`
- `encoder_manager`
- `oled_manager`
- `diagnostics`

近期测试重点：

- CH9102 断开/重连/热插拔
- 多旋钮并发输入稳定性
- 多 OLED 刷新对主 UI 的影响
- WiFi 连接/断开/重连
- BLE/WiFi 从机桥接恢复
- 30 分钟以上连续运行
- SWR 扫频前的 CAT 读写与安全流程验证

## 文档

详细开发过程、界面原型、接线记录、测试截图、问题修正和阶段性结论位于：

- `docs/obsidian/`
- `docs/code_review/`

这些文档是从 Obsidian 项目库同步而来，用于追踪本项目从可行性验证、硬件联调、界面设计、功能实现、问题修正到后续架构规划的完整过程。
