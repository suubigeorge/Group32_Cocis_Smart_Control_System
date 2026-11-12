/*
 * console.c - Serial management interface + authentication.
 * Owner: Interfaces & security.
 *
 * Anyone at the serial monitor can VIEW information.
 * Changing anything (schedule, cards, clock, lights, emergency) requires
 * an admin login:
 *   - password stored only as salted SHA-256 (never in plain text)
 *   - input is masked with '*'
 *   - 3 wrong passwords -> console locked for 60 s (+ ALERT logged)
 *   - session auto-logout after 5 minutes idle
 *   - every configuration change is written to the audit log
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "app_state.h"
#include "storage.h"
#include "schedule.h"
#include "rtc_ds1307.h"
#include "access.h"
#include "rooms.h"
#include "console.h"

void state_hash_password(const uint8_t salt[8], const char *pw, uint8_t out[32]);

#define CON_UART UART_NUM_0
#define CON_LINE_MAX 96
#define MAX_ARGS 8

static bool   s_admin;
static time_t s_last_activity;
static int    s_failed;
static time_t s_locked_until;

/* ------------------------------------------------------------------ */
/* Line input                                                          */
/* ------------------------------------------------------------------ */
static int read_line(char *buf, int max, bool mask)
{
    int n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CON_UART, &c, 1, pdMS_TO_TICKS(100)) != 1) {
            /* idle: enforce session timeout even while waiting */
            if (s_admin && time(NULL) - s_last_activity > SESSION_TIMEOUT_S) {
                s_admin = false;
                printf("\n[session timed out - logged out]\n> ");
                fflush(stdout);
                log_event(EV_CONFIG, '-', NULL, NULL, "Admin session timed out");
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n == 0 && c == '\n') continue;      /* swallow LF after CR */
            buf[n] = 0;
            printf("\n");
            return n;
        }
        if ((c == 8 || c == 127) && n > 0) { n--; printf("\b \b"); fflush(stdout); continue; }
        if (isprint(c) && n < max - 1) {
            buf[n++] = (char)c;
            putchar(mask ? '*' : c);
            fflush(stdout);
        }
    }
}

static int split(char *line, char **argv)
{
    int argc = 0;
    char *p = strtok(line, " \t");
    while (p && argc < MAX_ARGS) { argv[argc++] = p; p = strtok(NULL, " \t"); }
    return argc;
}

static bool parse_hhmm(const char *s, uint16_t *out)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *out = h * 60 + m;
    return true;
}

/* "01:02:03:04" or "01 02 03 04" or "01020304" -> "01020304" */
static void norm_uid(char *u)
{
    char *w = u;
    for (char *r = u; *r; r++)
        if (isxdigit((unsigned char)*r)) *w++ = toupper((unsigned char)*r);
    *w = 0;
}

static bool require_admin(void)
{
    if (s_admin) return true;
    printf("Permission denied: type 'login' first.\n");
    return false;
}

/* ------------------------------------------------------------------ */
/* View commands                                                       */
/* ------------------------------------------------------------------ */
static void cmd_help(void)
{
    printf(
        "\n===================== CoCIS SMART CONTROL - MENU =====================\n"
        " VIEW (anyone)\n"
        "  status                 system overview\n"
        "  inside                 who is in the building now\n"
        "  rooms                  room occupancy, light & LDR status\n"
        "  energy                 lighting energy used / saved\n"
        "  users                  registered RFID cards\n"
        "  log [n]                last n events (RAM)\n"
        "  sdlog access|events [n]  last n lines from SD card files\n"
        "  schedule               show access hours\n"
        "  time                   show date/time\n"
        "  tap in|out <uid>       present a card UID without a reader (test)\n"
        "  readers                RFID reader status\n"
        "  login | logout | whoami\n"
        " ADMIN (login required)\n"
        "  setsched HH:MM HH:MM   set access hours (open close)\n"
        "  warn <min>             closing warning lead time\n"
        "  settime YYYY-MM-DD HH:MM[:SS]\n"
        "  adduser <uid> <student|staff|security|admin> <name...>\n"
        "  deluser <uid> | block <uid> | unblock <uid>\n"
        "  checkout <uid>         correct a record (mark person as outside)\n"
        "  light <1|2> <auto|on|off>\n"
        "  timeout <s>            PIR vacancy timeout\n"
        "  ldr <0-4095>           darkness threshold\n"
        "  emergency on|off       release / secure all doors\n"
        "  lockdown on|off        refuse all non-security cards\n"
        "  export                 write INSIDE.CSV snapshot to SD\n"
        "  passwd                 change admin password\n"
        "=======================================================================\n");
}

static void cmd_time(void)
{
    char ts[24];
    fmt_time(time(NULL), ts, sizeof(ts));
    printf("Current time: %s\n", ts);
}

static void cmd_schedule(void)
{
    sys_config_t c;
    cfg_get(&c);
    printf("Access hours: %02d:%02d - %02d:%02d  (now %s), closing warning %d min before\n",
           c.open_min / 60, c.open_min % 60, c.close_min / 60, c.close_min % 60,
           schedule_is_open_now() ? "OPEN" : "CLOSED", c.warn_before_min);
    printf("Outside these hours nobody may ENTER or EXIT except SECURITY/ADMIN cards,\n"
           "unless emergency mode is on.\n");
}

static void cmd_status(void)
{
    sys_config_t c;
    cfg_get(&c);
    EventBits_t b = xEventGroupGetBits(g_sys_events);
    room_status_t rooms[ROOM_COUNT];
    int inside = 0, users = 0;
    if (state_lock()) {
        memcpy(rooms, g_rooms, sizeof(rooms));
        users = g_user_count;
        for (int i = 0; i < g_user_count; i++) if (g_inside[i]) inside++;
        state_unlock();
    }
    uint32_t w, e;
    storage_sd_stats(&w, &e);
    char ts[24];
    fmt_time(time(NULL), ts, sizeof(ts));
    printf("\n----------------------- SYSTEM STATUS -----------------------\n");
    printf(" Time            : %s\n", ts);
    printf(" Building        : %s  (hours %02d:%02d-%02d:%02d)%s\n", (b & EVT_BUILDING_OPEN) ? "OPEN" : "CLOSED",
           c.open_min / 60, c.open_min % 60, c.close_min / 60, c.close_min % 60,
           (b & EVT_CLOSING_SOON) ? "  * closing soon *" : "");
    printf(" Emergency       : %s   Lockdown: %s\n", (b & EVT_EMERGENCY) ? "ACTIVE" : "off",
           (b & EVT_LOCKDOWN) ? "ACTIVE" : "off");
    printf(" People inside   : %d  (of %d registered cards)\n", inside, users);
    for (int r = 0; r < ROOM_COUNT; r++)
        printf(" Lecture Room %d  : %-8s lights %-3s mode %s\n", r + 1, rooms[r].occupied ? "OCCUPIED" : "empty",
               rooms[r].light_on ? "ON" : "OFF",
               rooms[r].mode == LIGHT_AUTO ? "AUTO" : rooms[r].mode == LIGHT_FORCE_ON ? "FORCED ON" : "FORCED OFF");
    for (int d = 0; d < 2; d++) {
        uint8_t ver = 0;
        bool ok = access_reader_status(d, &ver);
        printf(" %s reader    : %s (chip 0x%02X)\n", d ? "Exit " : "Entry", ok ? "OK" : "NOT RESPONDING", ver);
    }
    printf(" SD card         : %s  (%lu records written, %lu errors, %lu dropped)\n",
           (b & EVT_SD_OK) ? "OK" : "NOT AVAILABLE", (unsigned long)w, (unsigned long)e,
           (unsigned long)log_dropped_count());
    printf(" Free heap       : %lu bytes   Uptime: %llu s\n", (unsigned long)esp_get_free_heap_size(),
           (unsigned long long)(esp_timer_get_time() / 1000000));
    printf(" Console user    : %s\n", s_admin ? "ADMIN" : "guest (view only)");
    printf("-------------------------------------------------------------\n");
}

static void cmd_inside(void)
{
    static user_rec_t u[MAX_USERS];
    static time_t since[MAX_USERS];
    int n = 0;
    if (!state_lock()) return;
    for (int i = 0; i < g_user_count; i++)
        if (g_inside[i]) { u[n] = g_users[i]; since[n] = g_inside_since[i]; n++; }
    state_unlock();
    printf("People currently inside: %d\n", n);
    char ts[24];
    for (int i = 0; i < n; i++) {
        fmt_time(since[i], ts, sizeof(ts));
        printf("  %-14s %-16s %-8s since %s\n", u[i].uid, u[i].name, role_name(u[i].role), ts + 11);
    }
}

static void cmd_rooms(void)
{
    room_status_t r[ROOM_COUNT];
    sys_config_t c;
    cfg_get(&c);
    if (!state_lock()) return;
    memcpy(r, g_rooms, sizeof(r));
    state_unlock();
    time_t now = time(NULL);
    printf("Room  Motion  Occupied  LDR   Dark  Light  Mode        Last motion\n");
    for (int i = 0; i < ROOM_COUNT; i++) {
        char lm[16] = "never";
        if (r[i].last_motion) snprintf(lm, sizeof(lm), "%lds ago", (long)(now - r[i].last_motion));
        printf("  %d   %-6s  %-8s  %-4d  %-4s  %-5s  %-10s  %s\n", i + 1, r[i].motion ? "yes" : "no",
               r[i].occupied ? "YES" : "no", r[i].ldr_raw, r[i].dark ? "yes" : "no",
               r[i].light_on ? "ON" : "off",
               r[i].mode == LIGHT_AUTO ? "AUTO" : r[i].mode == LIGHT_FORCE_ON ? "FORCED ON" : "FORCED OFF", lm);
    }
    printf("Vacancy timeout %ds, dark threshold %d\n", c.pir_timeout_s, c.ldr_dark);
}

static void cmd_energy(void)
{
    room_status_t r[ROOM_COUNT];
    sys_config_t c;
    cfg_get(&c);
    if (!state_lock()) return;
    memcpy(r, g_rooms, sizeof(r));
    state_unlock();
    printf("Lighting energy since boot (load %d W per room):\n", c.room_watts);
    double tot_used = 0, tot_saved = 0;
    for (int i = 0; i < ROOM_COUNT; i++) {
        double used = r[i].lit_s * (double)c.room_watts / 3600.0;
        double saved = r[i].saved_s * (double)c.room_watts / 3600.0;
        tot_used += used; tot_saved += saved;
        printf("  Room %d: lights on %lus, occupied %lus, used %.2f Wh, saved %.2f Wh\n", i + 1,
               (unsigned long)r[i].lit_s, (unsigned long)r[i].occupied_s, used, saved);
    }
    printf("  Total used %.2f Wh, saved %.2f Wh (vs lights left on in empty dark rooms)\n", tot_used, tot_saved);
}

static void cmd_users(void)
{
    static user_rec_t u[MAX_USERS];
    static bool in[MAX_USERS];
    int n;
    if (!state_lock()) return;
    n = g_user_count;
    memcpy(u, g_users, sizeof(u));
    memcpy(in, g_inside, sizeof(in));
    state_unlock();
    printf("UID            Name             Role      Card     Location\n");
    for (int i = 0; i < n; i++)
        printf("%-14s %-16s %-9s %-8s %s\n", u[i].uid, u[i].name, role_name(u[i].role),
               u[i].active ? "active" : "BLOCKED", in[i] ? "INSIDE" : "outside");
    printf("%d of %d slots used. Wokwi Red card AA:BB:CC:DD is unregistered (for the denied demo).\n", n, MAX_USERS);
}

static void cmd_log(int n)
{
    static log_event_t ev[RECENT_LOG_LEN];
    if (n <= 0 || n > RECENT_LOG_LEN) n = 15;
    int got = recent_copy(ev, n);
    char ts[24];
    for (int i = 0; i < got; i++) {
        fmt_time(ev[i].ts, ts, sizeof(ts));
        printf("%s %-7s %c %-14s %-15s %s\n", ts, event_type_name(ev[i].type), ev[i].dir,
               ev[i].uid, ev[i].name, ev[i].msg);
    }
    if (!got) printf("(no events yet)\n");
}

/* ------------------------------------------------------------------ */
/* Authentication                                                      */
/* ------------------------------------------------------------------ */
static bool check_password(const char *pw)
{
    sys_config_t c;
    cfg_get(&c);
    uint8_t h[32];
    state_hash_password(c.pw_salt, pw, h);
    uint8_t diff = 0;                       /* constant-time compare */
    for (int i = 0; i < 32; i++) diff |= h[i] ^ c.pw_hash[i];
    return diff == 0;
}

static void cmd_login(void)
{
    time_t now = time(NULL);
    if (s_admin) { printf("Already logged in.\n"); return; }
    if (now < s_locked_until) {
        printf("Too many failed attempts. Try again in %ld s.\n", (long)(s_locked_until - now));
        return;
    }
    char pw[40];
    printf("Password: ");
    fflush(stdout);
    read_line(pw, sizeof(pw), true);
    if (check_password(pw)) {
        s_admin = true;
        s_failed = 0;
        s_last_activity = time(NULL);
        printf("Login OK. Admin commands enabled (auto-logout after %d s idle).\n", SESSION_TIMEOUT_S);
        log_event(EV_CONFIG, '-', NULL, "admin", "Admin logged in");
        if (strcmp(pw, DEFAULT_ADMIN_PASSWORD) == 0)
            printf("WARNING: default password in use - change it with 'passwd'.\n");
    } else {
        s_failed++;
        printf("Wrong password (%d/%d).\n", s_failed, LOGIN_MAX_ATTEMPTS);
        log_event(EV_DENIED, '-', NULL, "console", "Failed admin login %d/%d", s_failed, LOGIN_MAX_ATTEMPTS);
        if (s_failed >= LOGIN_MAX_ATTEMPTS) {
            s_locked_until = time(NULL) + LOGIN_LOCKOUT_S;
            s_failed = 0;
            printf("Console locked for %d s.\n", LOGIN_LOCKOUT_S);
            log_event(EV_ALERT, '-', NULL, "console", "Console locked: repeated wrong passwords");
        }
    }
    memset(pw, 0, sizeof(pw));
}

static void cmd_passwd(void)
{
    char a[40], b[40];
    printf("Current password: "); fflush(stdout); read_line(a, sizeof(a), true);
    if (!check_password(a)) { printf("Wrong password.\n"); return; }
    printf("New password (min 6 chars): "); fflush(stdout); read_line(a, sizeof(a), true);
    printf("Repeat new password: "); fflush(stdout); read_line(b, sizeof(b), true);
    if (strlen(a) < 6 || strcmp(a, b) != 0) { printf("Passwords too short or do not match.\n"); return; }
    sys_config_t c;
    cfg_get(&c);
    esp_fill_random(c.pw_salt, sizeof(c.pw_salt));
    state_hash_password(c.pw_salt, a, c.pw_hash);
    printf(cfg_update(&c) ? "Password changed.\n" : "Failed to save.\n");
    log_event(EV_CONFIG, '-', NULL, "admin", "Admin password changed");
    memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b));
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */
static void handle(int argc, char **argv)
{
    const char *cmd = argv[0];
    for (char *p = argv[0]; *p; p++) *p = tolower((unsigned char)*p);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?") || !strcmp(cmd, "menu")) cmd_help();
    else if (!strcmp(cmd, "status"))   cmd_status();
    else if (!strcmp(cmd, "inside"))   cmd_inside();
    else if (!strcmp(cmd, "rooms"))    cmd_rooms();
    else if (!strcmp(cmd, "energy"))   cmd_energy();
    else if (!strcmp(cmd, "users"))    cmd_users();
    else if (!strcmp(cmd, "time"))     cmd_time();
    else if (!strcmp(cmd, "schedule")) cmd_schedule();
    else if (!strcmp(cmd, "log"))      cmd_log(argc > 1 ? atoi(argv[1]) : 15);
    else if (!strcmp(cmd, "sdlog")) {
        const char *f = (argc > 1 && !strcmp(argv[1], "events")) ? FILE_EVENTS : FILE_ACCESS;
        storage_tail(f, argc > 2 ? atoi(argv[2]) : 10);
    }
    else if (!strcmp(cmd, "tap")) {
        if (argc < 3 || (strcmp(argv[1], "in") && strcmp(argv[1], "out"))) { printf("usage: tap in|out <uid>\n"); return; }
        norm_uid(argv[2]);
        if (!access_queue_tap(argv[2], argv[1][0] == 'i' ? 'I' : 'O')) printf("Entrance busy, try again.\n");
    }
    else if (!strcmp(cmd, "readers")) {
        for (int d = 0; d < 2; d++) {
            uint8_t ver = 0;
            bool ok = access_reader_status(d, &ver);
            printf("%s reader (CS GPIO%d): %s, MFRC522 version 0x%02X%s\n", d ? "EXIT " : "ENTRY",
                   d ? PIN_RFID_EXIT_CS : PIN_RFID_ENTRY_CS, ok ? "OK" : "NOT RESPONDING", ver,
                   ver == 0x91 ? " (v1.0)" : ver == 0x92 ? " (v2.0)" : "");
        }
    }
    else if (!strcmp(cmd, "login"))  cmd_login();
    else if (!strcmp(cmd, "logout")) {
        if (s_admin) log_event(EV_CONFIG, '-', NULL, "admin", "Admin logged out");
        s_admin = false; printf("Logged out.\n");
    }
    else if (!strcmp(cmd, "whoami")) printf("%s\n", s_admin ? "admin" : "guest (view only)");

    /* ---------------- admin-only ---------------- */
    else if (!strcmp(cmd, "setsched")) {
        if (!require_admin()) return;
        sys_config_t c; cfg_get(&c);
        uint16_t o, cl;
        if (argc < 3 || !parse_hhmm(argv[1], &o) || !parse_hhmm(argv[2], &cl)) { printf("usage: setsched 07:00 22:00\n"); return; }
        c.open_min = o; c.close_min = cl;
        if (cfg_update(&c)) {
            printf("Access hours set to %s - %s\n", argv[1], argv[2]);
            log_event(EV_CONFIG, '-', NULL, "admin", "Schedule set %s-%s", argv[1], argv[2]);
        }
    }
    else if (!strcmp(cmd, "warn")) {
        if (!require_admin()) return;
        int v = argc > 1 ? atoi(argv[1]) : -1;
        if (v < 0 || v > 120) { printf("usage: warn <0-120>\n"); return; }
        sys_config_t c; cfg_get(&c); c.warn_before_min = v; cfg_update(&c);
        printf("Closing warning: %d min\n", v);
        log_event(EV_CONFIG, '-', NULL, "admin", "Closing warning %d min", v);
    }
    else if (!strcmp(cmd, "settime")) {
        if (!require_admin()) return;
        struct tm tm = {0};
        int Y, M, D, h, m, s = 0;
        if (argc < 3 || sscanf(argv[1], "%d-%d-%d", &Y, &M, &D) != 3 || sscanf(argv[2], "%d:%d:%d", &h, &m, &s) < 2) {
            printf("usage: settime 2026-11-02 08:30[:00]\n"); return;
        }
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s;
        bool rtc_ok = ds1307_set_time(&tm);
        cmd_time();
        if (!rtc_ok) printf("(RTC write failed - time kept until reboot only)\n");
        log_event(EV_CONFIG, '-', NULL, "admin", "Clock set %s %s", argv[1], argv[2]);
    }
    else if (!strcmp(cmd, "adduser")) {
        if (!require_admin()) return;
        if (argc < 4) { printf("usage: adduser <uid> <student|staff|security|admin> <name>\n"); return; }
        role_t role;
        if (!strcmp(argv[2], "student")) role = ROLE_STUDENT;
        else if (!strcmp(argv[2], "staff")) role = ROLE_STAFF;
        else if (!strcmp(argv[2], "security")) role = ROLE_SECURITY;
        else if (!strcmp(argv[2], "admin")) role = ROLE_ADMIN;
        else { printf("Unknown role.\n"); return; }
        char name[NAME_LEN] = "";
        for (int i = 3; i < argc; i++) {
            if (i > 3) strlcat(name, " ", sizeof(name));
            strlcat(name, argv[i], sizeof(name));
        }
        norm_uid(argv[1]);
        if (strlen(argv[1]) >= UID_LEN) { printf("UID too long.\n"); return; }
        if (users_add(argv[1], name, role)) {
            printf("Card %s registered to %s (%s)\n", argv[1], name, role_name(role));
            log_event(EV_CONFIG, '-', argv[1], name, "Card registered (%s)", role_name(role));
        } else printf("Card table full.\n");
    }
    else if (!strcmp(cmd, "deluser") || !strcmp(cmd, "block") || !strcmp(cmd, "unblock")) {
        if (!require_admin()) return;
        if (argc < 2) { printf("usage: %s <uid>\n", cmd); return; }
        norm_uid(argv[1]);
        bool ok = !strcmp(cmd, "deluser") ? users_remove(argv[1]) : users_set_active(argv[1], !strcmp(cmd, "unblock"));
        printf(ok ? "Done.\n" : "Card not found.\n");
        if (ok) log_event(EV_CONFIG, '-', argv[1], NULL, "Card %s", !strcmp(cmd, "deluser") ? "deleted" :
                                                                      !strcmp(cmd, "block") ? "blocked" : "unblocked");
    }
    else if (!strcmp(cmd, "checkout")) {
        if (!require_admin()) return;
        if (argc < 2) { printf("usage: checkout <uid>\n"); return; }
        norm_uid(argv[1]);
        bool ok = false;
        if (state_lock()) {
            int i = users_find(argv[1]);
            if (i >= 0 && g_inside[i]) { g_inside[i] = false; ok = true; }
            state_unlock();
        }
        printf(ok ? "Marked as outside.\n" : "Card not found or not inside.\n");
        if (ok) log_event(EV_CONFIG, 'O', argv[1], NULL, "Manual checkout by admin");
    }
    else if (!strcmp(cmd, "light")) {
        if (!require_admin()) return;
        int r = argc > 1 ? atoi(argv[1]) : 0;
        if (argc < 3 || r < 1 || r > ROOM_COUNT) { printf("usage: light <1|2> <auto|on|off>\n"); return; }
        light_mode_t m = !strcmp(argv[2], "on") ? LIGHT_FORCE_ON : !strcmp(argv[2], "off") ? LIGHT_FORCE_OFF : LIGHT_AUTO;
        rooms_set_mode(r - 1, m);
        printf("Room %d lights: %s\n", r, argv[2]);
    }
    else if (!strcmp(cmd, "timeout") || !strcmp(cmd, "ldr")) {
        if (!require_admin()) return;
        int v = argc > 1 ? atoi(argv[1]) : -1;
        sys_config_t c; cfg_get(&c);
        if (!strcmp(cmd, "timeout")) {
            if (v < 5 || v > 3600) { printf("usage: timeout <5-3600>\n"); return; }
            c.pir_timeout_s = v;
        } else {
            if (v < 0 || v > 4095) { printf("usage: ldr <0-4095>\n"); return; }
            c.ldr_dark = v;
        }
        cfg_update(&c);
        printf("%s set to %d\n", cmd, v);
        log_event(EV_CONFIG, '-', NULL, "admin", "%s = %d", cmd, v);
    }
    else if (!strcmp(cmd, "emergency") || !strcmp(cmd, "lockdown")) {
        if (!require_admin()) return;
        if (argc < 2) { printf("usage: %s on|off\n", cmd); return; }
        EventBits_t bit = !strcmp(cmd, "emergency") ? EVT_EMERGENCY : EVT_LOCKDOWN;
        bool on = !strcmp(argv[1], "on");
        if (on) xEventGroupSetBits(g_sys_events, bit); else xEventGroupClearBits(g_sys_events, bit);
        printf("%s %s\n", cmd, on ? "ON" : "OFF");
        log_event(bit == EVT_EMERGENCY ? EV_ALERT : EV_CONFIG, '-', NULL, "admin", "%s %s via console", cmd, on ? "ON" : "OFF");
    }
    else if (!strcmp(cmd, "export")) {
        if (!require_admin()) return;
        int n = storage_export_inside();
        if (n >= 0) printf("Wrote %s (%d people)\n", FILE_INSIDE, n);
    }
    else if (!strcmp(cmd, "passwd")) { if (require_admin()) cmd_passwd(); }
    else printf("Unknown command '%s'. Type 'help'.\n", cmd);
}

void console_task(void *arg)
{
    uart_driver_install(CON_UART, 1024, 0, 0, NULL, 0);
    char line[CON_LINE_MAX];
    char *argv[MAX_ARGS];
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\nType 'help' for the management menu.\n");
    for (;;) {
        printf("%s> ", s_admin ? "admin" : "cocis");
        fflush(stdout);
        if (read_line(line, sizeof(line), false) == 0) continue;
        if (s_admin) s_last_activity = time(NULL);
        int argc = split(line, argv);
        if (argc) handle(argc, argv);
    }
}
