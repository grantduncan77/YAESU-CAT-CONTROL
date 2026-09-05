#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "user_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "src/axs15231b/esp_lcd_axs15231b.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "i2c_bsp.h"

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;

static uint16_t *trans_buf_1 = NULL; 
uint8_t *lvgl_dest = NULL;                //旋转buffer
static SemaphoreHandle_t flush_done_semaphore;
static esp_io_expander_handle_t io_expander = NULL;
static uint32_t touch_last_pressed_ms = 0;
static lv_point_t touch_last_point = {0, 0};
static lvgl_port_ui_create_cb_t ui_create_cb = NULL;
static volatile uint16_t touch_debug_raw_x = 0;
static volatile uint16_t touch_debug_raw_y = 0;
static volatile int16_t touch_debug_x = 0;
static volatile int16_t touch_debug_y = 0;
static volatile bool touch_debug_pressed = false;
static volatile uint32_t touch_debug_seq = 0;

#define LCD_BIT_PER_PIXEL 16
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * BYTES_PER_PIXEL)
#define TOUCH_RELEASE_HOLD_MS 180


static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 100},
  {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t TaskWoken;
  xSemaphoreGiveFromISR(flush_done_semaphore,&TaskWoken);
  return false;
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void example_lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
  lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));
#if (Rotated == USER_DISP_ROT_90)
  lv_display_rotation_t rotation = lv_display_get_rotation(disp);
  lv_area_t rotated_area;
  if(rotation != LV_DISPLAY_ROTATION_0)
  {
    lv_color_format_t cf = lv_display_get_color_format(disp);
    /*Calculate the position of the rotated area*/
    rotated_area = *area;
    lv_display_rotate_area(disp, &rotated_area);
    /*Calculate the source stride (bytes in a line) from the width of the area*/
    uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
    /*Calculate the stride of the destination (rotated) area too*/
    uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
    /*Have a buffer to store the rotated area and perform the rotation*/
    
    int32_t src_w = lv_area_get_width(area);
    int32_t src_h = lv_area_get_height(area);
    lv_draw_sw_rotate(color_p, lvgl_dest, src_w, src_h, src_stride, dest_stride, rotation, cf);
    /*Use the rotated area and rotated buffer from now on*/
    area = &rotated_area;
  }

  const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
  const int offgap = (LCD_NOROT_VRES / flush_coun);
  const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
  int offsetx1 = 0;
  int offsety1 = 0;
  int offsetx2 = LCD_NOROT_HRES;
  int offsety2 = offgap;

  uint16_t *map = (uint16_t *)lvgl_dest;
  xSemaphoreGive(flush_done_semaphore);
  for(int i = 0; i<flush_coun; i++)
  {
    xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
    memcpy(trans_buf_1,map,LVGL_DMA_BUFF_LEN);
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
    offsety1 += offgap;
    offsety2 += offgap;
    map += dmalen;
  }
  xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
  lv_disp_flush_ready(disp);
#else
  const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
  const int offgap = (EXAMPLE_LCD_V_RES / flush_coun);
  const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
  int offsetx1 = 0;
  int offsety1 = 0;
  int offsetx2 = EXAMPLE_LCD_H_RES;
  int offsety2 = offgap;

  uint16_t *map = (uint16_t *)color_p;
  xSemaphoreGive(flush_done_semaphore);
  for(int i = 0; i<flush_coun; i++)
  {
    xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
    memcpy(trans_buf_1,map,LVGL_DMA_BUFF_LEN);
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
    offsety1 += offgap;
    offsety2 += offgap;
    map += dmalen;
  }
  xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
  lv_disp_flush_ready(disp);
#endif
}
static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData)
{
  static uint8_t last_count = 0;
  uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
  uint8_t buff[8] = {0};
  if (i2c_master_touch_write(disp_touch_dev_handle, read_touchpad_cmd, sizeof(read_touchpad_cmd)) != ESP_OK) {
    indevData->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (i2c_master_touch_read(disp_touch_dev_handle, buff, sizeof(buff)) != ESP_OK) {
    indevData->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  uint8_t gesture = buff[0];
  uint8_t count = buff[1];
  uint16_t pointX;
  uint16_t pointY;
  pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
  touch_debug_raw_x = pointX;
  touch_debug_raw_y = pointY;
  if (count != last_count) {
    ESP_LOGI("Touch","gesture=%u count=%u raw=%u,%u", gesture, count, pointX, pointY);
    last_count = count;
  }
  if (gesture == 0 && count > 0 && count < 5)
  {
    indevData->state = LV_INDEV_STATE_PRESSED;
#if (Rotated == USER_DISP_ROT_90)
    if(pointX > EXAMPLE_LCD_H_RES) pointX = EXAMPLE_LCD_H_RES;
    if(pointY > EXAMPLE_LCD_V_RES) pointY = EXAMPLE_LCD_V_RES;
    indevData->point.x = (EXAMPLE_LCD_H_RES - pointX);
    indevData->point.y = (EXAMPLE_LCD_V_RES - pointY);
#else
    if(pointX > EXAMPLE_LCD_V_RES) pointX = EXAMPLE_LCD_V_RES;
    if(pointY > EXAMPLE_LCD_H_RES) pointY = EXAMPLE_LCD_H_RES;
    indevData->point.x = pointY;
    indevData->point.y = (EXAMPLE_LCD_V_RES-pointX);
#endif
    touch_last_point = indevData->point;
    touch_last_pressed_ms = now_ms;
    touch_debug_x = indevData->point.x;
    touch_debug_y = indevData->point.y;
    touch_debug_pressed = true;
    touch_debug_seq++;
  }
  else 
  {
    if (now_ms - touch_last_pressed_ms < TOUCH_RELEASE_HOLD_MS) {
      indevData->state = LV_INDEV_STATE_PRESSED;
      indevData->point = touch_last_point;
      touch_debug_x = indevData->point.x;
      touch_debug_y = indevData->point.y;
      touch_debug_pressed = true;
      touch_debug_seq++;
    } else {
      indevData->state = LV_INDEV_STATE_RELEASED;
      touch_debug_pressed = false;
      touch_debug_seq++;
    }
  }
}

static bool example_lvgl_lock(int timeout_ms)
{
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGiveRecursive(lvgl_mux);
}

bool lvgl_port_lock(int timeout_ms)
{
  return example_lvgl_lock(timeout_ms);
}

void lvgl_port_unlock(void)
{
  example_lvgl_unlock();
}

void lvgl_port_set_ui_create_cb(lvgl_port_ui_create_cb_t cb)
{
  ui_create_cb = cb;
}

bool lvgl_port_get_touch_debug(uint16_t *raw_x, uint16_t *raw_y, int16_t *x, int16_t *y,
                               bool *pressed, uint32_t *seq)
{
  if (raw_x) *raw_x = touch_debug_raw_x;
  if (raw_y) *raw_y = touch_debug_raw_y;
  if (x) *x = touch_debug_x;
  if (y) *y = touch_debug_y;
  if (pressed) *pressed = touch_debug_pressed;
  if (seq) *seq = touch_debug_seq;
  return touch_debug_seq != 0;
}

void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (example_lvgl_lock(-1)) 
    {
      task_delay_ms = lv_timer_handler();
      //Release the mutex
      example_lvgl_unlock();
    }
    if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

static void example_lcd_pwm_off_early(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_BK_LIGHT);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, 0));
}

static void example_lcd_exio_init(void)
{
  if (io_expander == NULL) {
    i2c_master_bus_handle_t tca9554_i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  }

  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST |
                                                       EXAMPLE_EXIO_PIN_SYS_EN | EXAMPLE_EXIO_PIN_NS_MODE,
                                          IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN | EXAMPLE_EXIO_PIN_NS_MODE, 1));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
}

static void example_lcd_reset(void)
{
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 0));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
}

static void example_lcd_backlight_set(bool enable)
{
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, enable ? 1 : 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, enable ? 1 : 0));
}

static uint16_t swap_rgb565(uint16_t color)
{
  return (color >> 8) | (color << 8);
}

static void example_panel_native_test(esp_lcd_panel_handle_t panel)
{
  const int chunk_rows = 64;
  const int pixel_count = EXAMPLE_LCD_H_RES * chunk_rows;
  uint16_t *buf = (uint16_t *)heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
  assert(buf);

  const uint16_t colors[] = {
    swap_rgb565(0xF800),
    swap_rgb565(0x07E0),
    swap_rgb565(0x001F),
    swap_rgb565(0xFFE0),
    swap_rgb565(0xFFFF),
  };

  int y = 0;
  while (y < EXAMPLE_LCD_V_RES) {
    int rows = EXAMPLE_LCD_V_RES - y;
    if (rows > chunk_rows) {
      rows = chunk_rows;
    }

    uint16_t color = colors[(y / chunk_rows) % (sizeof(colors) / sizeof(colors[0]))];
    for (int i = 0; i < EXAMPLE_LCD_H_RES * rows; ++i) {
      buf[i] = color;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, EXAMPLE_LCD_H_RES, y + rows, buf));
    xSemaphoreTake(flush_done_semaphore, portMAX_DELAY);
    y += rows;
  }

  heap_caps_free(buf);
  vTaskDelay(pdMS_TO_TICKS(1200));
}

static void example_create_minimal_lvgl_scene(void)
{
  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "LVGL 9.3.0 OK");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t *note = lv_label_create(screen);
  lv_label_set_text(note, "ESP32-S3 3.49 V2");
  lv_obj_set_style_text_color(note, lv_color_hex(0xFF6A21), LV_PART_MAIN);
  lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 76);

  lv_obj_t *red = lv_obj_create(screen);
  lv_obj_set_size(red, 116, 96);
  lv_obj_set_style_bg_color(red, lv_color_hex(0xE53935), LV_PART_MAIN);
  lv_obj_set_style_border_width(red, 0, LV_PART_MAIN);
  lv_obj_align(red, LV_ALIGN_CENTER, 0, -82);

  lv_obj_t *green = lv_obj_create(screen);
  lv_obj_set_size(green, 116, 96);
  lv_obj_set_style_bg_color(green, lv_color_hex(0x43A047), LV_PART_MAIN);
  lv_obj_set_style_border_width(green, 0, LV_PART_MAIN);
  lv_obj_align(green, LV_ALIGN_CENTER, 0, 28);

  lv_obj_t *blue = lv_obj_create(screen);
  lv_obj_set_size(blue, 116, 96);
  lv_obj_set_style_bg_color(blue, lv_color_hex(0x1E88E5), LV_PART_MAIN);
  lv_obj_set_style_border_width(blue, 0, LV_PART_MAIN);
  lv_obj_align(blue, LV_ALIGN_CENTER, 0, 138);
}

void lvgl_port_init(void)
{
  flush_done_semaphore = xSemaphoreCreateBinary();
  assert(flush_done_semaphore);
  ESP_LOGI(TAG, "Initialize LCD reset and backlight");
  example_lcd_pwm_off_early();
  example_lcd_exio_init();

  ESP_LOGI(TAG, "Initialize QSPI bus");
  spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
	  esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    
  esp_lcd_panel_io_spi_config_t io_config = {};
	io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
    io_config.dc_gpio_num = -1;          
    io_config.spi_mode = 3;              
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;    
    io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
    //io_config.user_ctx = &disp_drv,         
    io_config.lcd_cmd_bits = 32;         
    io_config.lcd_param_bits = 8;        
    io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
  axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
  esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

  ESP_LOGI(TAG, "Install panel driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

  example_lcd_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  example_lcd_backlight_set(true);

  lv_init();

  lv_display_t * disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);  /* 以水平和垂直分辨率（像素）进行基本初始化 */
  lv_display_set_flush_cb(disp, example_lvgl_flush_cb);                           /* 设置刷新回调函数以绘制到显示屏 */
  
  uint8_t *buffer_1 = NULL;
  uint8_t *buffer_2 = NULL;
  buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
  buffer_2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
  assert(buffer_1);
  assert(buffer_2);
	trans_buf_1 = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
	assert(trans_buf_1);
  lv_display_set_buffers(disp, buffer_1, buffer_2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_user_data(disp, panel);
#if (Rotated == USER_DISP_ROT_90)
    lvgl_dest = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM); //旋转buf
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
#endif
  /*port indev*/
  lv_indev_t *touch_indev = NULL;
  touch_indev = lv_indev_create();
  lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

  lvgl_mux = xSemaphoreCreateRecursiveMutex();
  assert(lvgl_mux);
  xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL,0);
  if (ui_create_cb != NULL && example_lvgl_lock(-1))
  {
    ui_create_cb();
    lv_refr_now(NULL);
    example_lvgl_unlock();
  }
}
