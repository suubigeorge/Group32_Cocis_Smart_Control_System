/*
 * hw.c - Low-level hardware helpers.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "config.h"
#include "hw.h"

static const char *TAG = "hw";
static i2c_master_bus_handle_t s_bus;
static adc_oneshot_unit_handle_t s_adc;
static SemaphoreHandle_t s_adc_mtx;   /* both room tasks share ADC1 */

#define SERVO_TIMER   LEDC_TIMER_0
#define BUZZER_TIMER  LEDC_TIMER_1
#define CH_ENTRY      LEDC_CHANNEL_0
#define CH_EXIT       LEDC_CHANNEL_1
#define CH_BUZZER     LEDC_CHANNEL_2

static const int PIR_PINS[ROOM_COUNT]   = {PIN_PIR_ROOM1, PIN_PIR_ROOM2};
static const int LIGHT_PINS[ROOM_COUNT] = {PIN_LIGHT_ROOM1, PIN_LIGHT_ROOM2};
static const adc_channel_t LDR_CH[ROOM_COUNT] = {ADC_CHANNEL_6, ADC_CHANNEL_7}; /* GPIO34, GPIO35 */

static uint32_t servo_duty(int deg)
{
    /* 50 Hz, 14-bit: 20 ms period = 16384 counts; 0 deg = 0.5 ms, 180 deg = 2.4 ms */
    uint32_t us = 500 + (uint32_t)deg * 1900 / 180;
    return us * 16384 / 20000;
}

void hw_init(void)
{
    /* I2C master bus (thread-safe in the new ESP-IDF driver) */
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));

    /* Servos */
    ledc_timer_config_t st = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = SERVO_TIMER, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&st));
    ledc_channel_config_t ce = {
        .gpio_num = PIN_SERVO_ENTRY, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = CH_ENTRY,
        .timer_sel = SERVO_TIMER, .duty = servo_duty(SERVO_LOCKED_DEG), .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ce));
    ce.gpio_num = PIN_SERVO_EXIT; ce.channel = CH_EXIT;
    ESP_ERROR_CHECK(ledc_channel_config(&ce));

    /* Buzzer (2 kHz tone) */
    ledc_timer_config_t bt = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER, .freq_hz = 2000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bt));
    ledc_channel_config_t cb = {
        .gpio_num = PIN_BUZZER, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = CH_BUZZER,
        .timer_sel = BUZZER_TIMER, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&cb));

    /* Emergency button: active low with internal pull-up */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_BTN_EMERGENCY),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    /* RFID readers: keep chip-selects HIGH (not selected) and reset LOW
     * (reader off) until the rfid task sets them up. */
    gpio_config_t rfid = {
        .pin_bit_mask = (1ULL << PIN_RFID_ENTRY_CS) | (1ULL << PIN_RFID_EXIT_CS) | (1ULL << PIN_RFID_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rfid));
    gpio_set_level(PIN_RFID_ENTRY_CS, 1);
    gpio_set_level(PIN_RFID_EXIT_CS, 1);
    gpio_set_level(PIN_RFID_RST, 0);

    /* PIR sensors */
    gpio_config_t pir = {
        .pin_bit_mask = (1ULL << PIN_PIR_ROOM1) | (1ULL << PIN_PIR_ROOM2),
        .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir));

    /* Room lights */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_LIGHT_ROOM1) | (1ULL << PIN_LIGHT_ROOM2),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    hw_light(0, false);
    hw_light(1, false);

    /* LDRs on ADC1 */
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    for (int i = 0; i < ROOM_COUNT; i++)
        ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, LDR_CH[i], &ccfg));
    s_adc_mtx = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "hardware ready");
}

i2c_master_bus_handle_t hw_i2c_bus(void) { return s_bus; }

void hw_door_set(door_t door, bool open)
{
    ledc_channel_t ch = door == DOOR_ENTRY ? CH_ENTRY : CH_EXIT;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, servo_duty(open ? SERVO_OPEN_DEG : SERVO_LOCKED_DEG));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

void hw_tone(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, CH_BUZZER, on ? 512 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, CH_BUZZER);
}

void hw_beep(int ms, int times)
{
    for (int i = 0; i < times; i++) {
        hw_tone(true);
        vTaskDelay(pdMS_TO_TICKS(ms));
        hw_tone(false);
        if (i + 1 < times) vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

bool hw_pir(int room) { return gpio_get_level(PIR_PINS[room]) == 1; }

void hw_light(int room, bool on) { gpio_set_level(LIGHT_PINS[room], on ? 1 : 0); }

int hw_ldr_raw(int room)
{
    int raw = -1;
    if (xSemaphoreTake(s_adc_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (adc_oneshot_read(s_adc, LDR_CH[room], &raw) != ESP_OK) raw = -1;
        xSemaphoreGive(s_adc_mtx);
    }
    return raw;
}

bool hw_button_pressed(int gpio) { return gpio_get_level(gpio) == 0; }
