---
title: ESP32-P4 直连 FT-710 USB Host 实测记录
tags:
  - ESP32-P4
  - FT-710
  - USB-Host
  - CAT
  - test-log
created: 2026-08-22
---

# ESP32-P4 直连 FT-710 USB Host 实测记录

## 测试时间

- 2026-08-22 15:10 +08:00

## 测试目标

在 FT-710 已通过 USB 连接到 ESP32-P4 后，验证 ESP32-P4 是否可以通过 USB Host 枚举 FT-710 内部的 CP2105 串口，并进一步打开 Enhanced COM Port 发送安全只读 CAT 命令。

本轮固件只发送读命令，不发送 PTT 或发射相关命令。

## 测试项目

- 项目路径：`D:\CAT CONTROL\ft710_usb_probe`
- ESP-IDF：v5.5.5
- Target：`esp32p4`
- 板卡串口：`COM4`
- 固件用途：FT-710 USB Host CAT 探针
- 依赖组件：
  - `espressif/usb_host_vcp`
  - `espressif/usb_host_cp210x_vcp`
  - `espressif/usb_host_cdc_acm`

## 固件行为

- 安装 ESP-IDF USB Host。
- 注册 CP210x VCP 驱动。
- 尝试打开 CP210x VCP。
- 计划设置串口参数为 38400、8N2。
- RTS/DTR 保持 false。
- 仅计划发送以下只读 CAT 命令：
  - `ID;`
  - `FA;`
  - `FB;`
  - `MD0;`
  - `MD1;`
  - `PC;`
  - `NR0;`
  - `RL0;`

## 构建与烧写

第一次启用 CP210x VCP 探针后，ESP 能看到 FT-710 前级 USB Hub，但默认未启用外部 Hub 支持，日志出现：

```text
W HUB: External Hubs support disabled, Hub device was not initialized
E ft710_probe: Failed to open CP210x VCP device
```

随后在项目配置中启用：

```text
CONFIG_USB_HOST_HUBS_SUPPORTED=y
```

重新构建成功，生成：

```text
D:\CAT CONTROL\ft710_usb_probe\build\ft710_usb_probe.bin
```

刷写到 `COM4` 成功。

## 实测日志摘要

启用 Hub 支持后，ESP32-P4 启动正常，固件进入 FT-710 USB Host 探测流程：

```text
I ft710_probe: FT-710 USB Host CAT probe starting
I ft710_probe: Safety: only Enhanced VCP auto-open path, no PTT/TX commands, RTS/DTR false
I ft710_probe: Waiting for FT-710 CP2105 VCP device...
```

随后 USB Host 已识别到前级 Hub，但下游设备枚举失败：

```text
E USBH: Dev 1 EP 0 STALL
E HUB: Connected device is FS, transaction translator (TT) is not supported
E EXT_PORT: [1:1] Port disabled, reset attempts=1
W HUB: Device tree node (parent_port=1): not found
E HUB: Connected device is FS, transaction translator (TT) is not supported
E EXT_PORT: [1:2] Port disabled, reset attempts=1
W HUB: Device tree node (parent_port=2): not found
E ft710_probe: Failed to open CP210x VCP device
```

## 源码确认

ESP-IDF v5.5.5 的 USB Host Hub 代码中明确限制：当父设备是 High-Speed Hub，而下游设备是 Full-Speed 或 Low-Speed 时，需要 Transaction Translator，当前未实现。

相关源码位置：

- `C:\esp\v5.5.5\esp-idf\components\usb\hub.c`
- `C:\esp\v5.5.5\esp-idf\components\usb\ext_hub.c`

关键判断摘要：

```text
Connected device is FS, transaction translator (TT) is not supported
Transaction Translator has not been implemented yet
```

## 结论

- ESP32-P4、ESP-IDF、USB Host 固件、烧写串口和基础运行环境正常。
- FT-710 通过 USB 接到 ESP 后，ESP 端能够看到 FT-710 USB 设备路径中的 Hub。
- 当前失败点不是 CAT 参数、波特率、CP210x 驱动注册或接线问题。
- 当前失败点是 ESP-IDF v5.5.5 USB Host Hub 栈不支持 High-Speed Hub 下游 Full-Speed 设备所需的 TT。
- 因此，本轮没有枚举到 CP2105 VCP，也没有进入 CAT `ID;` 查询阶段。

## 对项目的影响

电脑直连 FT-710 已证明 CAT 协议和 FT-710 Enhanced COM Port 完全可用；ESP 端当前受限于 USB Host Hub/TT 支持。

如果 FT-710 内部 USB 结构必须经过 High-Speed Hub 再挂 Full-Speed CP2105，那么 ESP32-P4 直接通过当前 ESP-IDF USB Host 栈访问 FT-710 CAT 会被该限制阻挡。

## 下一步建议

1. 确认 FT-710 USB 设备拓扑，记录 Hub、Audio、CP2105 的速度和接口层级。
2. 查 ESP-IDF 后续版本或 Espressif issue，确认 TT 支持是否已有可用实现。
3. 评估替代硬件链路：
   - 使用 FT-710 后部 REM/Linear 等非 USB CAT 接口，如果目标功能可覆盖。
   - 增加一个支持完整 USB Host Hub TT 的桥接主机，将 FT-710 Enhanced COM 转换为 ESP32-P4 可用 UART。
   - 采用 Linux/USB Host 能力更完整的协处理器负责 FT-710 USB，ESP32-P4 负责 UI。
4. 在决定硬件路线前，不建议继续投入完整 UI 与 FT-710 USB 直连深度集成。
