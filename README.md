# FT-710 CAT Control

[中文说明](README.zh-CN.md)

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
- VFO / RF Power / DNR / WIDTH values mirrored to 0.96-inch SSD1306 auxiliary OLEDs through TCA9548A
- DNR on/off and level control
- External EC11 encoder inputs for frequency, RF Power, DNR, and WIDTH through MCP23017 over I2C
- Five configurable auxiliary SSD1306 OLED display slots through TCA9548A
- Slot-based hardware/function binding for the five external knob + OLED groups
- Touch `MENU` configuration page for assigning functions to the five external knob/OLED groups and the four right-side main-screen function panels
- Configurable function set currently includes RF Power, DNR Level, WIDTH, Band, Mode, Noise Blanker, Notch, and Mic Gain
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
- Auxiliary displays: 0.96-inch 128 x 64 SSD1306 I2C OLEDs through a Qwiic Mux Breakout TCA9548A

The current five EC11 wiring baseline is:

- Knob 1: A -> MCP23017 PA0, B -> PA1, S/button -> PA2
- Knob 2: A -> PA3, B -> PA4, S/button -> PA5
- Knob 3: A -> PA6, B -> PA7, S/button -> PB1
- Knob 4: A -> PB7, B -> PB6, S/button -> PB5
- Knob 5: A -> PB4, B -> PB3, S/button -> PB2

The firmware enables MCP23017 pull-ups on those inputs, uses 100 kHz I2C for the auxiliary I2C devices, prioritizes the tested MCP23017 address `0x27`, and only accepts an address after the MCP23017 registers can be configured successfully.

The current OLED/Mux wiring is: ESP32-P4 I2C1 `SDA=GPIO7`, `SCL=GPIO8`; TCA9548A default address `0x70`; SSD1306 OLED address `0x3C`. OLED 1 is on channel 3, OLED 2 is on channel 2, OLED 3 is on channel 4, OLED 4 is on channel 5, and OLED 5 is on channel 6.

The current hardware groups are:

- Group 1: Knob 1 + OLED 1, currently used for frequency input/display
- Group 2: Knob 2 + OLED 2, currently used for DNR
- Group 3: Knob 3 + OLED 3, currently used for RF Power
- Group 4: Knob 4 + OLED 4, currently used for WIDTH
- Group 5: Knob 5 + OLED 5, reserved for the upcoming configurable function slot. For hardware validation, rotation increments/decrements a visible OLED counter and the button resets it to zero.

The firmware now describes those groups with a `control_slot_t` table. Each slot owns its MCP23017 A/B/button pins, an OLED pointer, and a function enum such as frequency, DNR, RF Power, WIDTH, or slot test. This is the first step toward the planned on-device configuration UI.

The firmware also includes an initial `s_feature_catalog[]` registry for planned functions. It records the CAT command, display label, read/write access, recommended UI surfaces, confirmation needs, and implementation status. The current `MENU` page uses that direction to configure the five external knob/OLED slots and the four main-screen function panels. The first configurable set contains RF Power, DNR Level, WIDTH, Band, Mode, Noise Blanker, Notch, and Mic Gain.

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
  Current main firmware. Includes LVGL touch UI, CH9102 USB-TTL CAT transport, command queue, VFO/mode/power/DNR/WIDTH controls, WiFi setup page, soft keyboard, time display, MCP23017 EC11 input, and auxiliary SSD1306 displays behind TCA9548A channels 2/3/4.

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

- `esp32_devkitc_tests/`  
  ESP32-DevKitC-32E WiFi/BLE/CAT bridge experiments. This includes the earlier WiFi scan and BLE UART validation work, plus the CAT-3 wireless bridge prototype. The DevKitC bridge phase is currently paused because the user reported hardware trouble.

- `esp32_c6_hosted_slave_recovery_20260830/`  
  ESP-Hosted slave recovery/reference work used while restoring the ESP32-P4 board's onboard ESP32-C6 WiFi path.

- `docs/obsidian/`  
  Snapshot of the Obsidian project notes, including planning, test logs, UI decisions, wiring notes, screenshots, and the latest 2026-08-30/31 external encoder and OLED record.

## Build And Flash

Build the main firmware:

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"
cd "D:\CAT CONTROL\ft710_controller"
idf.py -B build_v555 build
```

Flash to the ESP32-P4 board:

```powershell
idf.py -B build_v555 -p COM4 flash
```

Optional monitor:

```powershell
idf.py -B build_v555 -p COM4 monitor
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
- The right-side main-screen panels are now configurable from the existing top-right `MENU` label. The same page can also reassign the five external knob/OLED groups.
- MCP23017 + EC11 external encoders are detected over I2C at the verified `0x27` address. The frequency encoder adjusts the selected VFO input in 1 kHz steps and sends it on button press; the RF Power encoder is globally available and supports selectable `2W/5W/10W` steps from its push button; the DNR encoder adjusts DNR and its button turns DNR off; the WIDTH encoder adjusts `SH00xx;` and its button sends the WIDTH default.
- Standalone OLED/Mux/encoder test works: TCA9548A detected at `0x70`, OLED detected at `0x3C` behind channel 3, MCP23017 detected at `0x27`, and rotating the RF Power encoder updates the 0.96-inch OLED power display in 1W steps.
- Main firmware integration of the auxiliary OLEDs works from a background task, so touch changes, encoder changes, and CAT readback corrections can update the small displays without blocking UI refresh. The latest hardware baseline uses five OLED slots on TCA9548A channels `3/2/4/5/6`.
- Main firmware RF Power encoder verification after the OLED integration fix: `Power encoder set 14W..18W` and CAT `PC014..PC018` all returned `ESP_OK`.
- Mode buttons now follow the selected A/B input target, so VFO-A sends `MD0x;` and VFO-B sends `MD1x;`.
- WIDTH display now maps the FT-710 `SH` index to actual bandwidth values. `00` is shown as `DEF`; valid mode-specific values show Hz numbers such as `400`, `800`, or `3500`; invalid mode/index combinations show `---`.
- WIDTH OLED rendering now uses a variable trapezoid, extended bottom baseline, cross-hatched fill, and larger mapped Hz text below the baseline.
- RF Power OLED rendering now uses a smaller upper power readout plus a bottom `5-100W` progress bar. DNR and RF Power OLEDs no longer show the old lower-left `FT-710` label.
- WiFi setup page starts ESP-Hosted WiFi, scans 2.4 GHz APs, and presents a scrollable AP list.
- Soft keyboard control keys were fixed: `CLEAR`, `BACK`, and `SPACE` now behave correctly.
- CAT support has been extended in the configurable framework for Noise Blanker (`NB/NL`), Notch (`BC/BP`), and Mic Gain (`MG`). These are ready for FT-710 hardware verification.

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
- The DevKitC BLE/WiFi CAT bridge prototype is paused pending hardware recovery or replacement.
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
