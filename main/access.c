/*
 * access.c - Terminal A: main entrance.
 * Owner: Access control.
 *
 * Wokwi has no RC522 RFID part, so the RFID readers are modelled as:
 *   CARD button  - choose which card is "in your hand" (shown on the LCD)
 *   ENTRY button - tap that card on the ENTRY reader
 *   EXIT button  - tap that card on the EXIT reader
 * Any UID can also be tapped from the serial console ("tap in <uid>").
 * Both sources feed g_tap_q, so ONE task makes every door decision:
 * no two taps can ever be processed at the same time.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_state.h"
#include "schedule.h"
#include "hw.h"
#include "lcd_i2c.h"
#include "access.h"

static const char *TAG = "access";

#define UNKNOWN_DEMO_UID "0BADCAFE"   /* a card that is not registered */

static int  s_card_sel;                    /* index into users, == count -> unknown card */
static char s_card_label[17] = "Card: -";
static portMUX_TYPE s_label_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_door_close_at[2];         /* esp_timer us; 0 = closed */
static bool s_door_is_open[2];
static time_t s_unknown_ts[UNKNOWN_CARD_ALARM_COUNT];
static int s_unknown_i;

void access_get_card_label(char *buf, size_t len)
{
    taskENTER_CRITICAL(&s_label_mux);
    strlcpy(buf, s_card_label, len);
    taskEXIT_CRITICAL(&s_label_mux);
}

bool access_queue_tap(const char *uid, char dir)
{
    tap_req_t r = {.dir = dir, .source = TAP_SRC_SERIAL};
    strlcpy(r.uid, uid, UID_LEN);
    return xQueueSend(g_tap_q, &r, pdMS_TO_TICKS(100)) == pdTRUE;
}

/* Returns the UID of the currently selected demo card */
static void selected_uid(char *uid)
{
    strlcpy(uid, UNKNOWN_DEMO_UID, UID_LEN);
    char label[17];
    if (!state_lock()) return;
    if (s_card_sel > g_user_count) s_card_sel = 0;
    if (s_card_sel < g_user_count) {
        strlcpy(uid, g_users[s_card_sel].uid, UID_LEN);
        snprintf(label, sizeof(label), "Card:%.11s", g_users[s_card_sel].name);
    } else {
        snprintf(label, sizeof(label), "Card:UNKNOWN");
    }
    state_unlock();
    taskENTER_CRITICAL(&s_label_mux);
    strlcpy(s_card_label, label, sizeof(s_card_label));
    taskEXIT_CRITICAL(&s_label_mux);
}

static void open_door(door_t d)
{
    s_door_close_at[d] = esp_timer_get_time() + (int64_t)DOOR_OPEN_MS * 1000;
}

static bool unknown_card_burst(void)
{
    time_t now = time(NULL);
    s_unknown_ts[s_unknown_i] = now;
    s_unknown_i = (s_unknown_i + 1) % UNKNOWN_CARD_ALARM_COUNT;
    for (int i = 0; i < UNKNOWN_CARD_ALARM_COUNT; i++)
        if (s_unknown_ts[i] == 0 || now - s_unknown_ts[i] > UNKNOWN_CARD_WINDOW_S) return false;
    memset(s_unknown_ts, 0, sizeof(s_unknown_ts));
    return true;
}

typedef enum {
    R_GRANTED, R_EMERGENCY, R_UNKNOWN, R_BLOCKED, R_LOCKDOWN, R_SCHEDULE, R_PASSBACK_IN, R_PASSBACK_OUT
} result_t;

static void process_tap(const tap_req_t *req)
{
    sys_config_t cfg;
    cfg_get(&cfg);                                   /* copy config BEFORE taking the state lock */
    EventBits_t bits = xEventGroupGetBits(g_sys_events);
    bool open_hours = bits & EVT_BUILDING_OPEN;
    bool entry = (req->dir == 'I');

    result_t res;
    user_rec_t u = {0};
    int now_inside = 0;

    /* ---- Critical section: decide AND update atomically (no check-then-act race) ---- */
    if (!state_lock()) return;
    int idx = users_find(req->uid);
    if (idx >= 0) u = g_users[idx];
    bool privileged = idx >= 0 && (u.role == ROLE_SECURITY || u.role == ROLE_ADMIN);

    if (bits & EVT_EMERGENCY)                  res = R_EMERGENCY;
    else if (idx < 0)                          res = R_UNKNOWN;
    else if (!u.active)                        res = R_BLOCKED;
    else if ((bits & EVT_LOCKDOWN) && !privileged) res = R_LOCKDOWN;
    else if (!open_hours && !privileged)       res = R_SCHEDULE;
    else if (entry && g_inside[idx])           res = R_PASSBACK_IN;
    else if (!entry && !g_inside[idx])         res = R_PASSBACK_OUT;
    else                                       res = R_GRANTED;

    if ((res == R_GRANTED || res == R_EMERGENCY) && idx >= 0) {
        g_inside[idx] = entry;
        g_inside_since[idx] = entry ? time(NULL) : 0;
    }
    for (int i = 0; i < g_user_count; i++) if (g_inside[i]) now_inside++;
    state_unlock();
    /* ---- end critical section: slow work (servo, LCD, buzzer, log) below ---- */

    door_t door = entry ? DOOR_ENTRY : DOOR_EXIT;
    const char *name = idx >= 0 ? u.name : "UNKNOWN";
    char l2[32];
    snprintf(l2, sizeof(l2), "%.16s", name);

    switch (res) {
    case R_GRANTED:
        open_door(door);
        lcd_message(entry ? "WELCOME" : "GOODBYE", l2, 2500);
        log_event(EV_GRANTED, req->dir, req->uid, name, "%s%s, inside=%d",
                  role_name(u.role), open_hours ? "" : " after-hours", now_inside);
        hw_beep(60, 1);
        break;
    case R_EMERGENCY:
        lcd_message("EMERGENCY", "DOORS OPEN", 2000);
        log_event(EV_GRANTED, req->dir, req->uid, name, "emergency pass, inside=%d", now_inside);
        break;
    case R_UNKNOWN:
        lcd_message("ACCESS DENIED", "Unknown card", 2500);
        log_event(EV_DENIED, req->dir, req->uid, name, "unregistered card");
        hw_beep(400, 1);
        if (unknown_card_burst()) {
            log_event(EV_ALERT, req->dir, req->uid, name, "%d unknown cards in %ds - possible intrusion",
                      UNKNOWN_CARD_ALARM_COUNT, UNKNOWN_CARD_WINDOW_S);
            hw_beep(100, 6);
        }
        break;
    case R_BLOCKED:
        lcd_message("ACCESS DENIED", "Card blocked", 2500);
        log_event(EV_DENIED, req->dir, req->uid, name, "card blocked/lost");
        hw_beep(400, 1);
        break;
    case R_LOCKDOWN:
        lcd_message("ACCESS DENIED", "Lockdown active", 2500);
        log_event(EV_DENIED, req->dir, req->uid, name, "lockdown");
        hw_beep(400, 1);
        break;
    case R_SCHEDULE: {
        char hrs[32];
        snprintf(hrs, sizeof(hrs), "Hrs %02d:%02d-%02d:%02d", cfg.open_min / 60, cfg.open_min % 60,
                 cfg.close_min / 60, cfg.close_min % 60);
        lcd_message(entry ? "CLOSED: NO ENTRY" : "CLOSED: NO EXIT", hrs, 3000);
        log_event(EV_DENIED, req->dir, req->uid, name, "outside access hours");
        hw_beep(400, 1);
        if (!entry)   /* someone is stuck inside after hours: tell security */
            log_event(EV_ALERT, 'O', req->uid, name, "trying to exit after hours - security needed");
        break;
    }
    case R_PASSBACK_IN:
        lcd_message("DENIED", "Already inside", 2500);
        log_event(EV_DENIED, 'I', req->uid, name, "anti-passback: already inside");
        hw_beep(400, 1);
        break;
    case R_PASSBACK_OUT:
        lcd_message("DENIED", "Not checked in", 2500);
        log_event(EV_DENIED, 'O', req->uid, name, "anti-passback: never entered");
        hw_beep(400, 1);
        break;
    }
}

/* Simple debounced falling-edge detector */
typedef struct { int gpio; bool stable; int cnt; } btn_t;
static bool btn_pressed_edge(btn_t *b)
{
    bool raw = hw_button_pressed(b->gpio);
    if (raw == b->stable) { b->cnt = 0; return false; }
    if (++b->cnt < 3) return false;        /* ~60 ms stable */
    b->stable = raw;
    b->cnt = 0;
    return raw;                            /* true on press */
}

void access_task(void *arg)
{
    btn_t b_in  = {.gpio = PIN_BTN_ENTRY_TAP},   b_out = {.gpio = PIN_BTN_EXIT_TAP},
          b_sel = {.gpio = PIN_BTN_CARD_SELECT}, b_emg = {.gpio = PIN_BTN_EMERGENCY};
    char uid[UID_LEN];
    bool emergency_was = false;
    selected_uid(uid);
    ESP_LOGI(TAG, "Terminal A (main entrance) running");

    for (;;) {
        tap_req_t req;
        if (xQueueReceive(g_tap_q, &req, pdMS_TO_TICKS(20)) == pdTRUE) process_tap(&req);

        if (btn_pressed_edge(&b_sel)) {
            s_card_sel++;
            selected_uid(uid);
            char l[17];
            access_get_card_label(l, sizeof(l));
            lcd_message("Card in hand:", l + 5, 1200);
        }
        if (btn_pressed_edge(&b_in)) {
            tap_req_t r = {.dir = 'I', .source = TAP_SRC_READER};
            selected_uid(r.uid);
            process_tap(&r);
        }
        if (btn_pressed_edge(&b_out)) {
            tap_req_t r = {.dir = 'O', .source = TAP_SRC_READER};
            selected_uid(r.uid);
            process_tap(&r);
        }

        if (btn_pressed_edge(&b_emg)) {
            if (xEventGroupGetBits(g_sys_events) & EVT_EMERGENCY) xEventGroupClearBits(g_sys_events, EVT_EMERGENCY);
            else xEventGroupSetBits(g_sys_events, EVT_EMERGENCY);
        }

        /* Emergency mode: both doors held open (fire evacuation), lights forced on by rooms */
        bool emergency = xEventGroupGetBits(g_sys_events) & EVT_EMERGENCY;
        if (emergency != emergency_was) {
            emergency_was = emergency;
            log_event(EV_ALERT, '-', NULL, NULL, emergency ? "EMERGENCY: all doors released"
                                                           : "Emergency cleared, doors secured");
            lcd_message(emergency ? "!! EMERGENCY !!" : "Emergency over", emergency ? "Doors released" : "Doors locked", 3000);
            hw_beep(emergency ? 300 : 80, emergency ? 3 : 1);
        }

        int64_t now = esp_timer_get_time();
        for (int d = 0; d < 2; d++) {
            bool want_open = emergency || (s_door_close_at[d] && now < s_door_close_at[d]);
            if (!want_open) s_door_close_at[d] = 0;
            if (want_open != s_door_is_open[d]) {
                hw_door_set((door_t)d, want_open);
                s_door_is_open[d] = want_open;
            }
        }
    }
}
