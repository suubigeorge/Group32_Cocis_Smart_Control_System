/*
 * access.c - Terminal A: main entrance.
 * Owner: Access control.
 *
 * Two MFRC522 RFID readers: one on the ENTRY side, one on the EXIT side.
 *
 *   rfid task   (producer) - polls both readers over SPI, turns a card
 *                            into a tap_req_t {uid, 'I'/'O'} -> g_tap_q
 *   access task (consumer) - takes taps one at a time, decides, opens
 *                            the door, updates who is inside, logs it
 *
 * The serial command "tap in|out <uid>" feeds the same queue (handy for
 * testing cards you don't have). Because ONE task makes every door
 * decision, two taps can never be processed at the same time.
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
#include "rc522.h"

static const char *TAG = "access";

#define RFID_SPI_HOST SPI3_HOST       /* own bus, separate from the SD card (SPI2) */

static rc522_t s_reader[2];                /* [DOOR_ENTRY], [DOOR_EXIT] */
static volatile bool s_reader_ok[2];
static int64_t s_door_close_at[2];         /* esp_timer us; 0 = closed */
static bool s_door_is_open[2];
static time_t s_unknown_ts[UNKNOWN_CARD_ALARM_COUNT];
static int s_unknown_i;

/* LCD line 2 on the idle screen: invite a tap, or show a reader fault */
void access_get_card_label(char *buf, size_t len)
{
    bool in = s_reader_ok[DOOR_ENTRY], out = s_reader_ok[DOOR_EXIT];
    if (in && out)  strlcpy(buf, "Tap your card", len);
    else if (!in && !out) strlcpy(buf, "RFID FAULT IN+OUT", len);
    else strlcpy(buf, in ? "RFID FAULT: EXIT" : "RFID FAULT:ENTRY", len);
}

bool access_queue_tap(const char *uid, char dir)
{
    tap_req_t r = {.dir = dir, .source = TAP_SRC_SERIAL};
    strlcpy(r.uid, uid, UID_LEN);
    return xQueueSend(g_tap_q, &r, pdMS_TO_TICKS(100)) == pdTRUE;
}

bool access_reader_status(int door, uint8_t *version)
{
    if (door < 0 || door > 1) return false;
    if (version) *version = s_reader[door].version;
    return s_reader_ok[door];
}

/* ------------------------------------------------------------------ */
/* RFID task: the only code that talks to the two readers             */
/* ------------------------------------------------------------------ */
void rfid_task(void *arg)
{
    static const char *SIDE[2] = {"Entry", "Exit"};
    static const int CS[2] = {PIN_RFID_ENTRY_CS, PIN_RFID_EXIT_CS};
    char last_uid[2][UID_LEN] = {{0}};
    int64_t last_time[2] = {0};
    int64_t next_health = 0;

    /* The readers' own SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_RFID_MOSI, .miso_io_num = PIN_RFID_MISO, .sclk_io_num = PIN_RFID_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(RFID_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_LOGE(TAG, "RFID SPI bus: %s", esp_err_to_name(err));

    rc522_hard_reset(PIN_RFID_RST);
    for (int d = 0; d < 2; d++) {
        rc522_init(&s_reader[d], RFID_SPI_HOST, CS[d]);
        s_reader_ok[d] = s_reader[d].ok;
        log_event(s_reader_ok[d] ? EV_SYSTEM : EV_ALERT, '-', NULL, NULL, "%s RFID reader %s (ver 0x%02X)",
                  SIDE[d], s_reader_ok[d] ? "ready" : "NOT FOUND", s_reader[d].version);
    }
    ESP_LOGI(TAG, "RFID readers polled every %d ms", RFID_POLL_MS);
    /* Show the reader check on the LCD too, so a wiring fault is obvious */
    char l2[32];
    snprintf(l2, sizeof(l2), "IN:%s OUT:%s", s_reader_ok[0] ? "OK" : "FAIL", s_reader_ok[1] ? "OK" : "FAIL");
    lcd_message("RFID readers", l2, 2500);

    for (;;) {
        int64_t now = esp_timer_get_time();

        for (int d = 0; d < 2; d++) {
            if (!s_reader_ok[d]) continue;
            uint8_t uid[RC522_MAX_UID], len = 0;
            if (!rc522_read_uid(&s_reader[d], uid, &len)) continue;

            tap_req_t req = {.dir = d == DOOR_ENTRY ? 'I' : 'O', .source = TAP_SRC_READER};
            rc522_uid_to_str(uid, len, req.uid, sizeof(req.uid));

            /* A card held on the reader (Wokwi "Hold") is one tap, not many:
             * it only counts again after it has been away for a while. */
            bool same = strcmp(req.uid, last_uid[d]) == 0 && now - last_time[d] < RFID_REPEAT_IGNORE_MS * 1000LL;
            strlcpy(last_uid[d], req.uid, UID_LEN);
            last_time[d] = now;
            if (same) continue;

            printf("[rfid] %s reader read card %s\n", SIDE[d], req.uid);
            if (xQueueSend(g_tap_q, &req, pdMS_TO_TICKS(200)) != pdTRUE)
                log_event(EV_ALERT, req.dir, req.uid, NULL, "tap queue full - card ignored");
        }

        /* Health / tamper check: a reader that stops answering raises an alert */
        if (now >= next_health) {
            next_health = now + RFID_HEALTH_CHECK_S * 1000000LL;
            for (int d = 0; d < 2; d++) {
                bool ok = rc522_alive(&s_reader[d]);
                if (ok != s_reader_ok[d]) {
                    if (ok) {                       /* came back: set it up again */
                        rc522_configure(&s_reader[d]);
                        ok = s_reader[d].ok;
                    }
                    s_reader_ok[d] = ok;
                    log_event(EV_ALERT, '-', NULL, NULL, "%s RFID reader %s", SIDE[d],
                              ok ? "back ONLINE" : "OFFLINE - check wiring / tamper");
                    if (!ok) hw_beep(150, 3);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_MS));
    }
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
    btn_t b_emg = {.gpio = PIN_BTN_EMERGENCY};
    bool emergency_was = false;
    ESP_LOGI(TAG, "Terminal A (main entrance) running");

    for (;;) {
        tap_req_t req;
        if (xQueueReceive(g_tap_q, &req, pdMS_TO_TICKS(20)) == pdTRUE) process_tap(&req);

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
