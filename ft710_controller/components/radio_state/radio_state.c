#include "radio_state.h"

#include <string.h>

void radio_state_init(radio_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->active_vfo = 'A';
    state->mode_a = FT710_MODE_UNKNOWN;
    state->mode_b = FT710_MODE_UNKNOWN;
    state->power_w = 5;
    state->dnr_level = 0;
    state->width_index = 0;
    state->noise_blanker_on = false;
    state->noise_blanker_level = 0;
    state->notch_on = false;
    state->notch_value = 0;
    state->mic_gain = 0;
}

const char *radio_state_mode_name(ft710_mode_t mode)
{
    switch (mode) {
    case FT710_MODE_LSB:
        return "LSB";
    case FT710_MODE_USB:
        return "USB";
    case FT710_MODE_AM:
        return "AM";
    case FT710_MODE_FM:
        return "FM";
    case FT710_MODE_DATA_U:
        return "DATA-U";
    default:
        return "--";
    }
}

uint8_t radio_state_mode_code(ft710_mode_t mode)
{
    switch (mode) {
    case FT710_MODE_LSB:
        return 1;
    case FT710_MODE_USB:
        return 2;
    case FT710_MODE_AM:
        return 5;
    case FT710_MODE_FM:
        return 4;
    case FT710_MODE_DATA_U:
        return 12;
    default:
        return 0;
    }
}

ft710_mode_t radio_state_mode_from_code(uint8_t code)
{
    switch (code) {
    case 1:
        return FT710_MODE_LSB;
    case 2:
        return FT710_MODE_USB;
    case 4:
        return FT710_MODE_FM;
    case 5:
        return FT710_MODE_AM;
    case 12:
        return FT710_MODE_DATA_U;
    default:
        return FT710_MODE_UNKNOWN;
    }
}

uint32_t radio_state_band_default_hz(const char *band_label)
{
    if (!band_label) {
        return 0;
    }
    if (strcmp(band_label, "3.5") == 0) {
        return 3500000;
    }
    if (strcmp(band_label, "7") == 0) {
        return 7074000;
    }
    if (strcmp(band_label, "14") == 0) {
        return 14270000;
    }
    if (strcmp(band_label, "21") == 0) {
        return 21400000;
    }
    if (strcmp(band_label, "28") == 0) {
        return 28400000;
    }
    if (strcmp(band_label, "50") == 0) {
        return 50125000;
    }
    return 0;
}
