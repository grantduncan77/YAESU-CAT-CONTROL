---
title: 主程序集成 TCA9548A OLED 功率显示
tags:
  - ESP32-P4
  - FT-710
  - OLED
  - SSD1306
  - TCA9548A
  - MCP23017
  - EC11
  - I2C
created: 2026-08-29
---

# 主程序集成 TCA9548A OLED 功率显示

## 用户要求

在独立测试工程 `oled_i2c_test` 已经验证成功后，用户要求：

> 好的，回到主程序把这个功能加进去

目标是将 “Qwiic Mux Breakout-8 Channel TCA9548A channel 3 上的 0.96 寸 SSD1306 OLED” 与主程序中的 RF Power 状态联动：

- OLED 通过 TCA9548A channel 3 访问
- TCA9548A 默认地址 `0x70`
- OLED 地址 `0x3C`
- 功率旋钮仍使用 MCP23017 上的 EC11：
  - A：`PA6`
  - B：`PA7`
  - S/button：`PB0`
- 当功率发生变化时，小屏用大字显示功率数值，例如 `46W`

## 修改文件

主程序文件：

- `D:\CAT CONTROL\ft710_controller\components\app_controller\app_controller.c`

同步说明：

- `D:\CAT CONTROL\README.md`
- `D:\OBSIDIAN\GPT CODING\CAT Control\21_主程序集成TCA9548A_OLED功率显示.md`
- `D:\CAT CONTROL\docs\obsidian\21_主程序集成TCA9548A_OLED功率显示.md`

## 集成方式

主程序原本已有：

- MCP23017 自动扫描 `0x20..0x27`
- 频率旋钮 `PA0/PA1/PA2`
- 功率旋钮 `PA6/PA7/PB0`
- 功率旋钮每格 1W
- `encoder_adjust_power()` 中立即更新 `s_state.power_w` 并发送 CAT `PCxxx;`
- `poll_one()` 解析 `PC;` 读回并修正 `s_state.power_w`
- `update_ui_locked()` 刷新 7 寸主屏功率显示

本次没有重写旋钮逻辑，而是在主程序中加入辅助 OLED 层：

- 新增 TCA9548A 设备句柄
- 新增 SSD1306 OLED 设备句柄
- 新增 128 x 64 单色帧缓冲
- 新增 TCA9548A channel 3 选通函数
- 新增 SSD1306 初始化与整屏刷新函数
- 新增 5x7 字库和大字功率绘制函数
- 新增 I2C mutex，避免 MCP23017 读写与 OLED 刷新同时占用 I2C 总线

## 关键实现

新增定义：

```c
#define AUX_OLED_I2C_HZ 100000
#define AUX_OLED_WIDTH 128
#define AUX_OLED_HEIGHT 64
#define AUX_OLED_ADDR_PRIMARY 0x3C
#define AUX_OLED_ADDR_SECONDARY 0x3D
#define TCA9548A_ADDR 0x70
#define TCA9548A_OLED_CHANNEL 3
```

新增 I2C 互斥锁：

```c
static SemaphoreHandle_t s_i2c_mutex;
```

用途：

- `mcp23017_read_reg()`
- `mcp23017_write_reg()`
- `aux_oled_show_power()`

这可以避免功率旋钮读取和 OLED 整屏刷新同时操作 I2C。

TCA9548A channel 3 选通：

```c
static esp_err_t aux_oled_select(void)
{
    const uint8_t mask = BIT(TCA9548A_OLED_CHANNEL);
    return i2c_master_transmit(s_tca9548a_dev, &mask, 1, 1000);
}
```

每次向 SSD1306 写命令或数据前都会选通 channel 3。

OLED 功率页：

- 顶部：`RF POWER`
- 中央：大字号 `xxW`
- 底部：`FT-710`

刷新入口：

```c
static void aux_oled_show_power(uint8_t power_w, bool force)
```

该函数会记住上次显示值：

- 如果 `power_w` 没变化，不刷新
- 如果初始化失败，直接返回
- 如果 I2C 忙，跳过本次刷新

因此主程序可以安全地在 `update_ui_locked()` 中调用：

```c
aux_oled_show_power(s_state.power_w, false);
```

这样小 OLED 跟随主状态 `s_state.power_w`，不仅旋钮变化会显示，触摸按钮变化和 CAT `PC;` 读回修正也会显示。

## 启动流程

在 `app_controller_start()` 中：

1. 初始化 `s_i2c_mutex`
2. 启动 7 寸主屏 LVGL UI
3. 调用 `aux_oled_init()`
4. 如果成功，强制显示一次当前功率
5. 如果失败，仅打印 warning 并禁用辅助 OLED，不影响主控制器继续启动
6. 启动 WiFi、encoder、CAT 任务

失败保护：

```c
esp_err_t aux_err = aux_oled_init();
if (aux_err == ESP_OK) {
    aux_oled_show_power(s_state.power_w, true);
} else {
    ESP_LOGW(TAG, "Aux OLED disabled: %s", esp_err_to_name(aux_err));
}
```

## 编译与烧写验证

构建命令：

```powershell
cd "D:\CAT CONTROL\ft710_controller"
idf.py build
```

由于本机 `export.ps1` 仍会误报 xtensa/ulp 工具缺失，实际使用手动 ESP-IDF 环境变量调用：

```powershell
python C:\esp\v5.5.5\esp-idf\tools\idf.py build
```

构建结果：

```text
Project build complete.
ft710_controller.bin binary size 0x1864f0 bytes.
Smallest app partition is 0x800000 bytes.
0x679b10 bytes (81%) free.
```

烧写命令：

```powershell
idf.py -p COM4 flash monitor
```

烧写结果：

```text
Wrote 1598704 bytes ... Hash of data verified.
Hard resetting via RTS pin...
```

启动验证日志：

```text
I ft710_controller: Aux OLED ready: TCA9548A=0x70 channel=3 SSD1306=0x3C
I ft710_controller: MCP23017 encoder detected at 0x27, freq A=PA0 B=PA1 S=PA2 step=1000 Hz, power A=PA6 B=PA7 S=PB0 step=1 W
I ft710_controller: Waiting for CH9102 1A86:55D4
I ft710_controller: CH9102 opened, FT-710 control UI running
```

结论：

- 主程序中 TCA9548A 初始化成功
- 主程序中 channel 3 上 SSD1306 OLED 初始化成功
- MCP23017 功率旋钮仍正常识别
- 主程序构建、烧写、启动均通过
- 小 OLED 功率显示已接入主状态 `s_state.power_w`

## 当前注意事项

- 监听窗口内没有再次捕获到旋钮变化日志，原因可能是测试窗口期间未转动旋钮；独立测试中已验证旋钮变化可驱动 OLED 大字显示。
- 主程序中小 OLED 刷新跟随 `s_state.power_w`，理论上覆盖三种来源：
  - RF Power 触摸按钮
  - MCP23017 功率旋钮
  - FT-710 CAT `PC;` 读回
- 当前辅助 OLED 初始化失败不会影响主程序运行。
- ESP-Hosted WiFi 在本次日志中仍出现从机连接失败信息，此问题与本次 OLED 集成无直接阻断关系，但后续若同时长期使用 WiFi 与 I2C 外设，需要继续观察 GPIO/I2C/SPI 资源冲突和启动时序。
