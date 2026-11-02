/*
 * schedule.c - Access schedule and supervisor task.
 * Owner: Data storage & time.
 *
 * The open window is [open_min, close_min). If close < open the window
 * crosses midnight (e.g. 18:00 -> 02:00 for an evening class block).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw.h"
#include "lcd_i2c.h"
#include "schedule.h"
#include "access.h"

static const char *TAG = "sched";

bool schedule_is_open_at(const sys_config_t *c, int m)
{
    if (c->open_min == c->close_min) return false;           /* closed all day */
    if (c->open_min < c->close_min) return m >= c->open_min && m < c->close_min;
    return m >= c->open_min || m < c->close_min;             /* crosses midnight */
}

int schedule_minutes_to_close(const sys_config_t *c, int m)
{
    if (!schedule_is_open_at(c, m)) return -1;
    int d = c->close_min - m;
    if (d <= 0) d += 24 * 60;
    return d;
}

static int minute_of_day(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

bool schedule_is_open_now(void)
{
    sys_config_t c;
    cfg_get(&c);
    return schedule_is_open_at(&c, minute_of_day(time(NULL)));
}

void supervisor_task(void *arg)
{
    bool first = true, was_open = false, warned = false;
    char l1[32], l2[32];
    sys_config_t cfg;

    for (;;) {
        cfg_get(&cfg);
        time_t now = time(NULL);
        int m = minute_of_day(now);
        bool open = schedule_is_open_at(&cfg, m);
        int to_close = schedule_minutes_to_close(&cfg, m);
        EventBits_t bits = xEventGroupGetBits(g_sys_events);

        /* 1. Publish open/closed state to every terminal */
        if (open) xEventGroupSetBits(g_sys_events, EVT_BUILDING_OPEN);
        else      xEventGroupClearBits(g_sys_events, EVT_BUILDING_OPEN);

        if (first || open != was_open) {
            int inside = occupancy_count();
            if (open) {
                log_event(EV_SYSTEM, '-', NULL, NULL, "Building OPEN (schedule)");
            } else {
                log_event(EV_SYSTEM, '-', NULL, NULL, "Building CLOSED (schedule)");
                if (!first && inside > 0) {
                    /* Requirement 2.6: they cannot exit now - security must be told */
                    log_event(EV_ALERT, '-', NULL, NULL, "%d person(s) still inside at closing", inside);
                    lcd_message("CLOSED: PEOPLE", "STILL INSIDE!", 4000);
                    hw_beep(150, 4);
                }
            }
            was_open = open;
            first = false;
            warned = false;
        }

        /* 2. Closing-soon warning (once per day) */
        if (open && to_close > 0 && to_close <= cfg.warn_before_min) {
            xEventGroupSetBits(g_sys_events, EVT_CLOSING_SOON);
            if (!warned) {
                warned = true;
                int inside = occupancy_count();
                log_event(EV_SYSTEM, '-', NULL, NULL, "Closing in %d min, %d inside", to_close, inside);
                snprintf(l2, sizeof(l2), "in %d min", to_close);
                lcd_message("BUILDING CLOSES", l2, 3000);
                hw_beep(80, 2);
            }
        } else {
            xEventGroupClearBits(g_sys_events, EVT_CLOSING_SOON);
        }

        /* 3. Idle screen: clock, state, occupancy */
        struct tm tm;
        localtime_r(&now, &tm);
        const char *state = (bits & EVT_EMERGENCY) ? "EMERG" :
                            (bits & EVT_LOCKDOWN)  ? "LOCK " :
                            open ? "OPEN " : "CLOSD";
        snprintf(l1, sizeof(l1), "%02d:%02d %s In:%-3d", tm.tm_hour, tm.tm_min, state, occupancy_count());
        access_get_card_label(l2, sizeof(l2));
        lcd_idle(l1, l2);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
    (void)TAG;
}
