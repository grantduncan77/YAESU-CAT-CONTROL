#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FT710_MODE_UNKNOWN = 0,
    FT710_MODE_LSB,
    FT710_MODE_USB,
    FT710_MODE_AM,
    FT710_MODE_FM,
    FT710_MODE_DATA_U,
} ft710_mode_t;

typedef struct {
    bool online;
    char active_vfo;
    uint32_t vfo_a_hz;
    uint32_t vfo_b_hz;
    ft710_mode_t mode_a;
    ft710_mode_t mode_b;
    uint8_t power_w;
    bool dnr_on;
    uint8_t dnr_level;
    uint8_t width_index;
    uint32_t ok_count;
    uint32_t fail_count;
    uint32_t last_cat_ms;
} radio_state_t;

void radio_state_init(radio_state_t *state);
const char *radio_state_mode_name(ft710_mode_t mode);
uint8_t radio_state_mode_code(ft710_mode_t mode);
ft710_mode_t radio_state_mode_from_code(uint8_t code);
uint32_t radio_state_band_default_hz(const char *band_label);

#ifdef __cplusplus
}
#endif
