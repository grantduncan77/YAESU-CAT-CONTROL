$env:IDF_PATH = "C:\esp\v5.5.5\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python_env\idf5.5_py3.12_env"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011"

$toolPaths = @(
    "C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    "C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin",
    "C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin",
    "C:\Espressif\tools\cmake\3.30.2\bin",
    "C:\Espressif\tools\ninja\1.12.1",
    "C:\Espressif\tools\idf-exe\1.0.3",
    "C:\Espressif\tools\ccache\4.12.1",
    "C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64",
    "C:\Espressif\tools\esp-rom-elfs\20241011",
    "C:\Espressif\tools\python_env\idf5.5_py3.12_env\Scripts",
    "C:\esp\v5.5.5\esp-idf\tools"
)

$env:PATH = ($toolPaths -join ";") + ";" + $env:PATH
Write-Host "ESP-IDF v5.5.5 ESP32 environment ready."
