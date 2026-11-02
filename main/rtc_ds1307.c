/*
 * rtc_ds1307.c - DS1307 RTC driver.
 * Owner: Data storage & time.
 */
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "config.h"
#include "hw.h"
#include "rtc_ds1307.h"

static const char *TAG = "rtc";
static i2c_master_dev_handle_t s_dev;
static bool s_ok;

static uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool ds1307_init(void)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = RTC_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(hw_i2c_bus(), &dc, &s_dev) != ESP_OK) return false;
    struct tm tm;
    s_ok = true;
    s_ok = ds1307_read(&tm);
    return s_ok;
}

bool ds1307_read(struct tm *tm)
{
    if (!s_dev) return false;
    uint8_t reg = 0, d[7];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, 7, 100) != ESP_OK) return false;
    memset(tm, 0, sizeof(*tm));
    tm->tm_sec  = bcd2dec(d[0] & 0x7F);
    tm->tm_min  = bcd2dec(d[1] & 0x7F);
    tm->tm_hour = bcd2dec(d[2] & 0x3F);
    tm->tm_mday = bcd2dec(d[4] & 0x3F);
    tm->tm_mon  = bcd2dec(d[5] & 0x1F) - 1;
    tm->tm_year = bcd2dec(d[6]) + 100;          /* 2000-based */
    tm->tm_isdst = -1;
    return tm->tm_mon >= 0 && tm->tm_mon < 12 && tm->tm_mday >= 1;
}

bool ds1307_write(const struct tm *tm)
{
    if (!s_dev) return false;
    uint8_t d[8] = {
        0x00,
        dec2bcd(tm->tm_sec) & 0x7F,                 /* CH bit = 0 -> clock runs */
        dec2bcd(tm->tm_min),
        dec2bcd(tm->tm_hour),                       /* 24h mode */
        dec2bcd(tm->tm_wday + 1),
        dec2bcd(tm->tm_mday),
        dec2bcd(tm->tm_mon + 1),
        dec2bcd(tm->tm_year % 100),
    };
    return i2c_master_transmit(s_dev, d, sizeof(d), 100) == ESP_OK;
}

static void set_system(struct tm *tm)
{
    time_t t = mktime(tm);
    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, NULL);
}

void ds1307_sync_system_time(void)
{
    struct tm tm;
    if (s_ok && ds1307_read(&tm)) {
        set_system(&tm);
        ESP_LOGI(TAG, "time from DS1307: %04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = 2026 - 1900; tm.tm_mon = 10; tm.tm_mday = 2; tm.tm_hour = 8;
        tm.tm_isdst = -1;
        set_system(&tm);
        ESP_LOGW(TAG, "RTC not available - clock set to 2026-11-02 08:00 (use 'settime')");
    }
}

bool ds1307_set_time(const struct tm *in)
{
    struct tm tm = *in;
    tm.tm_isdst = -1;
    set_system(&tm);             /* also normalises tm_wday */
    time_t t = time(NULL);
    localtime_r(&t, &tm);
    return ds1307_write(&tm);
}
