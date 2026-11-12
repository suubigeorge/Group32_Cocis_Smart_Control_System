/*
 * main.c - Group32_CoCIS_Smart_Control
 *
 * Start-up order and the task map (one ESP32, three logical terminals):
 *
 *   Task        Prio  Role
 *   access      6     Terminal A - main entrance: decisions, doors, occupancy
 *   rfid        5     Terminal A - polls the ENTRY and EXIT MFRC522 readers
 *   room1/room2 5     Terminals B, C - PIR occupancy + automatic lights
 *   supervisor  4     schedule enforcement, closing warnings, LCD clock
 *   logger      3     single writer of the SD card (queue consumer)
 *   console     2     serial menus, admin login, configuration
 */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_state.h"
#include "storage.h"
#include "hw.h"
#include "lcd_i2c.h"
#include "rtc_ds1307.h"
#include "schedule.h"
#include "access.h"
#include "rooms.h"
#include "console.h"

static const char *TAG = "main";

void app_main(void)
{
    setenv("TZ", "EAT-3", 1);          /* East Africa Time, UTC+3 */
    tzset();

    ESP_ERROR_CHECK(storage_nvs_init());
    state_init();                      /* mutexes, queues, config, cards */
    hw_init();
    lcd_init();
    lcd_message("CoCIS Smart Ctrl", "Starting...", 1500);

    bool rtc = ds1307_init();
    ds1307_sync_system_time();
    storage_sd_init();                 /* logs fall back to RAM if this fails */

    /* Logger first so no start-up events are lost */
    xTaskCreate(logger_task,     "logger",     4096, NULL,            3, NULL);
    xTaskCreate(access_task,     "access",     4096, NULL,            6, NULL);
    xTaskCreate(rfid_task,       "rfid",       4096, NULL,            5, NULL);
    xTaskCreate(room_task,       "room1",      3072, (void *)0,       5, NULL);
    xTaskCreate(room_task,       "room2",      3072, (void *)1,       5, NULL);
    xTaskCreate(supervisor_task, "supervisor", 3072, NULL,            4, NULL);
    xTaskCreate(console_task,    "console",    6144, NULL,            2, NULL);

    log_event(EV_SYSTEM, '-', NULL, NULL, "System started (RTC %s, SD %s, LCD %s)",
              rtc ? "ok" : "missing",
              (xEventGroupGetBits(g_sys_events) & EVT_SD_OK) ? "ok" : "missing",
              lcd_ok() ? "ok" : "missing");
    ESP_LOGI(TAG, "all terminals running");
}

/*
//For use in the wokwi online browser to prevent arduino-core calling app_main()
#if defined(ARDUINO)
void arduino_setup(void) __asm__("_Z5setupv");
void arduino_loop(void)  __asm__("_Z4loopv");
void arduino_setup(void) { system_start(); }
void arduino_loop(void)  { vTaskDelay(pdMS_TO_TICKS(1000)); }   // all work is in our tasks 
#else
void app_main(void) { system_start(); }
#endif
*/