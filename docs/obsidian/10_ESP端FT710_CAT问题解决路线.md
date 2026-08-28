---
title: ESP 端 FT-710 CAT 问题解决路线
tags:
  - ESP32-P4
  - FT-710
  - CAT
  - USB-Host
  - UART
created: 2026-08-22
---

# ESP 端 FT-710 CAT 问题解决路线

## 背景

ESP32-P4 通过 USB 直连 FT-710 时，默认 High-Speed USB Host 路线可以识别到 FT-710 USB 路径中的 Hub，但 ESP-IDF v5.5.5 不支持 High-Speed Hub 下游 Full-Speed 设备所需的 Transaction Translator，导致无法枚举 CP2105 CAT 串口。

前置事实：

- 电脑直连 FT-710 已验证 `COM5 Enhanced COM Port` CAT 通信正常。
- ESP32-P4 USB Host 固件、CP210x 组件和 ESP-IDF 构建烧写流程正常。
- 当前障碍集中在 USB Host Hub/TT 支持。

## 已尝试的软件 workaround

### 1. 启用 ESP-IDF Hub 支持

项目：`D:\CAT CONTROL\ft710_usb_probe`

配置：

```text
CONFIG_USB_HOST_HUBS_SUPPORTED=y
```

结果：

```text
E HUB: Connected device is FS, transaction translator (TT) is not supported
E ft710_probe: Failed to open CP210x VCP device
```

结论：能识别 Hub，但仍被 TT 限制阻挡。

### 2. 强制尝试 ESP32-P4 OTG1.1 Full-Speed 根口

修改：

- 手动 `usb_new_phy()`。
- `USB_PHY_TARGET_INT`
- `USB_PHY_SPEED_FULL`
- `usb_host_config_t.skip_phy_setup = true`
- `usb_host_config_t.peripheral_map = BIT1`

目的：让 FT-710 的 Hub 以 Full-Speed 上游工作，避免 High-Speed Hub 下游 Full-Speed 设备所需的 TT。

结果：

```text
I ft710_probe: USB workaround trial: force ESP32-P4 OTG1.1 Full-Speed root port to avoid HS hub TT
E USBH: Dev 0 EP 0 Error
E ENUM: Bad transfer status 1: CHECK_SHORT_DEV_DESC
E ENUM: [0:0] CHECK_SHORT_DEV_DESC FAILED
E ft710_probe: Failed to open CP210x VCP device
```

### 3. Full-Speed 根口 power-cycle

修改：

- `root_port_unpowered = true`
- 安装 Host 后执行：
  - `usb_host_lib_set_root_port_power(false)`
  - 延时 500 ms
  - `usb_host_lib_set_root_port_power(true)`
  - 延时 1500 ms

结果仍为：

```text
E USBH: Dev 0 EP 0 Error
E ENUM: Bad transfer status 1: CHECK_SHORT_DEV_DESC
E ENUM: [0:0] CHECK_SHORT_DEV_DESC FAILED
```

结论：Full-Speed 根口 workaround 改变了失败形态，但没有成功枚举 FT-710 USB 设备。

### 4. 按 JI1FGX Yaesu CP2105 ESP32-S3 方案尝试本地 Hub 组件

参考项目：

```text
https://ji1fgx.com/en/260408.php
```

实验工程：

```text
D:\CAT CONTROL\ft710_ji1fgx_probe
```

做法：

- 下载并分析 JI1FGX Yaesu CP2105 ESP32-S3 项目。
- 复制其 `managed_components\espressif__usb` 到本地实验工程。
- 修改本地 `hub.c`，把 HS Hub 下游 FS 设备的 TT 硬失败改为继续尝试枚举。

实机结果：

```text
W HUB: Trial: continuing enumeration for FS device behind HS hub without explicit TT support
E USBH: Dev 0 EP 0 Error
E ENUM: Bad transfer status 1: CHECK_SHORT_DEV_DESC
E ENUM: CHECK_SHORT_DEV_DESC FAILED
E ft710_probe: Failed to open CP210x VCP device
```

结论：这证明修改后的本地 Hub 组件确实进入固件，但单纯绕过 TT 检查不够。ESP32-P4 + FT-710 仍需要真实的 High-Speed Hub Transaction Translator 支持，否则无法完成 CP2105 下游设备枚举。详细记录见 [[11_JI1FGX_CP2105方案移植尝试记录]]。

## 当前判断

继续在 ESP-IDF v5.5.5 内给 USB Host Hub/TT 硬补功能，风险和工作量都偏高，不适合作为本项目第一阶段主线。JI1FGX 方案的 CP2105 用户态驱动可以留作后续参考，但不能直接解决 FT-710 内置 Hub 下游 CP2105 在 ESP32-P4 上的枚举问题。

更稳妥的解决路线是避开 FT-710 USB Composite/Hub，改走 FT-710 官方 CAT-3 TTL 串口路径。2026-08-22 已进一步验证：通过外置 CH9102 USB-TTL 接 CAT-3，并由 ESP32-P4 USB Host 打开 CH9102，可以完成 CAT 最小闭环。

## 推荐解决方案：CAT-3 UART / 外置 USB-TTL

FT-710 后面板 `TUNER/LINEAR` 可配置为 CAT-3 TTL 串口。该路线不经过 USB Hub，不需要 ESP32-P4 USB Host，也不涉及 CP2105/TT。

注意事项：

- FT-710 CAT-3 为 5V TTL。
- ESP32-P4 GPIO 为 3.3V，不应直接接 5V TTL。
- 必须使用电平转换或隔离电路。
- 初期仍保持只读命令测试，不发送 PTT/TX。

当前已验证的可行实现是：

```text
ESP32-P4 USB Host -> 外置 CH9102 USB-TTL -> FT-710 CAT-3 TTL UART
```

该方案仍使用 ESP32-P4 USB Host，但 USB 设备变成外置 CH9102，而不是 FT-710 内置 Hub 下游 CP2105，因此绕开了 TT 限制。

## 已验证的 CH9102 USB-TTL 探针工程

项目路径：

```text
D:\CAT CONTROL\ft710_ch9102_usb_probe
```

USB 设备：

```text
VID:PID = 1A86:55D4
设备 = USB-Enhanced-SERIAL CH9102
```

ESP 侧参数：

- ESP32-P4 USB Host
- `usb_host_cdc_acm` 组件
- `cdc_acm_host_open(0x1A86, 0x55D4, 0, ...)`
- 38400
- 8N1
- 无流控
- RTS/DTR false

ESP32-P4 实机读回：

```text
ID;  -> ID0800;
FA;  -> FA009014000;
FB;  -> FB028400000;
MD0; -> MD05;
MD1; -> MD11;
```

结论：外置 CH9102 USB-TTL + FT-710 CAT-3 已完成 ESP32-P4 CAT 最小闭环。详细记录见 [[12_ESP32P4_CH9102_USBTTL_CAT3实测记录]]。

## 已建立的 UART 探针工程

项目路径：

```text
D:\CAT CONTROL\ft710_uart_probe
```

默认参数：

- ESP32-P4 UART：`UART_NUM_1`
- TX：GPIO27
- RX：GPIO26
- 波特率：38400
- 数据位：8
- 停止位：2
- 校验：None
- 流控：None

只读测试命令：

```text
ID;
FA;
FB;
MD0;
MD1;
PC;
NR0;
RL0;
```

构建结果：

```text
D:\CAT CONTROL\ft710_uart_probe\build\ft710_uart_probe.bin
```

构建已通过。

## 推荐接线

需要 FT-710 CAT-3 TTL 与 ESP32-P4 UART 之间加电平转换：

- FT-710 CAT-3 TX -> 电平转换 -> ESP32-P4 GPIO26 RX
- ESP32-P4 GPIO27 TX -> 电平转换 -> FT-710 CAT-3 RX
- GND 共地，或使用隔离模块按模块说明接地

如果后续做成正式产品，建议在外置 CH9102 原型基础上评估内置 USB-UART、隔离 UART、限流、ESD、防误插保护和射频环境抗扰。

## 下一步

1. 将 `ft710_ch9102_usb_probe` 的 CH9102 打开、串口设置、收发与分号拆帧整理为正式 `CatTransport`。
2. 扩展只读查询集合，验证 `PC; NR0; RL0;` 等状态读取。
3. 对外置 CH9102 + CAT-3 做热插拔、开关电台、长时间运行和射频发射环境测试。
4. 根据正式硬件目标，决定继续使用外置/内置 USB-UART，还是改成 ESP32-P4 GPIO UART + 隔离/电平转换。
5. 保留 FT-710 后部 USB Enhanced/CP2105 直连路线，等待 ESP-IDF 后续提供真实 Hub TT 支持后再评估。
