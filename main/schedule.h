/*
 * schedule.h - Configurable building access schedule + supervisor task
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "app_state.h"

bool schedule_is_open_at(const sys_config_t *cfg, int minute_of_day);
bool schedule_is_open_now(void);
int  schedule_minutes_to_close(const sys_config_t *cfg, int minute_of_day); /* -1 if closed */

/* Watches the clock: sets/clears EVT_BUILDING_OPEN, closing warnings,
 * end-of-day report of people still inside, idle LCD screen. */
void supervisor_task(void *arg);
