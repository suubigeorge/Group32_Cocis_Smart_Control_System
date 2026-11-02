/*
 * rooms.c - Terminals B (Lecture Room 1) and C (Lecture Room 2).
 * Owner: Lecture room automation.
 *
 * The same code runs as two independent tasks (one per room).
 * Occupancy: a PIR detects motion; the room counts as occupied until no
 * motion has been seen for pir_timeout_s seconds (people sitting still in
 * a lecture still move enough to retrigger the PIR).
 * Lighting (AUTO mode): ON only if occupied AND the LDR says it is dark.
 * Lights are OFF when the room is empty, so nobody can leave them on.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw.h"
#include "rooms.h"

static const char *TAG = "rooms";
#define ROOM_PERIOD_MS 200

bool rooms_set_mode(int room, light_mode_t mode)
{
    if (room < 0 || room >= ROOM_COUNT) return false;
    if (!state_lock()) return false;
    g_rooms[room].mode = mode;
    state_unlock();
    log_event(EV_CONFIG, '-', NULL, NULL, "Room %d lights -> %s", room + 1,
              mode == LIGHT_AUTO ? "AUTO" : mode == LIGHT_FORCE_ON ? "FORCED ON" : "FORCED OFF");
    return true;
}

void room_task(void *arg)
{
    const int r = (int)(intptr_t)arg;
    sys_config_t cfg;
    bool prev_occupied = false, prev_light = false;
    time_t last_motion = 0, last_intruder_alert = 0;
    int ms_acc = 0;
    ESP_LOGI(TAG, "Terminal %c (Lecture Room %d) running", 'B' + r, r + 1);

    for (;;) {
        cfg_get(&cfg);
        EventBits_t bits = xEventGroupGetBits(g_sys_events);
        bool building_open = bits & EVT_BUILDING_OPEN;
        bool emergency = bits & EVT_EMERGENCY;

        /* ---- read sensors (no lock needed: hardware only) ---- */
        bool motion = hw_pir(r);
        int ldr = hw_ldr_raw(r);
        time_t now = time(NULL);
        if (motion) last_motion = now;
        bool occupied = last_motion && (now - last_motion) < cfg.pir_timeout_s;
        bool dark = ldr < 0 ? true : ldr > cfg.ldr_dark;  /* sensor fault -> assume dark (safe) */

        /* ---- read the light mode set from the console ---- */
        light_mode_t mode = LIGHT_AUTO;
        if (state_lock()) { mode = g_rooms[r].mode; state_unlock(); }

        bool light;
        if (emergency)                    light = true;            /* evacuation lighting */
        else if (mode == LIGHT_FORCE_ON)  light = true;
        else if (mode == LIGHT_FORCE_OFF) light = false;
        else                              light = occupied && dark;

        hw_light(r, light);

        /* ---- events ---- */
        if (occupied != prev_occupied) {
            log_event(EV_ROOM, '-', NULL, NULL, "Room %d %s", r + 1, occupied ? "OCCUPIED" : "EMPTY");
            prev_occupied = occupied;
        }
        if (light != prev_light) {
            log_event(EV_ROOM, '-', NULL, NULL, "Room %d lights %s%s", r + 1, light ? "ON" : "OFF",
                      (!light && !occupied && mode == LIGHT_AUTO) ? " (empty - saving power)" :
                      (!light && occupied && !dark) ? " (enough daylight)" : "");
            prev_light = light;
        }
        /* After-hours motion = possible intruder (building is closed) */
        if (motion && !building_open && !emergency && now - last_intruder_alert > 60) {
            last_intruder_alert = now;
            log_event(EV_ALERT, '-', NULL, NULL, "Motion in Room %d after hours!", r + 1);
            hw_beep(120, 3);
        }

        /* ---- publish status for the console (short critical section) ---- */
        ms_acc += ROOM_PERIOD_MS;
        bool tick = ms_acc >= 1000;
        if (tick) ms_acc -= 1000;
        if (state_lock()) {
            room_status_t *s = &g_rooms[r];
            s->motion = motion;
            s->occupied = occupied;
            s->dark = dark;
            s->light_on = light;
            s->ldr_raw = ldr;
            s->last_motion = last_motion;
            if (tick) {
                if (light) s->lit_s++;
                if (occupied) s->occupied_s++;
                if (!light && !occupied && building_open && dark) s->saved_s++;
            }
            state_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(ROOM_PERIOD_MS));
    }
}
