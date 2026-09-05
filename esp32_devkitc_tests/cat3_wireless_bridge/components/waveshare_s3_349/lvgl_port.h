#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdbool.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

typedef void (*lvgl_port_ui_create_cb_t)(void);
void lvgl_port_set_ui_create_cb(lvgl_port_ui_create_cb_t cb);
bool lvgl_port_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y, int16_t *x, int16_t *y,
                               bool *pressed, uint32_t *seq);


#ifdef __cplusplus
}
#endif



#endif






