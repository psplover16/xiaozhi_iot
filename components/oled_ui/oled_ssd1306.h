#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_SSD1306_I2C_ADDR   0x3C
#define OLED_WIDTH              128U
#define OLED_HEIGHT             64U
#define OLED_PAGE_COUNT         (OLED_HEIGHT / 8U)
#define OLED_FB_SIZE            (OLED_WIDTH * OLED_PAGE_COUNT)

esp_err_t      oled_ssd1306_init(void);
bool           oled_ssd1306_is_online(void);
esp_err_t      oled_ssd1306_set_contrast(uint8_t contrast);
esp_err_t      oled_ssd1306_flush(void);
const uint8_t *oled_ssd1306_get_framebuffer(void);

void           oled_fb_clear(void);
void           oled_fb_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);
void           oled_fb_draw_hline(uint8_t x, uint8_t y, uint8_t w, bool on);
void           oled_fb_draw_char(uint8_t x, uint8_t y, char c, bool invert);
void           oled_fb_draw_str(uint8_t x, uint8_t y, const char *str, bool invert);

#ifdef __cplusplus
}
#endif