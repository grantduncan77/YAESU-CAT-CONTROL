---
title: GitHub 与 Gitee 项目调研
tags:
  - GitHub
  - Gitee
  - reference
created: 2026-08-21
---

# GitHub 与 Gitee 项目调研

## 结论

没有发现一个可以直接烧录、并同时满足以下三项的完整开源项目：

1. 微雪 ESP32-P4-WIFI6-LCD-TOUCH-7B。
2. ESP32-P4 作为 USB Host 控制 FT-710 的 CP210x CAT 串口。
3. 当前定义的 1024 × 600 五区触摸界面。

可行做法是组合借鉴：以微雪官方 BSP 和示例完成板卡适配，以 Espressif CP210x 组件完成 USB Host 串口，以 FT-710 / Yaesu 控制项目借鉴 CAT 解析、队列和界面交互。

## 候选项目

| 项目 | 平台 | 可借鉴部分 | 适配注意 |
|---|---|---|---|
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-7B](https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-7B) | GitHub | 板级资料、显示、触摸和示例 | 首要基线，需核对当前板卡修订版 |
| [mrrc_ft710](https://github.com/cheenle/mrrc_ft710) | GitHub | FT-710 CAT 控制、状态轮询、远程控制结构 | 主机平台与 ESP32-P4 不同 |
| [Yaesu_Web_Control](https://github.com/mm5agm/Yaesu_Web_Control) | GitHub | Yaesu 命令映射、Web 控制交互、状态同步 | 需筛选 FT-710 支持范围 |
| [Arduino FT991A CAT library](https://github.com/vk3fsk/arduino-FT991A-CAT-library) | GitHub | Yaesu CAT 报文封装思路 | FT-991A 命令不能不经核对直接用于 FT-710 |
| [EspUsbHost](https://github.com/tanakamasayuki/EspUsbHost) | GitHub | ESP32 USB Host 的实现参考 | 芯片和 ESP-IDF 版本兼容性需单独验证 |
| [Espressif usb_host_cp210x_vcp](https://components.espressif.com/components/espressif/usb_host_cp210x_vcp) | Espressif | CP210x USB Host VCP 官方组件 | 优先采用，锁定并验证组件版本 |
| [微雪 ESP32-P4-WIFI6-Touch-LCD-X](https://gitee.com/waveshare/esp32-p4-wifi6-touch-lcd-x) | Gitee | 国内镜像、板卡资料和示例 | 仓库名为 X 系列，确认 7B 对应目录 |
| [FT817 CAT Display](https://gitee.com/hanjs/ft817-cat-display) | Gitee | 小型电台 CAT 显示器的结构 | 协议和硬件均不同，只借鉴分层 |
| [Q900 Display](https://gitee.com/bg1jt/q900-display) | Gitee | 嵌入式电台触控/显示项目组织 | 非 FT-710，不复用具体命令 |

## 最值得直接复用的层次

### 1. 板级启动

优先参考微雪仓库中的：

- ESP-IDF 工程配置。
- MIPI-DSI 显示初始化。
- GT911 触摸初始化和坐标映射。
- 背光、SD 卡、音频、IO 扩展器等外围驱动。
- LVGL 示例、帧缓冲和 PSRAM 配置。

此部分应尽量沿用官方 BSP，不在首版重写显示时序。

### 2. USB Host 与 CP210x

优先使用 Espressif 的 usb_host_cp210x_vcp 组件。验证顺序：

1. 枚举 VID / PID 和接口。
2. 打开 CP210x。
3. 设置 FT-710 对应串口参数。
4. 发送只读 CAT 命令。
5. 验证分号结尾报文的接收和拆帧。
6. 测试拔插和异常恢复。

### 3. CAT 模型

从 mrrc_ft710、Yaesu_Web_Control 和 FT991A 库借鉴：

- 命令构造器。
- 带分号终止符的流式解析。
- 请求队列和速率限制。
- 轮询项调度。
- 状态缓存与 UI 发布。
- 连接断开后的恢复机制。

具体命令编码必须以 FT-710 官方 CAT Operation Reference 为准。

### 4. 界面

开源项目中的界面适合作为交互思路参考，不宜直接移植像素布局。当前项目应自行实现 LVGL 组件，保持：

- 五个功能区。
- 大触摸目标。
- 低光深色主题。
- 回读确认。
- 模式和波段在界面层互不联动。

## DNR 的关键协议点

DNR 至少涉及两类状态：

- NR：DNR 开关。
- RL：DNR 级别。

因此 UI 的 OFF / 1 至 15 不能映射为一个简单数值寄存器。建议状态模型至少包含：

- dnr_enabled
- dnr_level

OFF 到 1、1 到 OFF、在线回读和外部旋钮改变，都要用双状态校准。

## 开源许可注意

- 复用任何代码前检查许可证、版权声明和再分发条件。
- 无明确许可证的仓库只用于阅读和思路参考，不直接复制代码。
- 将第三方代码独立放入 components 或 third_party，并保留来源、版本和修改记录。

