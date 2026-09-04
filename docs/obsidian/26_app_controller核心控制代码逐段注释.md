---
title: app_controller.c 核心控制代码逐段注释
tags:
  - FT-710
  - ESP32-P4
  - CodeReview
  - app_controller
created: 2026-09-02
source: D:\CAT CONTROL\ft710_controller\components\app_controller\app_controller.c
---

# app_controller.c 核心控制代码逐段注释

## 1. 核心文件结论

当前项目的核心控制代码文件是：

```text
D:\CAT CONTROL\ft710_controller\components\app_controller\app_controller.c
```

公开入口头文件是：

```text
D:\CAT CONTROL\ft710_controller\components\app_controller\include\app_controller.h
```

该头文件目前只暴露一个启动函数：

```c
void app_controller_start(void);
```

也就是说，固件启动后，`main/app_main.c` 调用 `app_controller_start()`，实际控制逻辑几乎全部进入 `app_controller.c`。

## 2. 文件总体职责

`app_controller.c` 目前是一个“大总管”文件，集中承担：

- ESP32-P4 7 寸 LVGL 主界面创建。
- WiFi 设置界面与软键盘。
- FT-710 CAT 命令发送、查询、响应解析。
- USB Host CDC ACM 连接外置 CH9102 / USB-CAT3。
- 电台状态轮询。
- 触摸按钮事件分发。
- MCP23017 外接旋钮读取。
- TCA9548A 多路复用。
- SSD1306 辅助 OLED 初始化与绘图。
- RF Power / DNR / WIDTH / Frequency 旋钮逻辑。
- SNTP 时间显示、本地/UTC 切换。
- WiFi STA 扫描、连接、断开、IP/Gateway/DNS 状态显示。
- WiFi Bridge 探测逻辑的早期实验代码。

这说明当前文件是项目 MVP 阶段快速集成的产物，功能已经跑通，但后续为了支持主控/从机、多链路、五组旋钮/OLED、YAESU 多机型，应该逐步拆分。

## 3. 推荐拆分方向

建议从 `app_controller.c` 拆出：

| 当前职责 | 建议模块 |
|---|---|
| CAT 命令拼接与响应解析 | `radio_driver/yaesu_ft710_driver.c` |
| USB Host CDC ACM CAT 收发 | `cat_transport/usb_direct_cat3.c` |
| WiFi Bridge 探测/连接 | `cat_transport/wifi_bridge.c` |
| 未来 BLE Bridge | `cat_transport/ble_bridge.c` |
| 链路优先级与健康检查 | `cat_link_manager.c` |
| MCP23017 旋钮输入 | `hardware_slots/mcp23017_encoder.c` |
| TCA9548A 与 SSD1306 OLED | `hardware_slots/aux_oled.c` |
| RF Power/DNR/WIDTH/Mode/Band 功能 | `control_features/*.c` |
| WiFi STA、SNTP、扫描 | `network_client/wifi_manager.c` |
| WiFi 设置页、主控设置页 | `settings_ui/*.c` |
| 主界面布局 | `main_ui/*.c` 或保留在 `app_controller` |

## 4. 依赖与包含区：第 1-40 行

| 行号 | 说明 |
|---:|---|
| 1-2 | 引入 `app_controller.h` 和 `radio_state.h`，这是本组件入口和电台状态模型。 |
| 4-7 | C 标准库：断言、布尔/整数、字符串。 |
| 9-35 | ESP-IDF、USB Host、CDC ACM、WiFi、SNTP、socket、LVGL、板级 BSP 依赖。 |
| 37-39 | `lwip` 相关头文件，用于 IPv4、DNS、UDP/TCP 网络功能。 |

审核点：

- 当前文件依赖过多，反映职责过宽。
- 后续拆分后，UI 文件不应直接包含 USB/WiFi socket 细节。
- CAT transport 文件不应包含 LVGL。

## 5. 类型定义区：第 41-110 行

| 行号 | 名称 | 说明 |
|---:|---|---|
| 41 | `TAG` | ESP 日志标签，统一输出为 `ft710_controller`。 |
| 43-54 | `app_cmd_type_t` | UI/旋钮向 CAT 任务发送的命令类型。包括设置频率、功率、DNR、WIDTH、模式、VFO、Band。 |
| 56-63 | `app_cmd_t` | 命令队列消息体。包含命令类型、目标 VFO、数值、模式、频率、文本标签。 |
| 65-67 | `button_ctx_t` | LVGL 按钮事件上下文。主要保存按钮 ID 字符串。 |
| 69-73 | `app_screen_t` | 当前屏幕枚举：主界面、WiFi 页面、WiFi 软键盘。 |
| 75-78 | `time_view_t` | 时间显示模式：本地时间或 UTC。 |
| 80-87 | `wifi_state_t` | WiFi 状态机：关闭、就绪、扫描、连接中、在线、失败。 |
| 89-92 | `wifi_edit_field_t` | WiFi 输入目标：SSID 或 Password。 |
| 94-98 | `wifi_cmd_type_t` | WiFi 管理任务命令：扫描、连接、断开。 |
| 100-102 | `wifi_cmd_t` | WiFi 命令队列消息。 |
| 104-109 | `wifi_ap_item_t` | 扫描出的 WiFi 热点条目：SSID、RSSI、信道、加密方式。 |

审核点：

- `app_cmd_type_t` 已经是一个功能调度核心，后续可迁移为 `control_features` 的统一 action。
- WiFi 状态和 CAT 状态在同一个文件，后续应分离。
- `app_cmd_t` 同时承载 UI、旋钮和 CAT 命令，后续建议变成语义化 radio action。

## 6. 宏定义区：第 111-141 行

| 行号 | 宏 | 说明 |
|---:|---|---|
| 111 | `WIFI_AP_MAX` | WiFi 扫描结果最多显示 16 个。 |
| 112-113 | `WIFI_DEFAULT_SSID/PASSWORD` | 测试默认 WiFi：`KE` / `qazwsxedc`。后续产品版应移到 NVS 默认配置或取消硬编码。 |
| 114-115 | `WIFI_BRIDGE_TCP_PORT/DISCOVERY_PORT` | WiFi CAT Bridge 早期实验端口。 |
| 116 | `ENCODER_STEP_HZ` | 频率旋钮每格默认步进 1000 Hz。 |
| 117-124 | MCP23017 地址和寄存器 | I/O 扩展芯片地址范围与寄存器定义。 |
| 125 | `MCP23017_FREQ_ENCODER_MASK` | 频率旋钮 PA0/PA1/PA2。 |
| 126 | `MCP23017_DNR_ENCODER_MASK` | DNR 旋钮 PA3/PA4/PA5。 |
| 127-128 | 功率旋钮 mask | 功率旋钮 PA6/PA7/PB0。 |
| 129 | WIDTH 旋钮 mask | WIDTH 旋钮 PB5/PB6/PB7。 |
| 130 | `AUX_I2C_HZ` | 外设 I2C 速率 100 kHz，解决清理后旋钮/OLED不稳定问题。 |
| 131-135 | OLED 尺寸与地址 | SSD1306 128x64，地址 0x3C/0x3D。 |
| 136-139 | TCA9548A 和 OLED 通道 | Mux 地址 0x70，WIDTH=2，Power=3，DNR=4。 |
| 140-141 | WIDTH 范围 | FT-710 `SH` WIDTH index 0-23。 |

审核点：

- 五组旋钮/OLED 后，这些固定宏应改为配置表。
- WiFi 密码不宜长期硬编码在源码。
- Bridge 端口应放入设置页和 NVS。

## 7. 辅助 OLED 状态结构：第 143-151 行

| 行号 | 字段 | 说明 |
|---:|---|---|
| 144 | `dev` | SSD1306 的 I2C device handle。 |
| 145 | `channel` | TCA9548A 复用通道。 |
| 146 | `ready` | OLED 是否初始化成功。 |
| 147 | `last_text` | 上次显示文本，用于避免重复刷新。 |
| 148 | `last_numeric` | 上次数值缓存。 |
| 149 | `last_flag` | 上次布尔状态缓存。 |
| 150 | `frame` | 128x64 单色帧缓冲。 |

审核点：

- 当前 `aux_oled_t` 已经接近可复用结构。
- 五块 OLED 时，应变成 `aux_oled_t oleds[5]`，并由 `hardware_slots` 管理。

## 8. 全局状态区：第 153-210 行

| 行号 | 变量 | 说明 |
|---:|---|---|
| 153 | `s_state` | 全局电台状态缓存，类型为 `radio_state_t`。 |
| 154 | `s_rx_queue` | USB CDC 接收回调向读取逻辑投递原始字节。 |
| 155 | `s_wifi_cmd_queue` | WiFi 管理任务命令队列。 |
| 156 | `s_wifi_event_group` | WiFi 连接/失败事件组。 |
| 157 | `s_cmd_queue` | UI/旋钮到 CAT 任务的控制命令队列。 |
| 158 | `s_lvgl_mutex` | LVGL 操作互斥。 |
| 159 | `s_i2c_mutex` | I2C 总线互斥，保护 MCP23017/TCA/OLED。 |
| 160 | `s_input_target_vfo` | 当前直接输入频率目标 VFO，A 或 B。 |
| 161 | `s_power_step` | 功率旋钮当前步进，初始 2W。 |
| 162 | `s_power_encoder_selecting_step` | 功率旋钮是否处于步进选择模式。 |
| 163 | `s_screen` | 当前 LVGL 页面。 |
| 164 | `s_time_view` | 状态栏时间显示本地/UTC。 |
| 165-181 | WiFi 状态/输入/扫描缓存 | 保存 WiFi STA 当前状态、SSID、密码/IP文本、扫描列表。 |
| 182 | `s_i2c_bus` | 外设 I2C bus handle。 |
| 183 | `s_mcp23017_dev` | MCP23017 device handle。 |
| 184-186 | `s_power_oled/s_dnr_oled/s_width_oled` | 三块已接入 OLED 的对象。 |
| 188-210 | LVGL 对象指针 | 状态栏、WiFi页、频率、模式、功率、DNR、Band 等控件指针。 |

审核点：

- 全局变量很多，后续要拆成 `app_context_t`、`wifi_context_t`、`hardware_context_t`。
- `s_state` 是当前跨 UI/CAT/OLED 的中心状态，后续可保留为共享状态模型。
- LVGL 指针和 CAT 队列放在同文件内，不利于测试。

## 9. 主题颜色与前置声明：第 212-225 行

| 行号 | 说明 |
|---:|---|
| 212-219 | 定义深色科技风 UI 颜色：背景、面板、按钮、激活按钮、边框、文字、弱化文字、青色强调。 |
| 221-225 | 前置声明 UI 更新、主 UI、WiFi UI、软键盘 UI 创建函数。 |

审核点：

- 颜色可以后续迁移到 `ui_theme.c/h`。
- 前置声明只覆盖 UI，其他函数靠源码顺序组织。

## 10. USB CDC 与 CAT 基础收发：第 227-311 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 227-235 | `rx_cb` | USB CDC 接收回调，把收到的字节逐个投递到 `s_rx_queue`。 |
| 237-246 | `event_cb` | USB CDC 设备事件回调，记录断开事件。 |
| 248-258 | `usb_lib_task` | USB Host library 后台事件处理任务。 |
| 260-265 | `flush_rx` | 清空接收队列，避免旧响应污染新命令。 |
| 267-288 | `read_frame` | 从 `s_rx_queue` 拼接到分号 `;` 为止，形成一条 CAT 响应帧。 |
| 290-300 | `cat_query` | 发送查询命令，等待并读取响应，同时记录耗时。 |
| 302-311 | `cat_send` | 只发送命令，不等待响应，主要用于设置型命令。 |

审核点：

- 这部分是 `cat_transport_usb_direct_cat3` 的雏形。
- 当前读帧只按 `;` 结束，适合 YAESU CAT ASCII。
- `cat_query` 与 `cat_send` 接收 `cdc_acm_dev_hdl_t`，后续应替换为 transport 抽象。

## 11. CAT 响应解析与输入解析：第 313-428 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 313-324 | `parse_fixed_uint` | 从固定字符范围解析无符号整数。 |
| 326-330 | `parse_vfo_hz` | 解析 `FAxxxxxxxxx;` 或 `FBxxxxxxxxx;` 频率响应。 |
| 332-340 | `parse_u8_3` | 解析三位数字参数，如 `PC050;`、`RL007;`。 |
| 342-358 | `parse_mode` | 解析 `MD0x;` 或 `MD1x;` 模式并转为 `ft710_mode_t`。 |
| 360-369 | `parse_bool_4` | 解析第 4 位布尔开关，如 `NR00;`/`NR01;`。 |
| 371-380 | `parse_width_index` | 解析 `SH00xx;` WIDTH index，范围 0-23。 |
| 382-422 | `parse_input_hz` | 把数字键盘输入解析为 Hz，支持 MHz 小数和直接 Hz。 |
| 424-428 | `fmt_freq` | 把 Hz 格式化为 `xx.xxx.xxx` 风格频率显示。 |

审核点：

- 这些函数应迁移到 FT-710 driver 或通用 CAT parser。
- `parse_input_hz` 是 UI 输入逻辑，不属于 driver。
- `parse_width_index` 当前只处理 `SH0;` 这一类 FT-710 响应，后续多机型需放进 driver。

## 12. WiFi 文本与输入辅助：第 430-533 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 430-447 | `wifi_state_text` | 状态栏 WiFi 文本。 |
| 449-466 | `wifi_page_status_text` | WiFi 设置页状态文本。 |
| 468-490 | `wifi_auth_text` | 把 ESP-IDF WiFi auth enum 转成 WPA/WPA2/WPA3 文本。 |
| 492-499 | `send_wifi_cmd` | 向 WiFi 管理任务发送扫描/连接/断开命令。 |
| 501-513 | `wifi_edit_target` | 根据当前编辑字段返回 SSID 或 Password 缓冲区。 |
| 515-526 | `password_mask` | 把密码显示为圆点。 |
| 528-533 | `clear_wifi_addrs` | 清空 IP/Gateway/DNS 文本。 |

审核点：

- WiFi UI 和 WiFi manager 可拆分，但这些文本辅助也可以保留在 UI 层。
- 密码缓冲区后续需要避免写入日志。

## 13. LVGL 基础控件工具：第 535-594 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 535-544 | `label` | 创建文本标签，设置位置、字体、颜色、字距。 |
| 546-559 | `box` | 创建面板/容器，设置背景、边框、圆角、不可滚动。 |
| 561-569 | `set_button_active` | 设置按钮激活/非激活配色，并更新子标签颜色。 |
| 571-574 | `send_cmd` | 向 CAT 命令队列投递 `app_cmd_t`。 |
| 576-584 | `update_input_hint_locked` | 刷新直接频率输入区提示，如目标 VFO 和输入文本。 |
| 586-589 | `selected_input_vfo_hz` | 返回当前输入目标 VFO 的现有频率。 |
| 591-594 | `set_input_from_hz` | 把当前 VFO 频率装入输入框，方便旋钮从当前值开始调整。 |

审核点：

- `label/box/button` 等可迁移到 `ui_helpers.c`。
- `send_cmd` 是 UI 到 CAT task 的桥，后续可以变成 `control_dispatch_action()`。

## 14. I2C 与设备扫描：第 597-681 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 597-600 | `i2c_take` | 获取 I2C 互斥锁。 |
| 602-606 | `i2c_give` | 释放 I2C 互斥锁。 |
| 609-627 | `i2c_scan_range` | 扫描指定 I2C 地址范围并打印探测结果。 |
| 629-681 | `i2c_diagnostic_scan` | 综合扫描外设总线、TCA9548A 各通道、OLED 和 MCP23017。 |

审核点：

- 这部分对现场排查非常有价值，建议保留为 `diagnostics/i2c_diag.c`。
- I2C 锁设计合理，因为 MCP23017 和多块 OLED 共用一条总线。

## 15. 辅助 OLED 底层驱动：第 683-859 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 683-690 | `aux_oled_select` | 选择 TCA9548A 的 OLED 通道。 |
| 692-697 | `aux_oled_cmd` | 向 SSD1306 发送命令字节。 |
| 699-711 | `aux_oled_data` | 分块向 SSD1306 发送显示数据。 |
| 713-723 | `aux_oled_flush` | 把 128x64 帧缓冲刷新到 OLED。 |
| 725-737 | `aux_oled_ssd1306_init` | 初始化 SSD1306。 |
| 739-751 | `aux_oled_pixel` | 设置单个像素。 |
| 753-763 | `aux_oled_rect` | 绘制矩形边框。 |
| 765-772 | `aux_oled_fill_rect` | 绘制填充矩形。 |
| 774-796 | `aux_oled_line` | Bresenham 画线。 |
| 798-835 | `aux_oled_glyph` | 返回内置 5x7 字符点阵。 |
| 837-851 | `aux_oled_char` | 按 scale 绘制单个字符。 |
| 853-859 | `aux_oled_text` | 绘制字符串。 |

审核点：

- 已具备独立 OLED mini graphics 能力，应拆成 `aux_oled` 模块。
- 字库有限，后续如显示中文或更复杂 UI 需新增字体方案。

## 16. 辅助 OLED 功能画面：第 861-1056 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 861-870 | `aux_oled_draw_status` | 通用状态页：标题 + 大字值 + 边框。 |
| 872-888 | `aux_oled_show_status` | 状态页缓存刷新，避免重复刷屏。 |
| 890-932 | `aux_oled_show_power` | RF Power OLED：数字上移、底部 5-100W 进度条。 |
| 934-939 | `aux_oled_show_power_step` | 功率步进选择页，显示 `RF STEP` 和步进值。 |
| 941-950 | `aux_oled_show_dnr` | DNR OLED：显示 OFF 或等级数值。 |
| 952-955 | `current_width_mode` | 根据当前活动 VFO 返回 WIDTH 计算所需模式。 |
| 957-990 | `width_index_to_hz` | 按 FT-710 SH 表将 WIDTH index 映射到实际 Hz。 |
| 992-1036 | `aux_oled_draw_width` | WIDTH OLED：可变梯形、贯穿基准线、交叉斜线填充、下方 Hz 数值。 |
| 1038-1056 | `aux_oled_show_width` | WIDTH OLED 缓存刷新。 |

审核点：

- `width_index_to_hz` 是 FT-710 业务表，应放到 driver 或 feature。
- `aux_oled_draw_width` 是 WIDTH feature 的 OLED renderer。
- Power/DNR/WIDTH 三个 OLED 页面已经适合作为功能模块模板。

## 17. 辅助 OLED 初始化与统一刷新：第 1058-1145 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1058-1067 | `aux_oled_show_current` | 根据当前状态统一刷新 Power/DNR/WIDTH OLED。 |
| 1069-1105 | `aux_oled_init_one` | 初始化单个 OLED，尝试 0x3C/0x3D 地址。 |
| 1107-1145 | `aux_oled_init` | 初始化 TCA9548A 和三块 OLED。 |

审核点：

- 五块 OLED 时应由配置表循环初始化。
- 当前通道 2/3/4 固定写死，后续应改为 `hardware_group_config_t`。

## 18. 旋钮业务逻辑：第 1147-1356 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1147-1162 | `submit_input_frequency` | 把输入框频率发送为 `CMD_SET_FREQ`。 |
| 1164-1187 | `encoder_adjust_input` | 频率旋钮按 1 kHz 改变输入值，不立即发送。 |
| 1189-1198 | `power_step_index` | 把 2/5/10W 转为索引。 |
| 1200-1224 | `encoder_adjust_power_step` | 功率旋钮处于步进选择模式时切换 2/5/10W。 |
| 1226-1255 | `encoder_adjust_power` | 按当前步进调整功率并发送 `CMD_SET_POWER`。 |
| 1257-1288 | `encoder_adjust_dnr` | 调整 DNR 等级，0 表示关闭。 |
| 1290-1306 | `encoder_dnr_off` | DNR 旋钮按键直接关闭 DNR。 |
| 1308-1311 | `active_vfo_mode` | 返回当前活动 VFO 的模式。 |
| 1313-1317 | `width_default_for_mode` | 当前暂时统一返回 FT-710 WIDTH default index 0。 |
| 1319-1333 | `encoder_set_width` | 设置 WIDTH index，并发送 `CMD_SET_WIDTH`。 |
| 1335-1351 | `encoder_adjust_width` | 调整 WIDTH index。 |
| 1353-1356 | `encoder_width_default` | WIDTH 旋钮按键恢复 default。 |

审核点：

- 这些就是未来 `control_features` 的核心。
- 每个函数都应该改成 feature 的 `on_rotate/on_press/render_oled`。
- 频率旋钮目前改输入区，按键才发送，符合用户已定需求。

## 19. MCP23017 驱动与旋钮任务：第 1358-1679 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1358-1367 | `mcp23017_write_reg` | 写 MCP23017 寄存器。 |
| 1369-1377 | `mcp23017_read_reg` | 读 MCP23017 寄存器。 |
| 1379-1392 | `mcp23017_write_reg_retry` | 带重试写寄存器，提高稳定性。 |
| 1394-1492 | `encoder_mcp23017_init` | 扫描 0x27 优先的 MCP23017 地址，配置输入和上拉。 |
| 1494-1679 | `encoder_task` | 周期读取 GPIOA/GPIOB，解码四个 EC11：频率、DNR、功率、WIDTH。 |

审核点：

- 当前已支持 4 个旋钮，第 5 个旋钮需要利用剩余 MCP23017 引脚。
- 旋钮解码逻辑应抽象为通用 `encoder_slot_t`，不应每个旋钮写一套状态变量。
- 目前 task 里业务调用较多，后续应变成 `hardware event -> feature dispatch`。

## 20. OLED 刷新任务：第 1681-1697 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1681-1697 | `aux_oled_task` | 初始化辅助 OLED 后，每 200 ms 刷新一次 Power/DNR/WIDTH。 |

审核点：

- 五块 OLED 后，刷新周期应按 feature 脏标记触发，避免无意义刷新。
- 对 I2C 稳定性而言，当前 200 ms 是较保守的选择。

## 21. LVGL 按钮事件分发：第 1699-1867 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1699-1867 | `button_event_cb` | 所有触摸按钮共用事件回调，根据字符串 ID 执行动作。 |

主要分支：

| ID 类型 | 功能 |
|---|---|
| `wifi` | 打开 WiFi 设置页。 |
| `back` | 从 WiFi 页返回主界面。 |
| `time` | 切换本地/UTC 时间显示。 |
| 数字/`.`/`bs`/`clear`/`enter` | 直接频率输入。 |
| `target_a/target_b` | 选择输入目标 VFO。 |
| `switch_ab` | 切换工作 VFO。 |
| `pwr_*` | 设置功率或功率步进。 |
| `dnr_*` | 设置 DNR。 |
| `mode_*` | 设置模式，目标跟随 A/B。 |
| `band_*` | 选择波段默认频率。 |
| `wifi_ssid/password/connect/disconnect/scan` | WiFi 页面操作。 |
| 软键盘按键 | 输入 SSID/Password，含 CLEAR/BACK/SPACE。 |

审核点：

- 这是当前 UI 交互中枢，但字符串 ID 分发后续会越来越难维护。
- 建议逐步改为：控件绑定 `control_action_t` 或 feature 指针。
- WiFi 键盘逻辑和电台控制逻辑应拆开。

## 22. UI 创建辅助：第 1868-1933 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1868-1887 | `button` | 创建按钮并绑定 `button_event_cb`。 |
| 1889-1895 | `pill` | 创建状态栏 pill。 |
| 1897-1906 | `touch_zone` | 创建透明触摸区域。 |
| 1908-1924 | `format_time_text` | 根据本地/UTC 生成状态栏时间文本。 |
| 1926-1933 | `update_top_bar_locked` | 刷新 CAT、BT、WiFi、RX、时间状态栏。 |

审核点：

- 状态栏已经有主控菜单入口雏形。
- CAT 状态后续应显示 `CAT WIFI/BLE/USB-S3/USB-DIRECT/OFF`。

## 23. 顶部状态栏与 UI 刷新：第 1935-2027 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 1935-1953 | `create_top_bar` | 创建顶部状态栏，包括 FT-710 CONTROL、CAT、BT、WiFi、RX、BACK/MENU。 |
| 1955-2027 | `update_ui_locked` | 根据 `s_state` 刷新主界面和 WiFi 页全部动态标签/按钮高亮。 |

审核点：

- `update_ui_locked` 是 UI 状态同步核心。
- 后续 feature 化后，每个 panel 应由自己的 `render/update` 处理。

## 24. UI 更新入口与 WiFi 页面：第 2029-2110 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2029-2035 | `update_ui` | 获取 LVGL 锁后调用 `update_ui_locked`。 |
| 2037-2110 | `create_wifi_ui` | 创建 WiFi 设置页，左侧扫描列表，右侧连接信息、SSID/Password、连接/断开。 |

审核点：

- WiFi 页面已经按用户要求收敛为路由器连接配置，不做 WiFi Host。
- 扫描列表支持滚动，适合 2.4G AP 选择。

## 25. WiFi 软键盘与主界面创建：第 2112-2261 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2112-2172 | `create_wifi_keyboard_ui` | 创建全屏软键盘，输入 SSID/Password。 |
| 2174-2261 | `create_ui` | 创建主控制界面：频率区、数字键盘、RF Power、DNR、Mode、Band。 |

审核点：

- 主界面固定 1024x600 横屏。
- 当前主界面还不是可配置 panel，后续 `create_ui` 要改为固定频率区 + 4 个动态 panel。

## 26. CAT 设置命令执行：第 2263-2344 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2263-2344 | `apply_command` | 把 `app_cmd_t` 转成 FT-710 CAT 设置命令并发送。 |

主要命令映射：

| app 命令 | CAT 命令 |
|---|---|
| `CMD_SET_FREQ` | `FAxxxxxxxxx;` 或 `FBxxxxxxxxx;` |
| `CMD_SET_POWER` | `PCxxx;` |
| `CMD_ADJUST_POWER` | `PCxxx;` |
| `CMD_SET_POWER_STEP` | 只改本地步进，不发 CAT。 |
| `CMD_SET_DNR_LEVEL` | `NR00;`、`NR01;`、`RL0xx;` |
| `CMD_ADJUST_DNR` | `NR00;` 或 `RL0xx;` + 必要时 `NR01;` |
| `CMD_SET_WIDTH` | `SH00xx;` |
| `CMD_SET_MODE` | `MD0x;` 或 `MD1x;` |
| `CMD_SELECT_MAIN_VFO` | `VS0;` 或 `VS1;` |
| `CMD_SET_BAND_FREQ` | `FAxxxxxxxxx;` 或 `FBxxxxxxxxx;` |

审核点：

- 这里是最应该迁移到 `radio_driver_yaesu_ft710` 的核心区域。
- 当前为了响应快，很多设置会先更新本地状态，再等待后续轮询纠正。
- DNR 开关和值分两条命令，这是 FT-710 的真实协议特性。

## 27. WiFi 配置与 SNTP：第 2346-2390 行

| 行号 | 函数/宏 | 功能 |
|---:|---|---|
| 2346-2347 | `WIFI_CONNECTED_BIT/WIFI_FAIL_BIT` | WiFi 事件组标志。 |
| 2349-2359 | `wifi_save_config` | 保存 SSID/Password 到 NVS。 |
| 2361-2372 | `wifi_load_config` | 从 NVS 读取 SSID/Password。 |
| 2374-2378 | `wifi_apply_test_defaults` | 当前强制应用测试默认 WiFi。 |
| 2380-2390 | `start_sntp_once` | 设置时区并启动 SNTP 时间同步。 |

审核点：

- `wifi_apply_test_defaults` 后续产品版要移除或改为首次启动默认。
- WiFi 密码保存 NVS 是必要的，但日志中不能打印密码。

## 28. WiFi Bridge 探测：第 2392-2486 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2392-2406 | `parse_bridge_discovery` | 解析 `CAT3BRIDGE ip port` 发现消息。 |
| 2408-2446 | `wifi_bridge_tcp_probe` | TCP 连接桥接端，发送 `ID;`，期待 `ID0800;`。 |
| 2448-2486 | `wifi_bridge_client_task` | UDP discovery 后尝试探测 WiFi Bridge。 |

审核点：

- 这是未来 `cat_transport_wifi_bridge` 的原型。
- 目前它只探测，不接管 CAT 主链路。
- 后续主控/从机架构中，WiFi Bridge 将成为 Auto 优先级第一位。

## 29. WiFi 事件与管理任务：第 2488-2674 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2488-2542 | `wifi_event_handler` | 处理 STA 断开、获得 IP，更新状态和 IP/Gateway/DNS，启动 SNTP/Bridge 探测。 |
| 2544-2586 | `wifi_scan` | 阻塞式扫描 AP，填充扫描列表，并重建 WiFi UI。 |
| 2588-2618 | `wifi_connect_current` | 使用当前 SSID/Password 配置 STA 并连接。 |
| 2620-2674 | `wifi_manager_task` | 初始化 NVS/netif/WiFi，自动 scan/connect，并处理 WiFi 命令队列。 |

审核点：

- WiFi STA 基础功能已具备。
- 从机未来也需要类似 WiFi 配置页，可复用设计但代码不一定直接复用。
- 主控的 WiFi 数据源和 WiFi CAT Bridge 需要继续解耦。

## 30. CAT 轮询任务：第 2676-2765 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2676-2700 | `poll_one` | 发送一个 CAT 查询，解析响应并更新 `s_state`。 |
| 2702-2765 | `cat_task` | USB Host 打开 CH9102/CDC 设备，处理命令队列，并周期轮询 FT-710。 |

轮询策略：

```text
快速轮询：FA; FB;
慢速轮询：ID; MD0; MD1; PC; NR0; RL0; SH0; VS;
```

审核点：

- 这是当前电台状态同步核心。
- 后续 `cat_task` 应不再直接打开 USB CDC，而是通过 `cat_link_manager` 使用当前活动 transport。
- Auto 链路需要在这里替换底层收发，不改变上层轮询命令。

## 31. 应用启动入口：第 2767-2809 行

| 行号 | 函数 | 功能 |
|---:|---|---|
| 2767-2809 | `app_controller_start` | 初始化状态、队列、锁、BSP/LVGL、UI、USB Host、WiFi、旋钮、OLED、CAT 任务。 |

启动流程按实际执行顺序理解：

```text
1. 初始化 radio_state。
2. 创建 RX/CAT/WiFi 队列和 I2C mutex。
3. 启动 BSP 显示和 LVGL。
4. 创建主界面。
5. 初始化 USB Host library。
6. 注册 CDC ACM Host driver。
7. 创建 WiFi manager task。
8. 创建 encoder task。
9. 创建 aux_oled task。
10. 创建 cat_task。
11. 打印控制 UI 启动完成日志。
```

审核点：

- 启动入口职责过多，但结构清晰。
- 后续可以改成 `module_init()` 风格，逐个初始化组件。

注意：当前源码总行数为 2809 行。本文函数行号来自当前 `rg`/PowerShell 扫描结果，若文件被继续编辑，行号会随之变化。

## 32. 当前已实现功能对照

| 功能 | 当前状态 | 核心代码位置 |
|---|---|---|
| 双 VFO 频率显示 | 已实现 | `update_ui_locked`、`poll_one` |
| 直接频率输入 | 已实现 | `parse_input_hz`、`submit_input_frequency`、`create_ui` |
| VFO-A/VFO-B 目标选择 | 已实现 | `button_event_cb`、`apply_command` |
| VFO 工作状态切换 | 已实现 | `VS0/VS1` |
| 模式设置 | 已实现 | `CMD_SET_MODE` -> `MD0x/MD1x` |
| RF Power 设置 | 已实现 | `encoder_adjust_power`、`apply_command`、`PCxxx` |
| 功率步进选择 | 已实现 | `encoder_adjust_power_step`、`aux_oled_show_power_step` |
| DNR 开关/等级 | 已实现 | `encoder_adjust_dnr`、`encoder_dnr_off`、`NR/RL` |
| WIDTH 设置 | 已实现 | `encoder_adjust_width`、`SH00xx` |
| WIDTH OLED 图形 | 已实现 | `aux_oled_draw_width` |
| WiFi STA 设置 | 已实现 | `create_wifi_ui`、`wifi_manager_task` |
| 时间本地/UTC切换 | 已实现 | `format_time_text`、`button_event_cb` |
| WiFi Bridge 探测 | 实验性 | `wifi_bridge_client_task` |
| BLE Bridge | 未实现到主控正式链路 | 需新增 |
| USB-S3 Bridge | 未实现 | 需新增 |
| Direct USB-CAT3 | 已实现 | `cat_task` + CDC ACM |
| 五组旋钮/OLED配置 | 未实现 | 需新增 `hardware_slots/layout_config` |
| 主屏四块可配置面板 | 未实现 | 需新增 `control_features/layout_config` |

## 33. 主要技术债

1. 单文件过大。

`app_controller.c` 同时管理 UI、网络、USB、CAT、I2C、OLED、旋钮，后续扩展会困难。

2. FT-710 CAT 命令未抽象。

`apply_command`、`poll_one`、各 parse 函数都写死 FT-710 CAT 格式，后续支持其他 YAESU 型号前应拆出 driver。

3. Transport 未抽象。

当前 CAT 主链路直接绑定 USB CDC ACM。后续 WiFi/BLE/USB-S3/Direct USB-CAT3 Auto 优先级需要 `cat_transport` 和 `cat_link_manager`。

4. 硬件槽位未配置化。

旋钮和 OLED 目前按固定引脚/固定 TCA 通道写死。五组旋钮/OLED 需要配置表。

5. UI 面板未模块化。

主屏 RF Power、DNR、Mode、Band 是固定布局。后续需变成四个可配置 panel。

6. WiFi 测试默认值硬编码。

`KE/qazwsxedc` 作为测试默认值有用，但产品版应从 NVS/首次配置流程获取。

## 34. 最小安全重构顺序

为了不破坏已经实机验证的功能，建议按以下顺序拆：

```text
1. 新建 radio_driver_yaesu_ft710，仅迁移 CAT 命令拼接和响应解析。
2. 新建 cat_transport_usb_direct_cat3，包住现有 USB CDC ACM read/write。
3. 新建 cat_link_manager，初期只注册 Direct USB-CAT3。
4. 把 poll_one/apply_command 改为调用 driver + link_manager。
5. 新建 control_features，迁移 Power/DNR/WIDTH/Frequency 的旋钮动作。
6. 新建 hardware_slots，把四个现有旋钮和三块 OLED 放入槽位表。
7. 扩展到五组旋钮/OLED。
8. 再加入 WiFi Bridge、BLE Bridge、USB-S3 Bridge。
```

这个顺序的好处是每一步都能独立 build/flash/实测，不需要一次性重写。
