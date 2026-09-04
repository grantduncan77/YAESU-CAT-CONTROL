$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
chcp 65001 | Out-Null

$env:ESP_IDF_VERSION = "5.5"
$env:IDF_PATH = "C:/esp/v5.5.5/esp-idf"
$env:IDF_TOOLS_PATH = "C:/Espressif/tools"
$env:IDF_PYTHON_ENV_PATH = "C:/Espressif/tools/python_env/idf5.5_py3.12_env"
$env:ESP_ROM_ELF_DIR = "C:/Espressif/tools/esp-rom-elfs/20241011"

$idfToolPaths = @(
    "$env:IDF_PYTHON_ENV_PATH/Scripts",
    "$env:IDF_TOOLS_PATH/cmake/3.30.2/bin",
    "$env:IDF_TOOLS_PATH/ninja/1.12.1",
    "$env:IDF_TOOLS_PATH/ccache/4.11.1",
    "$env:IDF_TOOLS_PATH/riscv32-esp-elf/esp-14.2.0_20241119/riscv32-esp-elf/bin",
    "$env:IDF_TOOLS_PATH/riscv32-esp-elf-gdb/14.2_20240403/riscv32-esp-elf-gdb/bin",
    "$env:IDF_TOOLS_PATH/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin",
    "$env:IDF_TOOLS_PATH/esp32ulp-elf/2.38_20240113/esp32ulp-elf/bin",
    "$env:ESP_ROM_ELF_DIR",
    "$env:IDF_PATH/tools"
)

$env:Path = ($idfToolPaths -join ";") + ";" + $env:Path

function global:idf.py {
    & "$env:IDF_PYTHON_ENV_PATH/Scripts/python.exe" "$env:IDF_PATH/tools/idf.py" @args
}
