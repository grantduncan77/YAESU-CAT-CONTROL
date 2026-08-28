# ESP32-DevKitC-32E BLE CAT-3 桥接前置测试记录

日期：2026-08-28  
阶段目标：开始实现 ESP32-P4 控制端与 FT-710 CAT-3 之间，通过 ESP32-DevKitC-32E 转换后以 BLE 方式无线连接。

## 本次用户要求

- 目标变更：ESP32-P4 控制端与 CAT-3 之间加入 ESP32-DevKitC，使用 BLE 做无线桥接。
- 当前任务：ESP32-DevKitC-32E 已经连接到本电脑，需要查阅模块相关开发文档，检查电脑与模块连接；如连接正常，测试模块工作状态，并可调用本机蓝牙功能测试模块蓝牙功能和 WiFi 功能。

## 查阅的开发文档

- ESP32-DevKitC 官方用户指南：说明 DevKitC 是面向 ESP32-WROOM/ESP32-WROVER 系列模块的开发板，通常只需要开发板、USB 2.0 A to Micro-B 数据线和电脑即可开始开发；模块绝大多数 I/O 已经引出到两侧排针。
- ESP32-DevKitC 官方产品页：确认该开发板集成 ESP32 模组，具备 Wi-Fi 与 Bluetooth 能力。
- ESP32-WROOM-32E / ESP32-WROOM-32UE 数据手册：用于确认 ESP32-WROOM-32E 模块能力、供电和射频基础信息。
- ESP32 SoC 官方产品页：确认 ESP32 SoC 集成 Wi-Fi 与 Bluetooth，可作为独立系统或从机工作。
- ESP-IDF ESP32 Getting Started：确认 ESP-IDF 对 ESP32 目标的构建、烧写、监视流程。

参考链接：

- https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html
- https://www.espressif.com/en/products/devkits/esp32-devkitc
- https://documentation.espressif.com/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.pdf
- https://www.espressif.com/en/products/socs/esp32
- https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html

## 电脑连接检查

Windows 串口枚举结果：

```text
DeviceID: COM8
Name: Silicon Labs CP210x USB to UART Bridge (COM8)
PNPDeviceID: USB\VID_10C4&PID_EA60\F81FC8FBCDBDF0118D9FBD7948E9DE0F
```

ESP32 芯片识别：

```text
Chip is ESP32-D0WD-V3 (revision v3.1)
Features: WiFi, BT, Dual Core, 240MHz, VRef calibration in efuse, Coding Scheme None
Crystal is 40MHz
MAC: 44:1d:64:4f:91:00
```

Flash 识别：

```text
Manufacturer: 46
Device: 4016
Detected flash size: 4MB
Flash voltage set by a strapping pin to 3.3V
```

结论：电脑通过 CP210x USB-UART 正常连接到 ESP32-DevKitC-32E，串口为 COM8；芯片、MAC、Flash 均可由 esptool 正常读取。

## ESP-IDF 环境检查与处理

本机 ESP-IDF 版本：`C:\esp\v5.5.5\esp-idf`，用于 ESP32-P4 项目的辅助激活脚本为：

```text
D:\CAT CONTROL\idf-v5.5.5.ps1
```

发现的问题：

- 原环境主要用于 ESP32-P4，切换到经典 ESP32 目标时，`xtensa-esp32-elf-gcc` 未进入 PATH。
- `idf_tools.py check` 在未正确加载环境时报告多个工具缺失。
- 直接运行官方激活脚本时，`xtensa-esp-elf` 与 `esp32ulp-elf` 版本登记异常，自动 export 失败。
- GitHub 工具链重新下载速度过慢，因此没有等待完整重装。

临时修复方式：

- 明确设置 `IDF_PATH`、`IDF_TOOLS_PATH`、`IDF_PYTHON_ENV_PATH`。
- 手工把已有工具链加入 PATH：
  - `C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin`
  - `C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin`
  - `C:\Espressif\tools\cmake\3.30.2\bin`
  - `C:\Espressif\tools\ninja\1.12.1`
  - `C:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts`
  - `C:\esp\v5.5.5\esp-idf\tools`
- 设置 `ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011`，避免 GDB ROM ELF 环境变量缺失警告。
- 已新增本项目辅助脚本：

```text
D:\CAT CONTROL\esp32_devkitc_tests\idf-esp32-env.ps1
```

用途：专门为 ESP32-DevKitC-32E 测试加载 ESP-IDF v5.5.5、Xtensa ESP32 工具链、CMake、Ninja、Python 虚拟环境和 ROM ELF 路径。

结论：手动环境变量方式下，ESP32 目标编译、烧写均可用。后续建议把 ESP32 与 ESP32-P4 的 IDF 激活脚本分开，避免目标工具链 PATH 互相影响。

## WiFi 功能测试

测试工程：

```text
D:\CAT CONTROL\esp32_devkitc_tests\wifi_scan
```

来源：

```text
C:\esp\v5.5.5\esp-idf\examples\wifi\scan
```

操作：

- 构建目标：ESP32
- 编译：成功
- 烧写端口：COM8
- 烧写：成功
- 串口速率：115200
- 固件启动：成功

关键日志：

```text
Project name:     scan
ESP-IDF:          v5.5.5
wifi:mode : sta (44:1d:64:4f:91:00)
scan: Total APs scanned = 22, actual AP number ap_info holds = 10
```

扫描到的部分 AP：

```text
DIRECT-4e-HP M281 LaserJet  RSSI -46  Channel 6
LEADCHAMP                  RSSI -50  Channel 6
2336                       RSSI -50  Channel 11
ChinaNet-6Ks6              RSSI -53  Channel 3
LY                         RSSI -54  Channel 1
xrt                        RSSI -54  Channel 9
360WiFi-9520DE             RSSI -55  Channel 11
2202anqi                   RSSI -61  Channel 11
LC-MON                     RSSI -64  Channel 1
```

结论：ESP32-DevKitC-32E 的 WiFi 射频、驱动、SDK 初始化和串口日志链路均工作正常。

## BLE 功能测试

测试工程：

```text
D:\CAT CONTROL\esp32_devkitc_tests\ble_uart_service_20260828
```

来源：

```text
C:\esp\v5.5.5\esp-idf\examples\bluetooth\ble_uart_service
C:\esp\v5.5.5\esp-idf\examples\bluetooth\common\ble_uart
```

选择该例程的原因：

- 后续 CAT-3 无线桥接本质上是 UART 字节流通过 BLE GATT 传输。
- `ble_uart_service` 使用 Nordic UART Service 风格 UUID，具备 RX write 与 TX notify 两个方向，最贴近 CAT 命令/响应透传。

BLE UART UUID：

```text
Service: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
RX:      6e400002-b5a3-f393-e0a9-e50e24dcca9e
TX:      6e400003-b5a3-f393-e0a9-e50e24dcca9e
```

首次官方默认配置：

- `.encrypted = true`
- BLE 广播成功
- PC 端可连接，但订阅 TX notify 时返回 `Insufficient Authentication`
- 这是预期行为，因为官方例程默认要求加密/配对

测试修正：

```c
.encrypted = false
```

修正目的：用于本机自动化功能验证，避免 Windows 配对弹窗阻塞测试。正式桥接时再决定是否启用配对/加密。

BLE 固件启动关键日志：

```text
Project name:     ble_uart_service
ESP-IDF:          v5.5.5
Bluetooth MAC: 44:1d:64:4f:91:02
ble_uart: BLE host task started
ble_uart: registered service 6e400001-b5a3-f393-e0a9-e50e24dcca9e handle=14
ble_uart: registered chr 6e400002-b5a3-f393-e0a9-e50e24dcca9e def=15 val=16
ble_uart: registered chr 6e400003-b5a3-f393-e0a9-e50e24dcca9e def=17 val=18
ble_uart: advertising as 'BleUart-9102'
```

电脑端蓝牙扫描：

```text
44:1D:64:4F:91:02  name='BleUart-9102'  rssi=-44  uuids=6e400001-b5a3-f393-e0a9-e50e24dcca9e
FOUND_BLE_UART=YES
```

电脑端 BLE UART 回显测试：

```text
TARGET_FOUND=YES address=44:1D:64:4F:91:02 name=BleUart-9102
CONNECTED=YES
NOTIFY 4341542d424c452d544553540a CAT-BLE-TEST
ECHO_OK=YES
```

结论：ESP32-DevKitC-32E 的 BLE 广播、PC 侧扫描、连接、GATT 写入、TX notify 回传均正常。该模块可作为 CAT-3 BLE 无线桥接端继续开发。

## 当前总体结论

- DevKitC 与电脑连接正常：COM8 / CP210x / esptool 可读芯片与 Flash。
- DevKitC 模块确认是 ESP32-D0WD-V3，具备 WiFi 与 BT。
- WiFi 功能已通过官方 WiFi scan 实测。
- BLE 功能已通过官方 BLE UART 服务与 PC 端 `bleak` 扫描、连接、写入、回显实测。
- ESP32 经典目标的工具链文件存在，但 IDF 自动 export 的版本发现/激活有问题；当前可用手工 PATH 方式继续开发。

## 下一步建议

1. 建立正式 `esp32_cat3_ble_bridge` 工程。
2. DevKitC 端实现：
   - UART2 接 CAT-3 TTL，默认 `38400 8N1`
   - BLE UART/NUS GATT 服务
   - RX characteristic：接收 ESP32-P4 控制端发来的 CAT 命令并写入 CAT-3 UART
   - TX notify：把 CAT-3 回应回传给 ESP32-P4
   - 简单连接状态日志与可选心跳
3. ESP32-P4 控制端实现 BLE central/client：
   - 扫描并连接 DevKitC 广播名
   - 订阅 TX notify
   - 把现有 CAT 收发层替换/抽象为 BLE UART transport
4. 安全策略：
   - 开发阶段先用明文 BLE UART 降低调试复杂度
   - 稳定后再评估 BLE pairing/bonding/encryption
5. 工具链：
   - 建议创建独立 `idf-v5.5.5-esp32.ps1`，专门加载 ESP32 经典 Xtensa 工具链，避免与 ESP32-P4 RISC-V 工具链互相干扰。
