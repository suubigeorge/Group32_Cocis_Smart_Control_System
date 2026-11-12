/*
 * storage.c - NVS + SD card storage and the logger task.
 * Owner: Data storage & time.
 *
 * Design: tasks never write the SD card directly. They call log_event(),
 * which drops a record into g_log_q. The logger task is the single
 * consumer: it writes the record to the SD card and to a RAM ring buffer.
 * Readers (the console "sdlog" command) take s_sd_mtx so they never read
 * a file while it is half written.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "storage.h"

static const char *TAG = "storage";
static SemaphoreHandle_t s_sd_mtx;
static sdmmc_card_t *s_card;
static uint32_t s_written, s_errors;

#define NVS_NS        "cocis"
#define NVS_KEY_CFG   "cfg"
#define NVS_KEY_USERS "users2"   /* v2: 16-byte UIDs for 7-byte RFID cards */

/* ---------------- NVS ---------------- */
esp_err_t storage_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_sd_mtx = xSemaphoreCreateMutex();
    return err;
}

static esp_err_t nvs_get(const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_put(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_load_config(sys_config_t *cfg)
{
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_get(NVS_KEY_CFG, cfg, &len);
    return (err == ESP_OK && len == sizeof(*cfg)) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_save_config(const sys_config_t *cfg)
{
    return nvs_put(NVS_KEY_CFG, cfg, sizeof(*cfg));
}

esp_err_t storage_load_users(user_rec_t *users, int *count)
{
    size_t len = sizeof(user_rec_t) * MAX_USERS;
    esp_err_t err = nvs_get(NVS_KEY_USERS, users, &len);
    if (err != ESP_OK || len % sizeof(user_rec_t)) return ESP_FAIL;
    *count = len / sizeof(user_rec_t);
    return ESP_OK;
}

esp_err_t storage_save_users(const user_rec_t *users, int count)
{
    if (count == 0) {                   /* nvs can't store a 0-length blob */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_USERS); nvs_commit(h); nvs_close(h);
        }
        return ESP_OK;
    }
    return nvs_put(NVS_KEY_USERS, users, sizeof(user_rec_t) * count);
}

/* ---------------- SD card ---------------- */
static void ensure_header(const char *path, const char *header)
{
    struct stat st;
    if (stat(path, &st) == 0) return;
    FILE *f = fopen(path, "w");
    if (f) { fputs(header, f); fclose(f); }
}

esp_err_t storage_sd_init(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (err != ESP_OK) { ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err)); return err; }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed (%s) - logs kept in RAM only", esp_err_to_name(err));
        return err;
    }
    ensure_header(FILE_ACCESS, "timestamp,result,direction,uid,name,detail\n");
    ensure_header(FILE_EVENTS, "timestamp,type,detail\n");
    xEventGroupSetBits(g_sys_events, EVT_SD_OK);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

static void write_event_to_sd(const log_event_t *ev)
{
    char ts[24];
    fmt_time(ev->ts, ts, sizeof(ts));
    bool is_access = (ev->type == EV_GRANTED || ev->type == EV_DENIED) && ev->dir != '-';

    if (xSemaphoreTake(s_sd_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) { s_errors++; return; }
    FILE *f = fopen(is_access ? FILE_ACCESS : FILE_EVENTS, "a");
    if (f) {
        if (is_access)
            fprintf(f, "%s,%s,%s,%s,%s,%s\n", ts, event_type_name(ev->type),
                    ev->dir == 'I' ? "ENTRY" : "EXIT", ev->uid, ev->name, ev->msg);
        else
            fprintf(f, "%s,%s,%s\n", ts, event_type_name(ev->type), ev->msg);
        fclose(f);
        s_written++;
    } else {
        s_errors++;
    }
    xSemaphoreGive(s_sd_mtx);
}

void logger_task(void *arg)
{
    log_event_t ev;
    char ts[24];
    for (;;) {
        if (xQueueReceive(g_log_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        recent_push(&ev);
        fmt_time(ev.ts, ts, sizeof(ts));
        /* Live feed on the serial monitor */
        printf("\n[%s] %-7s %c %-14s %-15s %s\n", ts + 11, event_type_name(ev.type),
               ev.dir, ev.uid, ev.name, ev.msg);
        if (xEventGroupGetBits(g_sys_events) & EVT_SD_OK) write_event_to_sd(&ev);
    }
}

int storage_tail(const char *file, int n)
{
    if (!(xEventGroupGetBits(g_sys_events) & EVT_SD_OK)) { printf("SD card not available.\n"); return -1; }
    if (n <= 0 || n > 50) n = 10;
    static char lines[50][112];      /* only the console task calls this */
    int count = 0, total = 0;

    if (xSemaphoreTake(s_sd_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;
    FILE *f = fopen(file, "r");
    if (!f) { xSemaphoreGive(s_sd_mtx); printf("Cannot open %s\n", file); return -1; }
    char buf[112];
    while (fgets(buf, sizeof(buf), f)) {
        strlcpy(lines[count % n], buf, sizeof(lines[0]));
        count++; total++;
    }
    fclose(f);
    xSemaphoreGive(s_sd_mtx);

    int shown = count < n ? count : n;
    int start = count < n ? 0 : count % n;
    printf("--- last %d of %d lines in %s ---\n", shown, total, file);
    for (int i = 0; i < shown; i++) printf("%s", lines[(start + i) % n]);
    return shown;
}

int storage_export_inside(void)
{
    if (!(xEventGroupGetBits(g_sys_events) & EVT_SD_OK)) { printf("SD card not available.\n"); return -1; }
    /* 1. copy shared data under the state lock */
    static user_rec_t users[MAX_USERS];
    static time_t since[MAX_USERS];
    int n = 0;
    if (!state_lock()) return -1;
    for (int i = 0; i < g_user_count; i++)
        if (g_inside[i]) { users[n] = g_users[i]; since[n] = g_inside_since[i]; n++; }
    state_unlock();

    /* 2. write the file under the SD lock (never both locks at once) */
    char ts[24];
    if (xSemaphoreTake(s_sd_mtx, pdMS_TO_TICKS(1000)) != pdTRUE) return -1;
    FILE *f = fopen(FILE_INSIDE, "w");
    if (f) {
        fmt_time(time(NULL), ts, sizeof(ts));
        fprintf(f, "# snapshot %s, %d inside\nuid,name,role,inside_since\n", ts, n);
        for (int i = 0; i < n; i++) {
            fmt_time(since[i], ts, sizeof(ts));
            fprintf(f, "%s,%s,%s,%s\n", users[i].uid, users[i].name, role_name(users[i].role), ts);
        }
        fclose(f);
    }
    xSemaphoreGive(s_sd_mtx);
    return f ? n : -1;
}

void storage_sd_stats(uint32_t *written, uint32_t *errors)
{
    *written = s_written;
    *errors = s_errors;
}
