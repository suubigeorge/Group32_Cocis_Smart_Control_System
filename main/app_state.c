/*
 * app_state.c - Shared state, mutexes, queues and the event group.
 * Owner: Communication & synchronisation.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_random.h"
#include <stddef.h>
#include "psa/crypto.h"
#include "app_state.h"
#include "storage.h"

static const char *TAG = "state";

SemaphoreHandle_t  g_state_mtx;
SemaphoreHandle_t  g_cfg_mtx;
SemaphoreHandle_t  g_recent_mtx;
EventGroupHandle_t g_sys_events;
QueueHandle_t      g_log_q;
QueueHandle_t      g_tap_q;

user_rec_t    g_users[MAX_USERS];
bool          g_inside[MAX_USERS];
time_t        g_inside_since[MAX_USERS];
int           g_user_count;
room_status_t g_rooms[ROOM_COUNT];

static sys_config_t g_cfg;
static log_event_t  s_recent[RECENT_LOG_LEN];
static int          s_recent_head, s_recent_n;
static volatile uint32_t s_dropped;   /* updated from several tasks */
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;

#define LOCK_TIMEOUT pdMS_TO_TICKS(500)

/* Demo cards loaded on first boot (later boots load from NVS). */
/* The first six UIDs are the cards built into Wokwi's MFRC522 reader,
 * so every one of them can be tapped in the simulation:
 *   Blue 01:02:03:04  Green 11:22:33:44  Yellow 55:66:77:88
 *   Red AA:BB:CC:DD (left UNREGISTERED on purpose)
 *   NFC tag 04:11:22:33:44:55:66 (7-byte UID)  Key fob C0:FF:EE:99
 * The Blue card's UID can be changed in diagram.json ("uid" attribute)
 * to any of the other members' UIDs below. */
static const user_rec_t DEFAULT_USERS[] = {
    {"01020304",       "Kuoi David",     ROLE_STUDENT,  1},   /* Wokwi Blue card   */
    {"11223344",       "Dr. Lecturer",   ROLE_STAFF,    1},   /* Wokwi Green card  */
    {"55667788",       "Security Guard", ROLE_SECURITY, 1},   /* Wokwi Yellow card */
    {"04112233445566", "Building Admin", ROLE_ADMIN,    1},   /* Wokwi NFC tag     */
    {"C0FFEE99",       "Lost Card",      ROLE_STUDENT,  0},   /* Wokwi Key fob: blocked */
    {"B2C3D4E5",       "Eria Paul",      ROLE_STUDENT,  1},   /* set Blue uid B2:C3:D4:E5 */
    {"C3D4E5F6",       "Suubi George",   ROLE_STUDENT,  1},   /* set Blue uid C3:D4:E5:F6 */
    {"D4E5F6A7",       "Mugalu Benjamin",ROLE_STUDENT,  1},   /* set Blue uid D4:E5:F6:A7 */
    {"E5F6A7B8",       "Ongol Matthew",  ROLE_STUDENT,  1},   /* set Blue uid E5:F6:A7:B8 */
};

static void hash_password(const uint8_t salt[8], const char *pw, uint8_t out[32])
{
    uint8_t buf[8 + 64];
    size_t n = strnlen(pw, 64);
    size_t hash_length = 0;

    memcpy(buf, salt, 8);
    memcpy(buf + 8, pw, n);

    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        buf,
        8 + n,
        out,
        32,
        &hash_length
    );

    if (status != PSA_SUCCESS || hash_length != 32) {
        ESP_LOGE(TAG, "Password hashing failed: %d", (int)status);
        memset(out, 0, 32);
    }

    memset(buf, 0, sizeof(buf));
}

static void cfg_defaults(sys_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic           = CONFIG_MAGIC;
    c->open_min        = DEFAULT_OPEN_HHMM;
    c->close_min       = DEFAULT_CLOSE_HHMM;
    c->pir_timeout_s   = DEFAULT_PIR_TIMEOUT_S;
    c->ldr_dark        = DEFAULT_LDR_DARK;
    c->room_watts      = DEFAULT_ROOM_WATTS;
    c->warn_before_min = DEFAULT_WARN_BEFORE_MIN;
    esp_fill_random(c->pw_salt, sizeof(c->pw_salt));
    hash_password(c->pw_salt, DEFAULT_ADMIN_PASSWORD, c->pw_hash);
}

/* Exposed for console.c (authentication) */
void state_hash_password(const uint8_t salt[8], const char *pw, uint8_t out[32])
{
    hash_password(salt, pw, out);
}

void state_init(void)
{
    /* Initialize PSA cryptography before password hashing. */
    psa_status_t status = psa_crypto_init();
    configASSERT(status == PSA_SUCCESS);

    g_state_mtx  = xSemaphoreCreateMutex();
    g_cfg_mtx    = xSemaphoreCreateMutex();
    g_recent_mtx = xSemaphoreCreateMutex();
    g_sys_events = xEventGroupCreate();
    g_log_q      = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_event_t));
    g_tap_q      = xQueueCreate(TAP_QUEUE_LEN, sizeof(tap_req_t));
    configASSERT(g_state_mtx && g_cfg_mtx && g_recent_mtx && g_sys_events && g_log_q && g_tap_q);

    /* Configuration */
    if (storage_load_config(&g_cfg) != ESP_OK || g_cfg.magic != CONFIG_MAGIC) {
        ESP_LOGW(TAG, "No saved config - using defaults (admin password: %s)", DEFAULT_ADMIN_PASSWORD);
        cfg_defaults(&g_cfg);
        storage_save_config(&g_cfg);
    }

    /* Users */
    int n = 0;
    if (storage_load_users(g_users, &n) != ESP_OK || n <= 0) {
        n = sizeof(DEFAULT_USERS) / sizeof(DEFAULT_USERS[0]);
        memcpy(g_users, DEFAULT_USERS, sizeof(DEFAULT_USERS));
        storage_save_users(g_users, n);
        ESP_LOGW(TAG, "No saved users - loaded %d demo cards", n);
    }
    g_user_count = n;
    memset(g_inside, 0, sizeof(g_inside));

    for (int i = 0; i < ROOM_COUNT; i++) {
        memset(&g_rooms[i], 0, sizeof(g_rooms[i]));
        g_rooms[i].mode = LIGHT_AUTO;
    }
}

bool state_lock(void)
{
    if (xSemaphoreTake(g_state_mtx, LOCK_TIMEOUT) == pdTRUE) return true;
    ESP_LOGE(TAG, "state lock timeout (possible deadlock) in task %s", pcTaskGetName(NULL));
    return false;
}

void state_unlock(void) { xSemaphoreGive(g_state_mtx); }

void cfg_get(sys_config_t *out)
{
    if (xSemaphoreTake(g_cfg_mtx, LOCK_TIMEOUT) == pdTRUE) {
        *out = g_cfg;
        xSemaphoreGive(g_cfg_mtx);
    } else {
        ESP_LOGE(TAG, "cfg lock timeout");
        cfg_defaults(out);
    }
}

bool cfg_update(const sys_config_t *in)
{
    sys_config_t copy;
    if (xSemaphoreTake(g_cfg_mtx, LOCK_TIMEOUT) != pdTRUE) return false;
    g_cfg = *in;
    g_cfg.magic = CONFIG_MAGIC;
    copy = g_cfg;
    xSemaphoreGive(g_cfg_mtx);
    return storage_save_config(&copy) == ESP_OK;   /* flash write outside the lock */
}

/* Copy the table while locked, write flash after unlocking (lock rule 2).
 * Only the console task edits users, so one static snapshot is enough. */
static user_rec_t s_snap[MAX_USERS];
static int        s_snap_n;
static void snapshot_users(void) { memcpy(s_snap, g_users, sizeof(s_snap)); s_snap_n = g_user_count; }
static void persist_snapshot(void) { storage_save_users(s_snap, s_snap_n); }

int users_find(const char *uid)
{
    for (int i = 0; i < g_user_count; i++)
        if (strcasecmp(g_users[i].uid, uid) == 0) return i;
    return -1;
}

bool users_add(const char *uid, const char *name, role_t role)
{
    bool ok = false;
    if (!state_lock()) return false;
    int idx = users_find(uid);
    if (idx >= 0) {                          /* update existing */
        strlcpy(g_users[idx].name, name, NAME_LEN);
        g_users[idx].role = role;
        g_users[idx].active = 1;
        ok = true;
    } else if (g_user_count < MAX_USERS) {
        idx = g_user_count++;
        strlcpy(g_users[idx].uid, uid, UID_LEN);
        strlcpy(g_users[idx].name, name, NAME_LEN);
        g_users[idx].role = role;
        g_users[idx].active = 1;
        g_inside[idx] = false;
        ok = true;
    }
    if (ok) snapshot_users();
    state_unlock();
    if (ok) persist_snapshot();
    return ok;
}

bool users_remove(const char *uid)
{
    bool ok = false;
    if (!state_lock()) return false;
    int idx = users_find(uid);
    if (idx >= 0) {
        for (int i = idx; i < g_user_count - 1; i++) {
            g_users[i] = g_users[i + 1];
            g_inside[i] = g_inside[i + 1];
            g_inside_since[i] = g_inside_since[i + 1];
        }
        g_user_count--;
        snapshot_users();
        ok = true;
    }
    state_unlock();
    if (ok) persist_snapshot();
    return ok;
}

bool users_set_active(const char *uid, bool active)
{
    bool ok = false;
    if (!state_lock()) return false;
    int idx = users_find(uid);
    if (idx >= 0) {
        g_users[idx].active = active ? 1 : 0;
        snapshot_users();
        ok = true;
    }
    state_unlock();
    if (ok) persist_snapshot();
    return ok;
}

int occupancy_count(void)
{
    int n = 0;
    if (!state_lock()) return -1;
    for (int i = 0; i < g_user_count; i++) if (g_inside[i]) n++;
    state_unlock();
    return n;
}

void log_event(event_type_t type, char dir, const char *uid, const char *name,
               const char *fmt, ...)
{
    log_event_t ev = {0};
    ev.ts = time(NULL);
    ev.type = type;
    ev.dir = dir ? dir : '-';
    strlcpy(ev.uid, uid ? uid : "-", UID_LEN);
    strlcpy(ev.name, name ? name : "-", NAME_LEN);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.msg, sizeof(ev.msg), fmt, ap);
    va_end(ap);

    /* Producer side of the logging pipeline: 0 wait so a door never stalls. */
    if (xQueueSend(g_log_q, &ev, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_drop_mux);
        s_dropped++;
        taskEXIT_CRITICAL(&s_drop_mux);
    }
}

uint32_t log_dropped_count(void) { return s_dropped; }

void recent_push(const log_event_t *ev)
{
    if (xSemaphoreTake(g_recent_mtx, LOCK_TIMEOUT) != pdTRUE) return;
    s_recent[s_recent_head] = *ev;
    s_recent_head = (s_recent_head + 1) % RECENT_LOG_LEN;
    if (s_recent_n < RECENT_LOG_LEN) s_recent_n++;
    xSemaphoreGive(g_recent_mtx);
}

int recent_copy(log_event_t *out, int max)
{
    if (xSemaphoreTake(g_recent_mtx, LOCK_TIMEOUT) != pdTRUE) return 0;
    int n = s_recent_n < max ? s_recent_n : max;
    int start = (s_recent_head - n + RECENT_LOG_LEN) % RECENT_LOG_LEN;
    for (int i = 0; i < n; i++) out[i] = s_recent[(start + i) % RECENT_LOG_LEN];
    xSemaphoreGive(g_recent_mtx);
    return n;
}

const char *role_name(uint8_t role)
{
    switch (role) {
    case ROLE_STUDENT:  return "STUDENT";
    case ROLE_STAFF:    return "STAFF";
    case ROLE_SECURITY: return "SECURITY";
    case ROLE_ADMIN:    return "ADMIN";
    default:            return "?";
    }
}

const char *event_type_name(uint8_t type)
{
    static const char *names[] = {"GRANTED", "DENIED", "ROOM", "ALERT", "CONFIG", "SYSTEM"};
    return type < 6 ? names[type] : "?";
}

void fmt_time(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
