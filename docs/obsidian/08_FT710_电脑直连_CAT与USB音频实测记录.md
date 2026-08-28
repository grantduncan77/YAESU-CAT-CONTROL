---
title: FT-710 电脑直连 CAT 与 USB 音频实测记录
tags:
  - FT-710
  - CAT
  - USB-Audio
  - CP2105
  - hardware-test
created: 2026-08-22
---

# FT-710 电脑直连 CAT 与 USB 音频实测记录

日期：2026-08-22  
测试目标：在 FT-710 通过 USB 直接连接本电脑的情况下，确认电脑侧 CAT 通信、基础读写命令和 USB 接收音频链路是否正常，为后续 ESP32-P4 USB Host 实现提供已知可用基准。

## 1. 结论

- FT-710 通过 USB 连接本电脑后，Windows 正常识别 CP2105 双串口。
- `COM5` 为 Enhanced COM Port，可用于 CAT-1 控制。
- `COM6` 为 Standard COM Port，本次未测试，避免触碰可能涉及 RTS/DTR/PTT 的接口。
- 使用 `COM5`、38400 baud、8N2、无流控，可稳定执行 FT-710 CAT 命令。
- `ID;` 返回 `ID0800;`，确认机型识别正确。
- VFO-A 频率、VFO-B 频率、VFO-A 模式、VFO-B 模式、DNR 开关与等级读写均验证通过。
- Windows 正常识别 FT-710 的 USB Audio 输入，并可将电台接收音频实时播放到本电脑扬声器。
- 用户确认监听结果正常。

## 2. Windows 设备枚举

串口设备：

```text
COM5 = Silicon Labs Dual CP2105 USB to UART Bridge: Enhanced COM Port
COM6 = Silicon Labs Dual CP2105 USB to UART Bridge: Standard COM Port
```

PNP 信息：

```text
USB Composite Device
PNPDeviceID: USB\VID_10C4&PID_EA70\01A81E2D
Status: OK

Enhanced COM Port
DeviceID: COM5
PNPDeviceID: USB\VID_10C4&PID_EA70&MI_00\7&10EC5CC5&0&0000
Service: silabser
Status: OK

Standard COM Port
DeviceID: COM6
PNPDeviceID: USB\VID_10C4&PID_EA70&MI_01\7&10EC5CC5&0&0001
Service: silabser
Status: OK
```

音频设备：

```text
USB Audio Device
Manufacturer: 通用 USB 音频
PNPDeviceID: USB\VID_0D8C&PID_0013&MI_00\7&30C7225A&0&0000
Status: OK
```

## 3. CAT 测试参数

测试只使用 `COM5`：

```text
Port: COM5
Interface: Enhanced COM Port / CAT-1
Baudrate: 38400
Data bits: 8
Parity: None
Stop bits: 2
Flow control: None
RTS: deasserted
DTR: deasserted
```

安全边界：

- 未打开 `COM6 Standard COM Port` 做控制测试。
- 未发送 `TX`、`MX`、PTT、RTS/DTR keying 等发射相关命令。
- 所有 CAT 写入均限于频率、模式、DNR 等普通控制项。

## 4. 基础只读 CAT 通信

首次只读查询结果：

```text
ID;  -> ID0800;
FA;  -> FA052586000;
MD0; -> MD05;
PC;  -> PC015;
VE0; -> VE00112;
VE1; -> VE10108;
VE2; -> VE20105;
VE3; -> VE30101;
```

含义：

```text
ID0800      = FT-710 机型识别
FA052586000 = VFO-A 52.586000 MHz
MD05        = 当前 VFO-A 模式码 05
PC015       = 设定功率 15 W
VE0~VE3     = FT-710 各固件版本查询结果
```

短循环稳定性测试：

```text
loops=20
commands=60
errors=0
```

循环读回样例：

```text
FA052586000; | MD05; | PC015;
FA052586000; | MD05; | PC015;
FA052586000; | MD05; | PC015;
```

结论：电脑直连 FT-710 的 Enhanced COM Port CAT 通信稳定可用。

## 5. VFO-A 频率写入测试

目标：将 VFO-A 改为 14.270000 MHz。

执行结果：

```text
FA;          -> FA052586000;
FA014270000; -> <timeout>
FA;          -> FA014270000;
```

说明：

- `FA014270000;` 为设置 VFO-A 到 14.270000 MHz。
- 设置命令本身没有直接应答，属正常现象。
- 后续 `FA;` 读回确认成功。

结论：VFO-A 频率写入和读回确认正常。

## 6. VFO-B 频率写入测试

目标：将 VFO-B 改为 21.400000 MHz。

执行结果：

```text
FB;          -> FB000883000;
FB021400000; -> <timeout>
FB;          -> FB021400000;
```

说明：

- `FB021400000;` 为设置 VFO-B 到 21.400000 MHz。
- 设置命令本身没有直接应答，属正常现象。
- 后续 `FB;` 读回确认成功。

结论：VFO-B 频率写入和读回确认正常。

## 7. VFO-A 模式写入测试

目标：将 VFO-A/当前主 VFO 模式设为 USB。

执行结果：

```text
MD0;  -> MD02;
MD02; -> <timeout>
MD0;  -> MD02;
```

说明：

- `MD02` 对应 USB。
- 设置前已经是 USB，设置后读回仍为 USB。

结论：VFO-A 模式读写确认正常。

## 8. VFO-B 模式写入测试

目标：将 VFO-B 模式设为 USB。

执行结果：

```text
MD1;  -> MD15;
MD12; -> <timeout>
MD1;  -> MD12;
```

说明：

- `MD1;` 查询 VFO-B 模式。
- `MD12;` 将 VFO-B 模式设为 USB。
- `MD12` 读回确认 VFO-B 已为 USB。

结论：VFO-B 模式读写确认正常。

## 9. DNR 设置与读取测试

目标：打开 DNR 并设置为 7。

执行结果：

```text
NR0;   -> NR00;
RL0;   -> RL001;
NR01;  -> <timeout>
RL007; -> <timeout>
NR0;   -> NR01;
RL0;   -> RL007;
```

说明：

- `NR0;` 查询 DNR 开关。
- `RL0;` 查询 DNR 等级。
- `NR01;` 打开 DNR。
- `RL007;` 设置 DNR 等级为 7。
- 写入后读回确认 DNR 打开且等级为 7。

随后再次读取 DNR：

```text
NR0; -> NR01;
RL0; -> RL013;
```

结论：

- DNR 当前为 ON。
- 后续读回等级为 13，说明电台端状态已从 7 变为 13；可能来自面板操作、其它状态恢复或电台内部状态变化。
- 固件设计中 UI 必须以读回状态为准，不能假设写入值永久保持。

## 10. USB 接收音频测试

测试目标：获取 FT-710 当前 USB 接收音频，并通过本电脑扬声器播放。

PortAudio 枚举到的相关设备：

```text
Input:  Microphone (USB Audio Device)
Output: Speakers (Realtek Audio)
```

实际监听桥：

```text
输入：USB Audio Device 麦克风输入
输出：Realtek(R) Audio 扬声器
采样率：44100 Hz
监听时长：120 秒
```

监听期间音频输入电平样例：

```text
  5s  input_rms=0.00002 peak=0.00006
 25s  input_rms=0.00234 peak=0.00659
 50s  input_rms=0.00450 peak=0.01221
 65s  input_rms=0.01685 peak=0.03247
 85s  input_rms=0.00577 peak=0.02008
120s  input_rms=0.00017 peak=0.00061
```

用户确认：

```text
监听结果正常
```

结论：FT-710 USB Audio 接收音频可被本电脑正常获取，并可实时播放到电脑扬声器。

## 11. 对 ESP32-P4 项目的意义

本次测试提供了一个“电脑侧已知可用”的基准：

```text
FT-710 Enhanced COM Port:
  COM5 / 38400 / 8N2 / no flow control

机型识别:
  ID; -> ID0800;

常用读写:
  FA / FB / MD0 / MD1 / PC / NR0 / RL0 均可用

USB Audio:
  Windows 可枚举并获取接收音频
```

下一步 ESP32-P4 固件应优先复现 CAT 链路：

1. Type-A USB Host 枚举 FT-710。
2. 识别 CP2105 的 Enhanced 与 Standard 两个接口。
3. 只认领 Enhanced 接口。
4. 配置 38400、8N2、无流控。
5. 发送 `ID; FA; MD0; PC;` 并解析返回。
6. 暂不在 ESP32-P4 首版中实现 USB Audio Host，避免把 CAT 控制和音频类驱动风险混在同一阶段。

## 12. 已验证命令清单

| 功能 | 命令 | 实测状态 |
| --- | --- | --- |
| 机型识别 | `ID;` | 返回 `ID0800;` |
| VFO-A 频率读取 | `FA;` | 正常 |
| VFO-A 频率写入 | `FA014270000;` | 正常 |
| VFO-B 频率读取 | `FB;` | 正常 |
| VFO-B 频率写入 | `FB021400000;` | 正常 |
| VFO-A 模式读取 | `MD0;` | 正常 |
| VFO-A USB 设置 | `MD02;` | 正常 |
| VFO-B 模式读取 | `MD1;` | 正常 |
| VFO-B USB 设置 | `MD12;` | 正常 |
| 功率设定读取 | `PC;` | 正常 |
| DNR 开关读取 | `NR0;` | 正常 |
| DNR 打开 | `NR01;` | 正常 |
| DNR 等级读取 | `RL0;` | 正常 |
| DNR 等级设置 | `RL007;` | 正常 |
| 固件版本读取 | `VE0;` ~ `VE3;` | 正常 |

