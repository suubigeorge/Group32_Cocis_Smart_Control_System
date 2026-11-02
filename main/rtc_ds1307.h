/*
 * rtc_ds1307.h - DS1307 real-time clock (keeps time for schedules and logs)
 */
#pragma once
#include <stdbool.h>
#include <time.h>

bool ds1307_init(void);                 /* true if the chip answers */
bool ds1307_read(struct tm *tm);
bool ds1307_write(const struct tm *tm);
/* Read RTC and set the ESP32 system clock from it (falls back to 08:00 today). */
void ds1307_sync_system_time(void);
/* Set both RTC and system clock */
bool ds1307_set_time(const struct tm *tm);
