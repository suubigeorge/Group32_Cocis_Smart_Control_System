/*
 * storage.h - Persistent storage: NVS (settings, cards) + SD card (logs)
 */
#pragma once
#include "esp_err.h"
#include "app_state.h"

esp_err_t storage_nvs_init(void);
esp_err_t storage_sd_init(void);      /* mounts SD; sets EVT_SD_OK on success */

esp_err_t storage_load_config(sys_config_t *cfg);
esp_err_t storage_save_config(const sys_config_t *cfg);
esp_err_t storage_load_users(user_rec_t *users, int *count);
esp_err_t storage_save_users(const user_rec_t *users, int count);

/* Logger task: the ONLY writer of log files -> no two tasks write the SD at once */
void logger_task(void *arg);

/* Print the last n lines of a file on the SD card (thread-safe w.r.t. logger) */
int  storage_tail(const char *file, int n);
/* Write a daily-summary/occupancy snapshot file (called by console) */
int  storage_export_inside(void);
void storage_sd_stats(uint32_t *written, uint32_t *errors);

#define FILE_ACCESS  SD_MOUNT_POINT "/ACCESS.CSV"   /* entry/exit records */
#define FILE_EVENTS  SD_MOUNT_POINT "/EVENTS.LOG"   /* rooms, alerts, config */
#define FILE_INSIDE  SD_MOUNT_POINT "/INSIDE.CSV"   /* snapshot of who is inside */
