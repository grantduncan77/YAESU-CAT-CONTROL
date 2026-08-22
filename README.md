# FT-710 CAT Control

ESP-IDF/LVGL project and hardware probes for a YAESU FT-710 external touch controller on the Waveshare ESP32-P4-WIFI6-LCD-TOUCH-7B.

## Projects

- `ft710_controller/` - current main firmware: LVGL touch UI, CH9102 USB-TTL CAT transport, WiFi setup page, time display, VFO/mode/power/DNR controls.
- `ft710_ch9102_usb_probe/` - ESP32-P4 USB Host probe for the external CH9102 USB-TTL to FT-710 CAT-3 path.
- `ft710_usb_probe/` - FT-710 rear USB direct CP2105 probe.
- `ft710_ji1fgx_probe/` - JI1FGX/Hub/CP2105 experiment with a local USB component copy.
- `ft710_uart_probe/` - direct UART CAT-3 probe.

## Environment

- ESP-IDF: v5.5.5
- Target: `esp32p4`
- Board serial port used during development: `COM4`
- Helper script: `idf-v5.5.5.ps1`

Typical build:

```powershell
. "D:\CAT CONTROL\idf-v5.5.5.ps1"
cd "D:\CAT CONTROL\ft710_controller"
idf.py build
idf.py -p COM4 flash
```

`build/` and normal ESP-IDF `managed_components/` directories are intentionally ignored. Dependencies are restored by ESP-IDF from the project manifests and lock files.
