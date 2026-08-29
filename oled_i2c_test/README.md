# OLED I2C + Power Encoder Test

Standalone ESP-IDF test for a 0.96-inch 128x64 SSD1306 OLED and the MCP23017 RF Power encoder connected to the ESP32-P4 board I2C port.

Verified on the Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B board:

- I2C peripheral: `I2C1`
- SDA: `GPIO7`
- SCL: `GPIO8`
- Qwiic Mux: TCA9548A at `0x70`
- OLED mux channel: `3`
- OLED address: `0x3C`
- MCP23017 address: `0x27`
- RF Power encoder: A -> MCP23017 `PA6`, B -> `PA7`, S/button -> `PB0`
- Clock: `100 kHz`
- Power display range: `1W..100W`
- Power step: `1W`

## Wiring

- OLED `GND` -> board `GND`
- OLED `VCC` -> `3.3V`
- Qwiic Mux `SCL` -> ESP32-P4 I2C SCL, GPIO8 on the Waveshare BSP I2C header
- Qwiic Mux `SDA` -> ESP32-P4 I2C SDA, GPIO7 on the Waveshare BSP I2C header
- OLED -> Qwiic Mux channel `3`
- MCP23017 -> ESP32-P4 root I2C bus
- RF Power EC11 A -> MCP23017 `PA6`
- RF Power EC11 B -> MCP23017 `PA7`
- RF Power EC11 S/button -> MCP23017 `PB0`

The program probes the TCA9548A at `0x70`, selects channel `3`, probes the OLED at `0x3C` / `0x3D`, then scans the MCP23017 at `0x20..0x27`.

## Test Behavior

After boot, the display shows the current RF Power value in large text.

Rotating the RF Power EC11 changes the displayed value in `1W` steps. The encoder button is wired and pulled up, but this test does not assign a button action yet.

## Build

```powershell
cd "D:\CAT CONTROL\oled_i2c_test"
idf.py set-target esp32p4
idf.py -p COM4 flash monitor
```

Expected monitor output:

```text
I oled_i2c_test: TCA9548A detected at 0x70, OLED channel 3 selected
I oled_i2c_test: Scanning I2C1 on SDA GPIO7, SCL GPIO8 at 100000 Hz
I oled_i2c_test: I2C device found at 0x3C
I oled_i2c_test: MCP23017 detected at 0x27, power encoder A=PA6 B=PA7 S=PB0
I oled_i2c_test: SSD1306 OLED initialized at 0x3C through TCA9548A channel 3
I oled_i2c_test: Power knob display test started, initial power 46W
I oled_i2c_test: Power display changed to 47W
```

Note: the ESP32-P4 board used in this project reports chip revision `v1.3`; `sdkconfig` is set to allow ESP32-P4 revision `v1.0..v1.99`.
