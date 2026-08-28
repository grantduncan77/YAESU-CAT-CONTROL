---
title: WiFi 设置界面与软键盘修正记录
tags:
  - FT-710
  - ESP32-P4
  - WiFi
  - LVGL
  - 实机测试
created: 2026-08-23
---

# WiFi 设置界面与软键盘修正记录

## 背景

本阶段在正式 FT-710 控制界面基础上，继续实现 WiFi 设置页，并修正用户实机反馈的软键盘输入问题。

用户明确要求：

- 只需要 ESP32-P4 通过 WiFi 连接到路由器和网络，不需要做 WiFi host 或 AP 主机功能。
- 左侧 WiFi 扫描热点列表要可以上下滚动，以便选择更多热点。
- 右侧界面不需要复杂设置：
  - 上半部分显示现有连接状态。
  - 显示 SSID、IP 地址、网关地址、DNS 地址。
  - 下半部分只保留 SSID 输入框、密码输入框、连接按钮、断开连接按钮。
- 点击左侧扫描到的热点时，SSID 自动带入右侧输入框。
- 点击 SSID 或 Password 输入框时弹出软键盘。
- 状态栏中的 WiFi 状态按钮进入 WiFi 设置页面。
- 状态栏显示时间，点击时间区域切换本地时间和 UTC 时间。
- 频率控制页面和 WiFi 设置页面都显示时间。

## 重要硬件结论

目标开发板为 Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B。

ESP32-P4 本身没有 2.4G/5G WiFi 射频，板载 ESP32-C6 作为无线协处理器，通过 ESP-Hosted / esp_wifi_remote 提供 WiFi 能力。

当前工程中使用的无线能力是：

- 模式：`WIFI_MODE_STA`
- 目的：连接 2.4 GHz 路由器 / AP
- 不做 AP 热点
- 不做 WiFi host 主机功能

根据 Waveshare 官方 ESP-IDF `05_wifistation` 示例要求，该板 WiFi Station 示例需要 2.4 GHz WiFi 接入点。当前阶段按 2.4 GHz 路由器连接路线实现。

## 代码位置

正式工程：

```text
D:\CAT CONTROL\ft710_controller
```

主要修改文件：

```text
D:\CAT CONTROL\ft710_controller\components\app_controller\app_controller.c
D:\CAT CONTROL\ft710_controller\components\app_controller\CMakeLists.txt
D:\CAT CONTROL\ft710_controller\main\idf_component.yml
```

使用 ESP-IDF 环境：

```text
D:\CAT CONTROL\idf-v5.5.5.ps1
```

开发板串口：

```text
COM4
```

## 依赖与组件

`main/idf_component.yml` 中加入板载 C6 Hosted WiFi 所需组件：

```yaml
dependencies:
  espressif/esp_wifi_remote:
    version: "==1.2.5"
  espressif/esp_hosted:
    version: "1.4.*"
```

`components/app_controller/CMakeLists.txt` 中为应用控制组件加入 WiFi/网络相关依赖：

```text
esp_event
esp_netif
esp_timer
esp_wifi
nvs_flash
```

说明：

- `esp_hosted` / `esp_wifi_remote` 是 ESP32-P4 使用板载 ESP32-C6 WiFi 的必要桥接组件。
- 这不是把设备做成 AP，也不是用户不需要的 WiFi host 功能。
- 当前应用层只使用 Station 模式连接路由器。

## WiFi 设置页面实现

新增页面状态：

```c
SCREEN_WIFI
SCREEN_WIFI_KEYBOARD
```

顶部状态栏：

- `FT-710 CONTROL`
- 时间显示区域
- `CAT ONLINE`
- `BT OFF`
- `WIFI ...`
- `RX`
- `MENU` 或 `BACK`

交互：

- 在主控制页面点击状态栏 WiFi 状态按钮进入 WiFi 设置页面。
- WiFi 设置页点击 `BACK` 返回主控制页面。
- 点击时间区域切换本地时间 / UTC 时间。

左侧扫描列表：

- 标题：`WIFI` / `SCAN RESULT`
- 使用 LVGL 可滚动容器。
- 扫描结果按 RSSI 列出。
- 每条显示：
  - SSID
  - RSSI
  - 认证方式
  - Channel
- 点击某个热点会把该 SSID 写入右侧 SSID 输入框。

右侧连接状态区：

- `SSID`
- `IP ADDRESS`
- `GATEWAY`
- `DNS`
- `STATUS`

右侧配置区：

- SSID 输入框
- Password 输入框
- `CONNECT`
- `DISCONNECT`

## WiFi 界面设计过程

### 初始设计目标

WiFi 页面需要继承主控制界面的视觉语言：

- 深色背景。
- 青色高亮。
- 细线框面板。
- 方角/小圆角触摸按钮。
- 标题栏和状态胶囊沿用 FT-710 主控页面样式。
- 页面必须是 1024 × 600 横屏，而不是竖屏或网页式滚动页面。

最初的 WiFi 配置界面按“设置面板”思路设计：

- 顶部保留状态栏。
- 左侧为扫描结果。
- 右侧上方为连接信息。
- 右侧下方为输入和网络模式设置。
- 下方提供 `STATION`、`AP MODE`、`DHCP`、`STATIC IP`、`OPEN KEYBOARD` 等按钮。

### 首版图形问题

首版确认图中暴露出两个主要问题：

1. 右侧连接信息区的 `RSSI`、`MODE` 等信息块下边界与下方 `CONFIG` 区域发生视觉重叠。
2. 右下角 `DHCP`、`STATIC IP` 和 `OPEN KEYBOARD` 等按钮互相压住，触摸区域和文字都不清晰。

用户反馈：

```text
越界和重叠
```

因此设计方向从“信息完整的设置面板”调整为“只保留当前阶段真正需要的连接配置”。

### 软键盘打开状态设计

随后生成软键盘打开状态的确认图。

该版本仍存在底部按钮重叠问题：

- `OPEN KEYBOARD` 与周围按钮距离过近。
- `STATIC IP` 与键盘区域/按钮区域发生挤压。
- 右侧设置页面既要显示连接信息，又要显示配置输入，又要显示软键盘，在 1024 × 600 屏幕内过于拥挤。

用户反馈：

```text
重叠
```

据此确定后续原则：

- 软键盘打开时应切换到独立键盘页面，而不是在 WiFi 设置页底部原地弹出。
- WiFi 设置主页面只负责扫描、选择、输入框入口、连接/断开。
- 键盘页面只负责编辑当前字段，减少同时显示的信息量。

### 功能范围收敛

用户进一步明确：

```text
错了，不需要wifi host，只需要可以通过wifi连接到路由器和网络联通即可
```

因此删除或暂不实现以下界面复杂项：

- AP Mode。
- DHCP/Static IP 切换页。
- WiFi host 相关概念。
- 复杂网络参数手动编辑。

保留的正式第一版范围：

- WiFi STA 连接路由器。
- 扫描热点。
- 选择热点。
- 输入 SSID。
- 输入密码。
- 连接。
- 断开。
- 显示连接后的 SSID、IP、Gateway、DNS、状态。

### 最终 WiFi 页面布局原则

最终 WiFi 页面按左右两栏实现：

左栏：

- 占屏幕左侧约 40% 宽度。
- 固定标题 `WIFI` / `SCAN RESULT`。
- 中间为可滚动 AP 列表。
- 底部为 `SCAN` 和必要操作区。

右栏：

- 占屏幕右侧约 60% 宽度。
- 上半部分为当前连接状态，只显示实际有用信息。
- 下半部分为连接配置，只保留 SSID、Password、CONNECT、DISCONNECT。

该布局的原因：

- 左侧热点列表天然需要纵向滚动。
- 右侧连接状态和输入框应保持稳定，避免因扫描结果数量变化而跳动。
- 1024 × 600 屏幕高度有限，应避免多层嵌套卡片和过多模式按钮。
- 触摸屏上优先保证按钮尺寸、边界清楚、文字不压线。

### 左侧滚动列表设计

用户要求：

```text
左侧wifi 扫描的热点/路由器部分应该可以上下滚动以选择更多
```

因此左侧 AP 列表使用 LVGL 可滚动容器：

- 设置垂直滚动方向。
- 开启自动滚动条。
- 每个 AP 使用固定高度按钮。
- AP 条目之间保留固定间距。

这样在扫描结果较多时，不再压缩所有热点到一个屏幕内，而是保持每个热点有足够触摸面积。

### 右侧简化设计

用户要求：

```text
界面右侧也无需现在这么麻烦，上半部分现有连接状态，显示SSID，IP地址，网关地址，DNS地址即可。
特别是下半部分，只要SSID输入框，密码输入框，下面按钮就是连接，断开连接两个即可
```

最终右侧设计：

连接状态区：

- SSID
- IP ADDRESS
- GATEWAY
- DNS
- STATUS

配置区：

- SSID 输入框
- PASSWORD 输入框
- CONNECT
- DISCONNECT

交互规则：

- 点击左侧热点：复制 SSID 到右侧 SSID 输入框。
- 点击 SSID 输入框：进入软键盘页面，编辑 SSID。
- 点击 Password 输入框：进入软键盘页面，编辑密码。
- 点击 CONNECT：以当前 SSID/Password 连接路由器。
- 点击 DISCONNECT：断开当前 WiFi。

### 软键盘设计原则

软键盘采用单独页面，而不是嵌入 WiFi 设置页。

原因：

- 7 英寸 1024 × 600 屏幕虽然横向宽，但纵向高度只有 600。
- WiFi 页如果同时放状态区、配置区、键盘区，会很容易重叠。
- 单独键盘页面可以给按键更大触摸面积，减少误触。

键盘页面应显示：

- 当前正在编辑的字段：SSID 或 PASSWORD。
- 当前输入内容，密码字段以掩码显示。
- 字母、数字、常用符号。
- `SHIFT`
- `CLEAR`
- `SPACE`
- `BACK`
- `CANCEL`
- `DONE`

底部控制键必须语义明确：

- `CLEAR`：清空当前字段。
- `BACK`：回删一个字符。
- `DONE`：确认并返回 WiFi 设置页。
- `CANCEL`：取消键盘页并返回。

## 时间显示设计过程

### 初始需求

用户提出：

```text
连接wifi后获取时间，并在状态标题栏中显示，要求点击一次显示本地时间，再点击一次显示UTC时间
```

设计目标：

- WiFi 联网后使用 SNTP 获取时间。
- 在状态栏显示当前时间。
- 用户可在本地时间和 UTC 时间之间切换。
- 不占用主控制区面积。

### 从按钮到状态栏文本

最初可以把时间做成一个按钮，但用户进一步明确：

```text
时间显示无需做成按钮，直接显示在 CONTROL 和 现在按钮之间即可
```

因此设计改为：

- 时间本身是状态栏中的文本区域。
- 视觉上不做明显按钮样式。
- 位置放在 `FT-710 CONTROL` 和后续状态按钮之间。
- 仍然保留触摸事件，用透明触摸区域响应点击。

这样既满足可点击切换，又不破坏状态栏的仪表式观感。

### 点击区域设计

用户继续明确：

```text
触摸或者点击这个时间区域切换UTC和本地时间显示
```

最终实现：

- 时间文本下方或同区域放置透明触摸区域。
- 点击该区域切换 `local / UTC` 状态。
- 切换后刷新当前页面状态栏。

该设计的好处：

- 视觉上只是一个时间显示，不像控制按钮。
- 触摸目标可以比文字本身更宽，更适合电容触摸屏。
- 后续可以继续在状态栏加入设置、菜单、网络状态，不打乱主 UI。

### 字体大小调整

用户反馈：

```text
字体加大，和FT-710 Control 一样大
```

因此时间显示字体调整为与 `FT-710 CONTROL` 接近或一致的标题级字体，而不是小号状态字。

设计原因：

- 时间属于高频查看信息。
- 顶部状态栏空间足够。
- 与产品标题同级显示，可以在车台/电台操作距离下更容易辨认。

### 页面一致性

用户要求：

```text
频率控制页面也是一样的要显示时间
```

因此时间显示不是 WiFi 页专属，而是抽象为状态栏公共元素：

- 主频率控制页面显示时间。
- WiFi 设置页面显示时间。
- 点击时间区域的切换逻辑在两个页面中一致。

最终规则：

- 未获取网络时间：显示 `--:--:--`。
- 已获取本地时间：显示本地时间。
- 点击后切换为 UTC 时间。
- 再点击切回本地时间。

## WiFi 功能实现

已实现功能：

- 初始化 NVS。
- 初始化 esp_netif。
- 初始化 esp_event。
- 初始化 esp_wifi_remote / ESP-Hosted WiFi。
- 创建默认 WiFi STA netif。
- 设置 `WIFI_MODE_STA`。
- 扫描周边 2.4 GHz AP。
- 保存扫描结果到本地缓存。
- 左侧扫描列表滚动显示扫描结果。
- 点击扫描结果自动填入 SSID。
- 输入 SSID / Password 后连接路由器。
- 获取 IP 事件后记录：
  - IP 地址
  - Gateway
  - DNS
- 连接后启动 SNTP 获取网络时间。
- 保存 SSID / Password 到 NVS。
- 支持断开连接。

## 时间显示实现

时间显示规则：

- 连接 WiFi 并获取 SNTP 时间前显示 `--:--:--`。
- 获取网络时间后，状态栏显示当前时间。
- 点击时间区域切换显示模式：
  - 本地时间
  - UTC 时间
- 频率控制页面和 WiFi 设置页面共用同一状态栏时间显示逻辑。

## 实机日志

编译与烧写：

```text
idf.py build
idf.py -p COM4 flash
```

启动日志确认：

```text
I app_init: Project name:     ft710_controller
I app_init: ESP-IDF:          v5.5.5
I ESP32_P4_EV: Display initialized
I ESP32_P4_EV: Touch 0x5d found
I esp_lvgl:adapter: LVGL task started successfully
I ft710_controller: CH9102 opened, FT-710 control UI running
```

ESP-Hosted WiFi 启动日志：

```text
I transport: Received INIT event from ESP32 peripheral
I transport: Features supported are:
I transport:      * WLAN
I transport: Base transport is set-up
I ft710_controller: ESP-Hosted WiFi ready, SSID='Grant-2G'
```

WiFi 扫描日志示例：

```text
I ft710_controller: WiFi AP[0]: ssid='KE' rssi=-55 channel=2 auth=WPA2
I ft710_controller: WiFi AP[1]: ssid='Xiaomi_DH' rssi=-58 channel=1 auth=WPA2
I ft710_controller: WiFi AP[2]: ssid='ChinaNet-2.4G-203' rssi=-64 channel=11 auth=WPA/WPA2
I ft710_controller: WiFi AP[3]: ssid='303' rssi=-65 channel=6 auth=WPA2
I ft710_controller: WiFi scan complete: 16 AP shown, 18 AP total
```

说明：

- 当前缓存显示最多 16 个 AP。
- 扫描实际可能发现更多 AP，例如日志中出现 `18 AP total`。
- 左侧列表已设为可滚动，以便在 16 个缓存结果中上下选择。

## 软键盘问题

用户实机反馈：

```text
软键盘缺少回删键，clr 不是清除所有，是c  bs也不是回删 是b
```

问题原因：

软键盘事件处理中，`K_CLR`、`K_BS`、`K_SPACE` 的 ID 都以 `K_` 开头。

原逻辑先判断通用字符输入：

```c
strncmp(id, "K_", 2) == 0
```

因此：

- `K_CLR` 被当成普通字符，输入 `C`
- `K_BS` 被当成普通字符，输入 `B`
- `K_SPACE` 也可能被当成普通字符，输入 `S`

这不是 LVGL 键盘显示问题，而是事件分发顺序问题。

## 软键盘修正

修正策略：

先处理控制键，再处理普通字符键。

修正后的逻辑顺序：

1. `K_SHIFT`
2. `K_SPACE`
3. `K_BS`
4. `K_CLR`
5. 普通 `K_` 字符输入

同时将底部按钮文字调整为更清晰：

- `CLR` 改为 `CLEAR`
- `BS` 改为 `BACK`

修正后行为：

- `CLEAR` 清空当前输入框全部内容。
- `BACK` 删除当前输入框最后一个字符。
- `SPACE` 输入空格。
- 普通字母、数字和符号按键继续正常输入。

## 修正后验证

重新编译：

```text
idf.py build
```

结果：

```text
Project build complete.
ft710_controller.bin binary size 0x187ec0 bytes.
Smallest app partition is 0x800000 bytes.
0x678140 bytes (81%) free.
```

重新烧写：

```text
idf.py -p COM4 flash
```

结果：

```text
Serial port COM4
Chip is ESP32-P4 (revision v1.3)
MAC: e8:f6:0a:e3:59:c6
Hash of data verified.
Hard resetting via RTS pin...
Done
```

启动验证：

```text
I ESP32_P4_EV: Display initialized
I ESP32_P4_EV: Touch 0x5d found
I esp_lvgl:adapter: LVGL task started successfully
I ft710_controller: CH9102 opened, FT-710 control UI running
I ft710_controller: ESP-Hosted WiFi ready, SSID='Grant-2G'
I ft710_controller: WiFi scan complete: 16 AP shown, 18 AP total
```

结论：

- 编译通过。
- 烧写成功。
- 触摸屏初始化成功。
- CH9102/CAT 链路启动成功。
- ESP-Hosted WiFi 启动成功。
- WiFi 扫描成功。
- 软键盘控制键逻辑已完成修复并进入实机固件。

## 当前仍需人工确认

用户下一步应在实机屏幕上确认：

1. 点击 SSID 输入框后软键盘弹出正常。
2. 点击 Password 输入框后软键盘弹出正常。
3. `CLEAR` 是否清空当前输入框。
4. `BACK` 是否删除最后一个字符。
5. `SPACE` 是否输入空格。
6. 左侧 WiFi 热点列表是否可以上下滚动。
7. 点击左侧热点是否能自动带入右侧 SSID。
8. 输入密码后 `CONNECT` 是否能连接路由器并显示 IP/Gateway/DNS。
9. 连接后状态栏时间是否能显示。
10. 点击时间区域是否能在本地时间和 UTC 时间之间切换。

## 当前限制

1. AP 结果缓存当前最多 16 个，扫描发现更多热点时只显示前 16 个。
2. WiFi 页面仍在 `app_controller.c` 内，与主控制 UI、CAT 状态机混在一起，后续需要拆分。
3. 软键盘是项目自定义键盘，不是系统输入法，后续可继续增加更多符号。
4. 目前只按 2.4 GHz 路由器连接路线实现。
5. SNTP 时间依赖网络连通，未联网时状态栏仍显示 `--:--:--`。
6. Monitor 中偶发 `Failed to acquire LVGL lock` 日志，需要后续观察是否影响 UI 刷新；当前启动、触摸、CAT、WiFi 扫描均正常。

## 建议下一步

1. 用户实机确认软键盘 `CLEAR/BACK/SPACE` 行为。
2. 用户实机连接 `Grant-2G` 或其他 2.4 GHz 路由器，确认 IP/Gateway/DNS 和时间显示。
3. 如 WiFi 连接正常，将 SSID/密码输入、保存、自动重连流程完善为正式设置模块。
4. 将 `app_controller.c` 拆分为：
   - `ui_main`
   - `ui_wifi`
   - `ui_keyboard`
   - `wifi_manager`
   - `time_service`
   - `cat_transport`
   - `ft710_cat`
5. 对 WiFi/CAT 同时运行做长稳测试，观察 LVGL lock 日志是否需要进一步处理。
