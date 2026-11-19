#include "oled_ssd1306.h"

#include <string.h>
#include "esp_log.h"
#include "board_bsp.h"

static const char *TAG = "oled_ssd1306";

#define SSD1306_CTRL_CMD        0x00U
#define SSD1306_CTRL_DATA       0x40U

static uint8_t s_framebuffer[OLED_FB_SIZE];
static bool    s_device_online = false;

typedef struct {
    char    ch;
    uint8_t col[5];
} glyph_5x7_t;

/* 精簡 5x7 縱向欄位點陣字型表 (LSB = 頂端像素 y=0) */
static const glyph_5x7_t s_glyph_table[] = {
    { ' ', { 0b0000000, 0b0000000, 0b0000000, 0b0000000, 0b0000000 } },
    { '0', { 0b0111110, 0b1000001, 0b1001001, 0b1000001, 0b0111110 } },
    { '1', { 0b0000000, 0b1000010, 0b1111111, 0b1000000, 0b0000000 } },
    { '2', { 0b1110010, 0b1001001, 0b1001001, 0b1001001, 0b1000110 } },
    { '3', { 0b0100010, 0b1000001, 0b1001001, 0b1001001, 0b0110110 } },
    { '4', { 0b0011000, 0b0010100, 0b0010010, 0b1111111, 0b0010000 } },
    { '5', { 0b0101111, 0b1001001, 0b1001001, 0b1001001, 0b0110001 } },
    { '6', { 0b0111110, 0b1001001, 0b1001001, 0b1001001, 0b0110010 } },
    { '7', { 0b0000001, 0b1110001, 0b0001001, 0b0000101, 0b0000011 } },
    { '8', { 0b0110110, 0b1001001, 0b1001001, 0b1001001, 0b0110110 } },
    { '9', { 0b0100110, 0b1001001, 0b1001001, 0b1001001, 0b0111110 } },
    { 'A', { 0b1111100, 0b0010010, 0b0010001, 0b0010010, 0b1111100 } },
    { 'B', { 0b1111111, 0b1001001, 0b1001001, 0b1001001, 0b0110110 } },
    { 'C', { 0b0111110, 0b1000001, 0b1000001, 0b1000001, 0b0100010 } },
    { 'D', { 0b1111111, 0b1000001, 0b1000001, 0b0100010, 0b0011100 } },
    { 'E', { 0b1111111, 0b1001001, 0b1001001, 0b1001001, 0b1000001 } },
    { 'F', { 0b1111111, 0b0001001, 0b0001001, 0b0001001, 0b0000001 } },
    { 'G', { 0b0111110, 0b1000001, 0b1001001, 0b1001001, 0b1111010 } },
    { 'H', { 0b1111111, 0b0001000, 0b0001000, 0b0001000, 0b1111111 } },
    { 'I', { 0b0000000, 0b1000001, 0b1111111, 0b1000001, 0b0000000 } },
    { 'J', { 0b0100000, 0b1000000, 0b1000001, 0b0111111, 0b0000001 } },
    { 'K', { 0b1111111, 0b0001000, 0b0010100, 0b0100010, 0b1000001 } },
    { 'L', { 0b1111111, 0b1000000, 0b1000000, 0b1000000, 0b1000000 } },
    { 'M', { 0b1111111, 0b0000010, 0b0001100, 0b0000010, 0b1111111 } },
    { 'N', { 0b1111111, 0b0000110, 0b0001000, 0b0110000, 0b1111111 } },
    { 'O', { 0b0111110, 0b1000001, 0b1000001, 0b1000001, 0b0111110 } },
    { 'P', { 0b1111111, 0b0001001, 0b0001001, 0b0001001, 0b0000110 } },
    { 'Q', { 0b0111110, 0b1000001, 0b1010001, 0b0100001, 0b1011110 } },
    { 'R', { 0b1111111, 0b0001001, 0b0011001, 0b0101001, 0b1000110 } },
    { 'S', { 0b0100110, 0b1001001, 0b1001001, 0b1001001, 0b0110010 } },
    { 'T', { 0b0000001, 0b0000001, 0b1111111, 0b0000001, 0b0000001 } },
    { 'U', { 0b0111111, 0b1000000, 0b1000000, 0b1000000, 0b0111111 } },
    { 'V', { 0b0011111, 0b0100000, 0b1000000, 0b0100000, 0b0011111 } },
    { 'W', { 0b0111111, 0b1000000, 0b0111000, 0b1000000, 0b0111111 } },
    { 'X', { 0b1100011, 0b0010100, 0b0001000, 0b0010100, 0b1100011 } },
    { 'Y', { 0b0000011, 0b0000100, 0b1111000, 0b0000100, 0b0000011 } },
    { 'Z', { 0b1100001, 0b1010001, 0b1001001, 0b1000101, 0b1000011 } },
    { '.', { 0b0000000, 0b1100000, 0b1100000, 0b0000000, 0b0000000 } },
    { ',', { 0b0000000, 0b1010000, 0b0110000, 0b0000000, 0b0000000 } },
    { ':', { 0b0000000, 0b0110110, 0b0110110, 0b0000000, 0b0000000 } },
    { '-', { 0b0001000, 0b0001000, 0b0001000, 0b0001000, 0b0001000 } },
    { '+', { 0b0001000, 0b0001000, 0b0111110, 0b0001000, 0b0001000 } },
    { '=', { 0b0010100, 0b0010100, 0b0010100, 0b0010100, 0b0010100 } },
    { '/', { 0b0100000, 0b0010000, 0b0001000, 0b0000100, 0b0000010 } },
    { '%', { 0b1100011, 0b0010011, 0b0001000, 0b1100100, 0b1100011 } },
    { '!', { 0b0000000, 0b0000000, 0b1011111, 0b0000000, 0b0000000 } },
    { '[', { 0b0000000, 0b1111111, 0b1000001, 0b1000001, 0b0000000 } },
    { ']', { 0b0000000, 0b1000001, 0b1000001, 0b1111111, 0b0000000 } },
    { '(', { 0b0000000, 0b0011100, 0b0100010, 0b1000001, 0b0000000 } },
    { ')', { 0b0000000, 0b1000001, 0b0100010, 0b0011100, 0b0000000 } },
    { '<', { 0b0001000, 0b0010100, 0b0100010, 0b1000001, 0b0000000 } },
    { '>', { 0b0000000, 0b1000001, 0b0100010, 0b0010100, 0b0001000 } },
    { '^', { 0b0000100, 0b0000010, 0b0000001, 0b0000010, 0b0000100 } },
    { '_', { 0b1000000, 0b1000000, 0b1000000, 0b1000000, 0b1000000 } },
};

static const uint8_t s_fallback_glyph[5] = {
    0b0000010, 0b0000001, 0b1011001, 0b0000101, 0b0000010
};

static esp_err_t ssd1306_write_cmd(uint8_t cmd)
{
    uint8_t pkt[2] = { SSD1306_CTRL_CMD, cmd };
    return board_i2c_write(OLED_SSD1306_I2C_ADDR, pkt, sizeof(pkt));
}

static const uint8_t *lookup_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - ('a' - 'A'));
    }

    const size_t count = sizeof(s_glyph_table) / sizeof(s_glyph_table[0]);
    for (size_t i = 0; i < count; i++) {
        if (s_glyph_table[i].ch == c) {
            return s_glyph_table[i].col;
        }
    }
    return s_fallback_glyph;
}

static inline void fb_set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }

    size_t idx = ((size_t)(y >> 3) * OLED_WIDTH) + (size_t)x;
    uint8_t mask = (uint8_t)(1U << (y & 0x07U));

    if (on) {
        s_framebuffer[idx] |= mask;
    } else {
        s_framebuffer[idx] &= (uint8_t)(~mask);
    }
}

esp_err_t oled_ssd1306_init(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    if (!board_i2c_is_device_online(OLED_SSD1306_I2C_ADDR)) {
        s_device_online = false;
        ESP_LOGW(TAG, "SSD1306 (0x%02X) offline; running headless", OLED_SSD1306_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }

    static const uint8_t init_cmds[] = {
        0xAE,       /* Display OFF */
        0xD5, 0x80, /* Clock divide ratio / oscillator freq */
        0xA8, 0x3F, /* Multiplex ratio 1/64 */
        0xD3, 0x00, /* Display offset = 0 */
        0x40,       /* Start line = 0 */
        0x8D, 0x14, /* Enable internal charge pump */
        0x20, 0x02, /* Page addressing mode */
        0xA1,       /* Segment remap (col 127 -> SEG0) */
        0xC8,       /* COM scan direction remapped (COM63 -> COM0) */
        0xDA, 0x12, /* COM pins hardware config (128x64) */
        0x81, 0x8F, /* Default contrast */
        0xD9, 0xF1, /* Pre-charge period */
        0xDB, 0x40, /* VCOMH deselect level */
        0xA4,       /* Resume to RAM content display */
        0xA6,       /* Normal display (non-inverted) */
        0x2E,       /* Deactivate scroll */
        0xAF        /* Display ON */
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t ret = ssd1306_write_cmd(init_cmds[i]);
        if (ret != ESP_OK) {
            s_device_online = false;
            ESP_LOGE(TAG, "SSD1306 init cmd[0x%02X] failed: %s", init_cmds[i], esp_err_to_name(ret));
            return ret;
        }
    }

    s_device_online = true;
    ESP_LOGI(TAG, "SSD1306 128x64 OLED initialized (0x%02X)", OLED_SSD1306_I2C_ADDR);
    return oled_ssd1306_flush();
}

bool oled_ssd1306_is_online(void)
{
    return s_device_online;
}

esp_err_t oled_ssd1306_set_contrast(uint8_t contrast)
{
    if (!s_device_online) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ssd1306_write_cmd(0x81);
    if (ret != ESP_OK) {
        return ret;
    }
    return ssd1306_write_cmd(contrast);
}

esp_err_t oled_ssd1306_flush(void)
{
    if (!s_device_online) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t page_pkt[1 + OLED_WIDTH];
    page_pkt[0] = SSD1306_CTRL_DATA;

    for (uint8_t page = 0; page < OLED_PAGE_COUNT; page++) {
        esp_err_t ret = ssd1306_write_cmd((uint8_t)(0xB0U | page));
        if (ret != ESP_OK) {
            return ret;
        }
        ret = ssd1306_write_cmd(0x00U);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = ssd1306_write_cmd(0x10U);
        if (ret != ESP_OK) {
            return ret;
        }

        memcpy(&page_pkt[1], &s_framebuffer[(size_t)page * OLED_WIDTH], OLED_WIDTH);
        ret = board_i2c_write(OLED_SSD1306_I2C_ADDR, page_pkt, sizeof(page_pkt));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

const uint8_t *oled_ssd1306_get_framebuffer(void)
{
    return s_framebuffer;
}

void oled_fb_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void oled_fb_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT || w == 0 || h == 0) {
        return;
    }

    uint16_t x_end = (uint16_t)x + (uint16_t)w;
    uint16_t y_end = (uint16_t)y + (uint16_t)h;
    if (x_end > OLED_WIDTH) {
        x_end = OLED_WIDTH;
    }
    if (y_end > OLED_HEIGHT) {
        y_end = OLED_HEIGHT;
    }

    for (uint16_t py = y; py < y_end; py++) {
        for (uint16_t px = x; px < x_end; px++) {
            fb_set_pixel((uint8_t)px, (uint8_t)py, on);
        }
    }
}

void oled_fb_draw_hline(uint8_t x, uint8_t y, uint8_t w, bool on)
{
    oled_fb_fill_rect(x, y, w, 1, on);
}

void oled_fb_draw_char(uint8_t x, uint8_t y, char c, bool invert)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) {
        return;
    }

    const uint8_t *cols = lookup_glyph(c);

    for (uint8_t col_idx = 0; col_idx < 5; col_idx++) {
        uint8_t bits = cols[col_idx];
        for (uint8_t row = 0; row < 7; row++) {
            bool pixel_on = ((bits >> row) & 0x01U) != 0;
            if (invert) {
                pixel_on = !pixel_on;
            }
            fb_set_pixel((uint8_t)(x + col_idx), (uint8_t)(y + row), pixel_on);
        }
    }

    for (uint8_t row = 0; row < 7; row++) {
        fb_set_pixel((uint8_t)(x + 5), (uint8_t)(y + row), invert);
    }
}

void oled_fb_draw_str(uint8_t x, uint8_t y, const char *str, bool invert)
{
    if (str == NULL) {
        return;
    }

    uint8_t cur_x = x;
    while (*str != '\0' && (cur_x + 5U) <= OLED_WIDTH) {
        oled_fb_draw_char(cur_x, y, *str, invert);
        cur_x = (uint8_t)(cur_x + 6U);
        str++;
    }
}