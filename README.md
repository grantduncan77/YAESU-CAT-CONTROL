# FT-710 CAT Control

YAESU FT-710 external touch controller based on the Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B.

This repository contains the current ESP-IDF/LVGL firmware and the hardware probe projects created while validating the CAT communication path.

## Goal

The project builds a 7-inch landscape touch panel for controlling common FT-710 CAT functions:

- VFO-A / VFO-B frequency display
- Direct frequency input
- VFO-A / VFO-B input target selection
- A/B working VFO switch
- Mode selection
- Band default-frequency write
- RF Power control
- RF Power value mirrored to a 0.96-inch SSD1306 auxiliary OLED through TCA9548A channel 3
- DNR on/off and level control
- External EC11 encoder frequency and RF Power input through MCP23017 over I2C
- CAT / BT / WiFi / RX status bar
- WiFi setup page
- Network time display with local / UTC toggle

The first version intentionally does not implement PTT or transmit control.

## Hardware

- Board: Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B
- Display: 7-inch 1024 x 600 MIPI-DSI LCD
- Touch: GT911 capacitive touch
- MCU: ESP32-P4
- Wireless coprocessor: onboard ESP32-C6
- Current CAT link: ESP32-P4 USB Host -> external CH9102 USB-TTL -> FT-710 CAT-3 TTL UART
- Optional input: EC11 rotary encoders with buttons -> Waveshare MCP23017 I/O expander -> ESP32-P4 I2C
- Auxiliary test display: 0.96-inch 128 x 64 SSD1306 I2C OLED through a Qwiic Mux Breakout TCA9548A, channel 3

The current EC11 test wiring is: frequency encoder A -> MCP23017 PA0, B -> PA1, S/button -> PA2; RF Power encoder A -> PA6, B -> PA7, S/button -> PB0. The firmware enables MCP23017 pull-ups on those inputs and auto-scans I2C addresses `0x20` through `0x27`; the tested Waveshare module was detected at `0x27`.

The current OLED/Mux test wiring is: ESP32-P4 I2C1 `SDA=GPIO7`, `SCL=GPIO8`; TCA9548A default address `0x70`; SSD1306 OLED on mux channel 3, detected at `0x3C`.

The FT-710 rear USB direct CP2105 route was tested, but ESP-IDF v5.5.5 currently cannot enumerate the FT-710 downstream CP2105 through the radio's USB hub because the required Hub TT path is not supported for this use case. The first working route is therefore the external CH9102 USB-TTL adapter connected to the FT-710 CAT-3 port.

## Software Baseline

- ESP-IDF: v5.5.5
- Target: `esp32p4`
- UI: LVGL 9
- Main board serial port used during development: `COM4`
- Helper script: `idf-v5.5.5.ps1`

Activate the local ESP-IDF environment:

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"
```

## Repository Layout

- `ft710_controller/`  
  Current main firmware. Includes LVGL touch UI, CH9102 USB-TTL CAT transport, command queue, VFO/mode/power/DNR controls, WiFi setup page, soft keyboard, time display, MCP23017 EC11 input, and the auxiliary SSD1306 RF Power display behind TCA9548A channel 3.

- `ft710_ch9102_usb_probe/`  
  ESP32-P4 USB Host probe for the external CH9102 USB-TTL to FT-710 CAT-3 path.

- `ft710_usb_probe/`  
  FT-710 rear USB direct CP2105 probe. This documents the failed direct USB route and the Hub/TT limitation.

- `ft710_ji1fgx_probe/`  
  JI1FGX Yaesu CP2105 ESP32-S3 Hub/CP2105 experiment. This includes a local copy of the modified `espressif__usb` component used for the test.

- `ft710_uart_probe/`  
  Direct UART CAT-3 probe.

- `oled_i2c_test/`  
  Standalone ESP-IDF test for the 0.96-inch SSD1306 OLED and MCP23017 RF Power encoder over the ESP32-P4 I2C bus. It selects TCA9548A channel 3 for the OLED and shows the power value in large text while the RF Power EC11 is rotated.

## Build And Flash

Build the main firmware:

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"
cd "D:\CAT CONTROL\ft710_controller"
idf.py build
```

Flash to the ESP32-P4 board:

```powershell
idf.py -p COM4 flash
```

Optional monitor:

```powershell
idf.py -p COM4 monitor
```

`build/` and normal ESP-IDF `managed_components/` directories are intentionally ignored. Dependencies are restored by ESP-IDF from `idf_component.yml` and `dependencies.lock`.

## Verified Results

Development and hardware tests have confirmed:

- ESP32-P4 rev v1.3 boots with ESP-IDF v5.5.5.
- PSRAM, MIPI-DSI display, EK79007 LCD panel, GT911 touch, and LVGL task initialize correctly.
- FT-710 CAT over a Windows Enhanced COM port works as a baseline.
- FT-710 USB Audio input can be received on Windows.
- ESP32-P4 can open the external CH9102 USB-TTL adapter as `VID:PID 1A86:55D4`.
- CH9102 + FT-710 CAT-3 works at `38400 8N1`.
- CAT reads confirmed: `ID;`, `FA;`, `FB;`, `MD0;`, `MD1;`.
- CAT writes confirmed: VFO-A/VFO-B frequency, VFO-A/VFO-B mode, DNR on, DNR level.
- Dual VFO frequency polling around 100 ms is usable.
- Main control UI runs on the 1024 x 600 touch screen.
- RF Power and DNR now use immediate local UI update plus CAT write and later readback correction.
- MCP23017 + EC11 external encoders are detected over I2C. The frequency encoder adjusts the selected VFO input in 1 kHz steps and sends it on button press; the RF Power encoder sends `PCxxx;` in 1W steps.
- Standalone OLED/Mux/encoder test works: TCA9548A detected at `0x70`, OLED detected at `0x3C` behind channel 3, MCP23017 detected at `0x27`, and rotating the RF Power encoder updates the 0.96-inch OLED power display in 1W steps.
- Main firmware integration of the auxiliary OLED works at startup: `Aux OLED ready: TCA9548A=0x70 channel=3 SSD1306=0x3C`. The OLED follows the main `power_w` state, so touch power changes, encoder changes, and CAT readback corrections can update the small display.
- Mode buttons now follow the selected A/B input target, so VFO-A sends `MD0x;` and VFO-B sends `MD1x;`.
- WiFi setup page starts ESP-Hosted WiFi, scans 2.4 GHz APs, and presents a scrollable AP list.
- Soft keyboard control keys were fixed: `CLEAR`, `BACK`, and `SPACE` now behave correctly.

## WiFi Notes

ESP32-P4 does not include a WiFi radio. On this board, WiFi is provided by the onboard ESP32-C6 through ESP-Hosted / `esp_wifi_remote`.

Current application mode:

- `WIFI_MODE_STA`
- Connects to a 2.4 GHz router/AP
- Does not implement AP mode
- Does not implement WiFi host behavior

After WiFi and SNTP are available, the status bar can display time. Touching the time area toggles local time and UTC time.

## UI Design Notes

The UI follows a dark radio-console style:

- landscape 1024 x 600 layout
- cyan highlight color
- thin bordered panels
- large frequency readouts
- large touch targets
- persistent top status bar

The main screen contains:

- status bar
- dual VFO frequency panel
- direct frequency keypad
- RF Power panel
- DNR panel
- Mode panel
- Band panel

The WiFi page contains:

- left scrollable AP scan list
- right connection status: SSID, IP, gateway, DNS, status
- right config inputs: SSID, password, connect, disconnect
- separate soft keyboard page for text entry

## Safety Boundary

The first version is deliberately conservative:

- no PTT
- no transmit automation
- no automatic band/mode coupling in the UI
- CAT write actions should be corrected by radio readback

## Known Limitations

- `ft710_controller/components/app_controller/app_controller.c` currently contains UI, CAT, WiFi, time, and state logic together. It should be split into smaller modules.
- CH9102 hotplug and reconnect handling needs more work.
- EC11 direction, step size, and optional button shortcuts may need final operator tuning after longer hands-on use.
- WiFi connection failure handling, saved credential UX, and reconnect flow need more polish.
- AP scan cache currently stores a limited number of AP entries.
- FT-710 rear USB direct CP2105 remains blocked by the Hub/TT path in the tested ESP-IDF stack.
- Band buttons currently write project-defined default frequencies, not FT-710 Band Stack entries.
- Long-duration stability and RF-environment testing still need to be completed.

## Suggested Next Steps

Planned refactor:

- `cat_transport_ch9102`
- `ft710_cat`
- `radio_state`
- `ui_main`
- `ui_wifi`
- `ui_keyboard`
- `wifi_manager`
- `time_service`
- `settings`
- `diagnostics`

Test priorities:

- CH9102 reconnect and hotplug
- EC11 long-press / step-size / VFO target shortcuts
- WiFi connect/disconnect/reconnect
- 30-minute and longer stability tests
- pending/confirmed CAT command UI state
- real operating-position touch usability
