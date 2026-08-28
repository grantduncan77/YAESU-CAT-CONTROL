---
title: ESP32-P4 外置 CH9102 USB-TTL 连接 FT-710 CAT-3 实测记录
tags:
  - ESP32-P4
  - FT-710
  - CAT
  - CAT-3
  - CH9102
  - USB-Host
created: 2026-08-22
---

# ESP32-P4 外置 CH9102 USB-TTL 连接 FT-710 CAT-3 实测记录

## 背景

FT-710 后部 USB Type-B 直连 ESP32-P4 时，会遇到 FT-710 内置 USB Hub 下游 CP2105 的 High-Speed Hub Transaction Translator 限制。ESP-IDF v5.5.5 当前无法完成该拓扑下 CP2105 枚举。

为绕开 FT-710 内置 USB Hub/CP2105 路径，本次测试改用：

```text
ESP32-P4 USB Host -> 外置 CH9102 USB-TTL -> FT-710 CAT-3 TTL UART
```

这样 ESP32-P4 侧仍然使用 USB Host，但枚举对象变为外置 CH9102，而不是 FT-710 内置 CP2105。

## 电脑侧前置验证

外置 TTL-USB 芯片在 Windows 上识别为：

```text
COM7 = USB-Enhanced-SERIAL CH9102
VID:PID = 1A86:55D4
```

曾出现的问题：

- 初次接入 FT-710 CAT-3 时，空闲也持续收到 `80 80 80 00 00` 或 `F8 F8 F8 00 00` 一类规律性乱码。
- CH9102 本地 TX/RX 回环在低速率正常，初期 `38400` 回环异常。
- 调整接线/设置后，CAT-3 空闲乱码消失。

最终电脑侧 CAT-3 通信验证成功，参数：

```text
COM7
38400
8N1
无流控
RTS/DTR false
```

电脑侧读写实测：

```text
ID;  -> ID0800;
FA;  -> FA002347000;
FB;  -> FB021400000;
MD0; -> MD05;
MD1; -> MD11;
```

随后通过电脑侧 CAT 写入并读回：

```text
FA005347000; -> FA005347000;   VFO-A = 5.347000 MHz
FB028400000; -> FB028400000;   VFO-B = 28.400000 MHz
MD05;        -> MD05;          VFO-A = AM
MD11;        -> MD11;          VFO-B = LSB
```

## ESP32-P4 探针工程

新建工程：

```text
D:\CAT CONTROL\ft710_ch9102_usb_probe
```

工程用途：

- ESP32-P4 作为 USB Host。
- 枚举并打开外置 CH9102 USB-TTL。
- 设置串口参数为 `38400 8N1`。
- 只发送安全只读 CAT 查询。
- 在 monitor 中打印响应。

关键依赖：

```yaml
dependencies:
  usb_host_cdc_acm: "^2"
  idf: ">=5.5.0"
```

本次先按 CDC/CDC-like 设备直接打开：

```text
VID = 0x1A86
PID = 0x55D4
interface = 0
```

不需要 FT-710 内置 CP2105，也不需要 ESP-IDF Hub TT 支持。

## 构建与烧写

构建命令：

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"; idf.py build
```

构建结果：

```text
Generated D:/CAT CONTROL/ft710_ch9102_usb_probe/build/ft710_ch9102_usb_probe.bin
```

烧写与监视：

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"; idf.py -p COM4 flash monitor
```

开发板信息：

```text
Chip is ESP32-P4 (revision v1.3)
MAC: e8:f6:0a:e3:59:c6
ESP-IDF: v5.5.5
```

## ESP32-P4 实机结果

关键 monitor 日志：

```text
I ft710_ch9102: FT-710 CAT-3 via external CH9102 USB-TTL probe starting
I ft710_ch9102: Expected USB device: WCH CH9102 VID:PID 1A86:55D4
I ft710_ch9102: Line coding: 38400 8N1, no flow control. Only read-only CAT queries are sent.
I ft710_ch9102: Waiting for CH9102 on ESP32-P4 USB Host...
I ft710_ch9102: CH9102 opened
I ft710_ch9102: Line coding set to 38400 8N1
I ft710_ch9102: RTS/DTR set false
I ft710_ch9102: CAT TX: ID;
I ft710_ch9102: CAT RX: ID0800;
I ft710_ch9102: CAT TX: FA;
I ft710_ch9102: CAT RX: FA009014000;
I ft710_ch9102: CAT TX: FB;
I ft710_ch9102: CAT RX: FB028400000;
I ft710_ch9102: CAT TX: MD0;
I ft710_ch9102: CAT RX: MD05;
I ft710_ch9102: CAT TX: MD1;
I ft710_ch9102: CAT RX: MD11;
I ft710_ch9102: Probe finished: 5/5 CAT queries returned complete frames
```

读回解释：

```text
ID0800;       = FT-710
FA009014000; = VFO-A 9.014000 MHz
FB028400000; = VFO-B 28.400000 MHz
MD05;        = VFO-A AM
MD11;        = VFO-B LSB
```

## 结论

本次测试确认：

- ESP32-P4 可以通过 USB Host 枚举并打开外置 CH9102 USB-TTL。
- CH9102 在 ESP32-P4 上可用 `usb_host_cdc_acm` 按 CDC/CDC-like 设备打开。
- `38400 8N1` 下 CAT-3 通信正常。
- ESP32-P4 已通过外置 CH9102 + CAT-3 完成 FT-710 CAT 最小闭环。
- 5 条只读查询全部返回完整分号结尾帧：`ID; FA; FB; MD0; MD1;`。

这条链路已经绕开 FT-710 内置 USB Hub/CP2105/TT 问题，可作为当前项目第一阶段主线方案。

## 对项目路线的影响

推荐将正式软件中的 CAT 传输层抽象为 `CatTransport`：

- 第一版可采用 `UsbTtlCatTransport`：ESP32-P4 USB Host -> CH9102 USB-TTL -> FT-710 CAT-3。
- 保留 `UsbCp2105CatTransport` 作为未来 ESP-IDF 支持 Hub TT 后的可选实现。
- 仍保留 ESP32-P4 原生 UART CAT-3 方案作为硬件备选。

硬件注意事项：

- FT-710 CAT-3 为 TTL UART，正式接线仍需确认电平兼容、共地、ESD、防误插和射频环境抗扰。
- 外置 USB-TTL 方案减少了 ESP32-P4 GPIO 与 FT-710 之间的直接连接风险，但正式产品应考虑内置 USB-UART 芯片或隔离方案。
- 初期固件继续禁止 PTT/TX 自动控制，所有写命令需明确白名单。
