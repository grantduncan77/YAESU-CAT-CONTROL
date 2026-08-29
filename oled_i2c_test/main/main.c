#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled_i2c_test";

#define I2C_PORT 1
#define I2C_SDA_GPIO 7
#define I2C_SCL_GPIO 8
#define I2C_FREQ_HZ 100000

#define SSD1306_WIDTH 128
#define SSD1306_HEIGHT 64
#define SSD1306_PAGES (SSD1306_HEIGHT / 8)
#define SSD1306_I2C_ADDR_PRIMARY 0x3C
#define SSD1306_I2C_ADDR_SECONDARY 0x3D

#define TCA9548A_ADDR 0x70
#define TCA9548A_OLED_CHANNEL 3

#define MCP23017_ADDR_MIN 0x20
#define MCP23017_ADDR_MAX 0x27
#define MCP23017_REG_IODIRA 0x00
#define MCP23017_REG_IODIRB 0x01
#define MCP23017_REG_GPPUA 0x0C
#define MCP23017_REG_GPPUB 0x0D
#define MCP23017_REG_GPIOA 0x12
#define MCP23017_REG_GPIOB 0x13
#define MCP23017_POWER_ENCODER_A_MASK (BIT(6) | BIT(7))
#define MCP23017_POWER_ENCODER_B_MASK BIT(0)
#define POWER_MIN_W 1
#define POWER_MAX_W 100
#define POWER_INITIAL_W 46

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_oled;
static i2c_master_dev_handle_t s_tca;
static i2c_master_dev_handle_t s_mcp;
static uint8_t s_frame[SSD1306_WIDTH * SSD1306_PAGES];

static esp_err_t tca_select(uint8_t channel)
{
    if (!s_tca || channel > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t mask = BIT(channel);
    return i2c_master_transmit(s_tca, &mask, 1, 1000);
}

static esp_err_t mcp_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_mcp, data, sizeof(data), 1000);
}

static esp_err_t mcp_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_mcp, &reg, 1, value, 1, 1000);
}

static esp_err_t oled_cmd(uint8_t cmd)
{
    const uint8_t data[2] = {0x00, cmd};
    ESP_RETURN_ON_ERROR(tca_select(TCA9548A_OLED_CHANNEL), TAG, "select oled mux channel");
    return i2c_master_transmit(s_oled, data, sizeof(data), 1000);
}

static esp_err_t oled_data(const uint8_t *data, size_t len)
{
    uint8_t line[1 + SSD1306_WIDTH] = {0x40};
    while (len) {
        const size_t chunk = len > SSD1306_WIDTH ? SSD1306_WIDTH : len;
        memcpy(&line[1], data, chunk);
        ESP_RETURN_ON_ERROR(tca_select(TCA9548A_OLED_CHANNEL), TAG, "select oled mux channel");
        ESP_RETURN_ON_ERROR(i2c_master_transmit(s_oled, line, chunk + 1, 1000), TAG, "oled data");
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t oled_flush(void)
{
    for (uint8_t page = 0; page < SSD1306_PAGES; ++page) {
        ESP_RETURN_ON_ERROR(oled_cmd(0xB0 | page), TAG, "page");
        ESP_RETURN_ON_ERROR(oled_cmd(0x00), TAG, "low col");
        ESP_RETURN_ON_ERROR(oled_cmd(0x10), TAG, "high col");
        ESP_RETURN_ON_ERROR(oled_data(&s_frame[page * SSD1306_WIDTH], SSD1306_WIDTH), TAG, "flush");
    }
    return ESP_OK;
}

static esp_err_t oled_init(void)
{
    const uint8_t init[] = {
        0xAE,       // display off
        0x20, 0x00, // horizontal addressing mode
        0xB0,
        0xC8,       // COM scan direction remapped
        0x00,
        0x10,
        0x40,
        0x81, 0x7F, // contrast
        0xA1,       // segment remap
        0xA6,       // normal display
        0xA8, 0x3F, // multiplex ratio 1/64
        0xA4,       // display follows RAM
        0xD3, 0x00, // display offset
        0xD5, 0x80, // clock divide
        0xD9, 0xF1, // pre-charge
        0xDA, 0x12, // COM pins
        0xDB, 0x40, // VCOMH
        0x8D, 0x14, // charge pump enable
        0xAF,       // display on
    };
    for (size_t i = 0; i < sizeof(init); ++i) {
        ESP_RETURN_ON_ERROR(oled_cmd(init[i]), TAG, "init");
    }
    memset(s_frame, 0, sizeof(s_frame));
    return oled_flush();
}

static void set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= SSD1306_WIDTH || y < 0 || y >= SSD1306_HEIGHT) {
        return;
    }
    uint8_t *byte = &s_frame[(y / 8) * SSD1306_WIDTH + x];
    const uint8_t bit = BIT(y % 8);
    if (on) {
        *byte |= bit;
    } else {
        *byte &= (uint8_t)~bit;
    }
}

static void draw_rect(int x, int y, int w, int h, bool on)
{
    for (int i = 0; i < w; ++i) {
        set_pixel(x + i, y, on);
        set_pixel(x + i, y + h - 1, on);
    }
    for (int i = 0; i < h; ++i) {
        set_pixel(x, y + i, on);
        set_pixel(x + w - 1, y + i, on);
    }
}

static const uint8_t *glyph_5x7(char c)
{
    static const uint8_t space[5] = {0, 0, 0, 0, 0};
    static const uint8_t minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0, 0, 0x60, 0x60, 0};
    static const uint8_t colon[5] = {0, 0x36, 0x36, 0, 0};
    static const uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
        {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22},
        {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41},
        {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A},
        {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41},
        {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E},
        {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E},
        {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F},
        {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };

    if (c == ' ') return space;
    if (c == '-') return minus;
    if (c == '.') return dot;
    if (c == ':') return colon;
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    return space;
}

static void draw_char(int x, int y, char c, int scale)
{
    const uint8_t *g = glyph_5x7(c);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (g[col] & BIT(row)) {
                for (int sx = 0; sx < scale; ++sx) {
                    for (int sy = 0; sy < scale; ++sy) {
                        set_pixel(x + col * scale + sx, y + row * scale + sy, true);
                    }
                }
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale)
{
    while (*text) {
        draw_char(x, y, *text++, scale);
        x += 6 * scale;
    }
}

static void draw_power_screen(int power_w, uint8_t oled_addr, uint8_t mcp_addr)
{
    memset(s_frame, 0, sizeof(s_frame));
    draw_rect(0, 0, SSD1306_WIDTH, SSD1306_HEIGHT, true);
    draw_text(6, 4, "RF POWER", 1);

    char info[24] = {};
    snprintf(info, sizeof(info), "OLED%02X MCP%02X", oled_addr, mcp_addr);
    draw_text(6, 54, info, 1);

    char value[8] = {};
    snprintf(value, sizeof(value), "%dW", power_w);
    const int scale = power_w >= 100 ? 4 : 5;
    const int width = (int)strlen(value) * 6 * scale - scale;
    draw_text((SSD1306_WIDTH - width) / 2, 20, value, scale);
}

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

static esp_err_t tca_attach(void)
{
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9548A_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_bus, TCA9548A_ADDR, 100), TAG, "TCA9548A not found at 0x70");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_tca), TAG, "add TCA9548A");
    ESP_RETURN_ON_ERROR(tca_select(TCA9548A_OLED_CHANNEL), TAG, "select OLED channel 3");
    ESP_LOGI(TAG, "TCA9548A detected at 0x%02X, OLED channel %d selected", TCA9548A_ADDR, TCA9548A_OLED_CHANNEL);
    return ESP_OK;
}

static esp_err_t oled_attach(uint8_t *addr_out)
{
    ESP_LOGI(TAG, "Scanning I2C%d on SDA GPIO%d, SCL GPIO%d at %d Hz", I2C_PORT, I2C_SDA_GPIO, I2C_SCL_GPIO,
             I2C_FREQ_HZ);
    uint8_t found_addr = 0;
    const uint8_t probe_addrs[] = {SSD1306_I2C_ADDR_PRIMARY, SSD1306_I2C_ADDR_SECONDARY};
    for (size_t i = 0; i < sizeof(probe_addrs); ++i) {
        const uint8_t addr = probe_addrs[i];
        if (i2c_master_probe(s_bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            if (addr == SSD1306_I2C_ADDR_PRIMARY || addr == SSD1306_I2C_ADDR_SECONDARY) {
                found_addr = addr;
            }
        }
    }
    ESP_RETURN_ON_FALSE(found_addr != 0, ESP_ERR_NOT_FOUND, TAG, "SSD1306 not found at 0x3C/0x3D");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = found_addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_oled), TAG, "add oled");
    *addr_out = found_addr;
    return ESP_OK;
}

static esp_err_t mcp_attach(uint8_t *addr_out)
{
    uint8_t found_addr = 0;
    for (uint8_t candidate = MCP23017_ADDR_MIN; candidate <= MCP23017_ADDR_MAX; ++candidate) {
        if (i2c_master_probe(s_bus, candidate, 100) == ESP_OK) {
            found_addr = candidate;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(found_addr != 0, ESP_ERR_NOT_FOUND, TAG, "MCP23017 not found at 0x20..0x27");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = found_addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_mcp), TAG, "add MCP23017");

    uint8_t iodira = 0;
    uint8_t iodirb = 0;
    uint8_t gppua = 0;
    uint8_t gppub = 0;
    if (mcp_read_reg(MCP23017_REG_IODIRA, &iodira) != ESP_OK) {
        iodira = 0xFF;
    }
    if (mcp_read_reg(MCP23017_REG_IODIRB, &iodirb) != ESP_OK) {
        iodirb = 0xFF;
    }
    if (mcp_read_reg(MCP23017_REG_GPPUA, &gppua) != ESP_OK) {
        gppua = 0x00;
    }
    if (mcp_read_reg(MCP23017_REG_GPPUB, &gppub) != ESP_OK) {
        gppub = 0x00;
    }

    ESP_RETURN_ON_ERROR(mcp_write_reg(MCP23017_REG_IODIRA, iodira | MCP23017_POWER_ENCODER_A_MASK), TAG,
                        "set IODIRA");
    ESP_RETURN_ON_ERROR(mcp_write_reg(MCP23017_REG_IODIRB, iodirb | MCP23017_POWER_ENCODER_B_MASK), TAG,
                        "set IODIRB");
    ESP_RETURN_ON_ERROR(mcp_write_reg(MCP23017_REG_GPPUA, gppua | MCP23017_POWER_ENCODER_A_MASK), TAG,
                        "set GPPUA");
    ESP_RETURN_ON_ERROR(mcp_write_reg(MCP23017_REG_GPPUB, gppub | MCP23017_POWER_ENCODER_B_MASK), TAG,
                        "set GPPUB");

    *addr_out = found_addr;
    ESP_LOGI(TAG, "MCP23017 detected at 0x%02X, power encoder A=PA6 B=PA7 S=PB0", found_addr);
    return ESP_OK;
}

static uint8_t power_encoder_state(void)
{
    uint8_t gpioa = 0xFF;
    if (mcp_read_reg(MCP23017_REG_GPIOA, &gpioa) != ESP_OK) {
        return 0xFF;
    }
    const bool a = (gpioa & BIT(6)) != 0;
    const bool b = (gpioa & BIT(7)) != 0;
    return (uint8_t)((a ? 2 : 0) | (b ? 1 : 0));
}

static int power_encoder_delta(uint8_t previous, uint8_t current)
{
    static const int8_t table[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0,
    };
    if (previous > 3 || current > 3) {
        return 0;
    }
    return table[(previous << 2) | current];
}

void app_main(void)
{
    uint8_t oled_addr = 0;
    uint8_t mcp_addr = 0;
    int power_w = POWER_INITIAL_W;

    ESP_ERROR_CHECK(i2c_bus_init());

    while (tca_attach() != ESP_OK) {
        ESP_LOGW(TAG, "TCA9548A not detected; retry in 1 s. Check mux VCC/GND/SDA/SCL and address jumpers.");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (oled_attach(&oled_addr) != ESP_OK) {
        ESP_LOGW(TAG, "SSD1306 not detected on TCA9548A channel %d; retry in 1 s. Check Qwiic port 3 wiring.",
                 TCA9548A_OLED_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (mcp_attach(&mcp_addr) != ESP_OK) {
        ESP_LOGW(TAG, "MCP23017 not detected on root I2C; retry in 1 s. Check encoder expander wiring.");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_ERROR_CHECK(oled_init());

    ESP_LOGI(TAG, "SSD1306 OLED initialized at 0x%02X through TCA9548A channel %d", oled_addr, TCA9548A_OLED_CHANNEL);
    draw_power_screen(power_w, oled_addr, mcp_addr);
    ESP_ERROR_CHECK(oled_flush());

    uint8_t last_state = power_encoder_state();
    int detent_accum = 0;
    ESP_LOGI(TAG, "Power knob display test started, initial power %dW", power_w);

    while (true) {
        const uint8_t state = power_encoder_state();
        const int delta = power_encoder_delta(last_state, state);
        last_state = state;
        if (delta != 0) {
            detent_accum += delta;
            if (detent_accum >= 4 || detent_accum <= -4) {
                const int step = detent_accum / 4;
                detent_accum = 0;
                const int next_power = power_w + step;
                if (next_power >= POWER_MIN_W && next_power <= POWER_MAX_W && next_power != power_w) {
                    power_w = next_power;
                    draw_power_screen(power_w, oled_addr, mcp_addr);
                    ESP_ERROR_CHECK(oled_flush());
                    ESP_LOGI(TAG, "Power display changed to %dW", power_w);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
