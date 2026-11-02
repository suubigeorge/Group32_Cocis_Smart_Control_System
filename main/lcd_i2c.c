/*
 * lcd_i2c.c - HD44780 16x2 LCD driver via PCF8574 (address 0x27).
 * Owner: Interfaces.
 *
 * Several tasks write to the LCD (entrance messages, clock/idle screen,
 * alerts). Writing a line takes ~40 I2C transactions, so a whole update is
 * wrapped in s_lcd_mtx: two tasks can never mix characters on the screen.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config.h"
#include "hw.h"
#include "lcd_i2c.h"

static const char *TAG = "lcd";
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lcd_mtx;
static int64_t s_hold_until_us;
static bool s_ok;
static char s_shown[2][17];      /* cache: skip rewriting identical text */

#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08

static void pcf_write(uint8_t v)
{
    if (!s_ok) return;                      /* display missing: stop retrying */
    if (i2c_master_transmit(s_dev, &v, 1, 50) != ESP_OK) s_ok = false;
}

static void lcd_pulse(uint8_t v)
{
    pcf_write(v | LCD_EN);
    esp_rom_delay_us(1);
    pcf_write(v & ~LCD_EN);
    esp_rom_delay_us(50);
}

static void lcd_write4(uint8_t nibble, uint8_t mode)
{
    uint8_t v = (nibble & 0xF0) | mode | LCD_BL;
    pcf_write(v);
    lcd_pulse(v);
}

static void lcd_send(uint8_t value, uint8_t mode)
{
    lcd_write4(value & 0xF0, mode);
    lcd_write4((value << 4) & 0xF0, mode);
}

static void lcd_cmd(uint8_t c) { lcd_send(c, 0); if (c <= 0x02) vTaskDelay(pdMS_TO_TICKS(2)); }

static void put_line_locked(int row, const char *text)
{
    char buf[17];
    memset(buf, ' ', 16);
    buf[16] = 0;
    size_t n = strnlen(text ? text : "", 16);
    memcpy(buf, text, n);
    if (strcmp(buf, s_shown[row]) == 0) return;
    lcd_cmd(0x80 | (row ? 0x40 : 0x00));
    for (int i = 0; i < 16; i++) lcd_send((uint8_t)buf[i], LCD_RS);
    memcpy(s_shown[row], buf, 17);
}

void lcd_init(void)
{
    s_lcd_mtx = xSemaphoreCreateMutex();
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(hw_i2c_bus(), &dc, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "LCD not added");
        return;
    }
    s_ok = true;
    vTaskDelay(pdMS_TO_TICKS(50));
    /* HD44780 4-bit init sequence */
    lcd_write4(0x30, 0); vTaskDelay(pdMS_TO_TICKS(5));
    lcd_write4(0x30, 0); esp_rom_delay_us(150);
    lcd_write4(0x30, 0);
    lcd_write4(0x20, 0);
    lcd_cmd(0x28);   /* 4-bit, 2 lines, 5x8 */
    lcd_cmd(0x0C);   /* display on, cursor off */
    lcd_cmd(0x06);   /* entry mode */
    lcd_cmd(0x01);   /* clear */
    memset(s_shown, 0, sizeof(s_shown));
    if (!s_ok) ESP_LOGE(TAG, "LCD not responding at 0x%02X", LCD_I2C_ADDR);
}

bool lcd_ok(void) { return s_ok; }

void lcd_message(const char *line1, const char *line2, int hold_ms)
{
    if (!s_dev || xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    put_line_locked(0, line1);
    put_line_locked(1, line2);
    s_hold_until_us = esp_timer_get_time() + (int64_t)hold_ms * 1000;
    xSemaphoreGive(s_lcd_mtx);
}

void lcd_idle(const char *line1, const char *line2)
{
    if (!s_dev || xSemaphoreTake(s_lcd_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (esp_timer_get_time() >= s_hold_until_us) {
        put_line_locked(0, line1);
        put_line_locked(1, line2);
    }
    xSemaphoreGive(s_lcd_mtx);
}
