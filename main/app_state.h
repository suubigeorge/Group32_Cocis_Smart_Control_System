/*
 * app_state.h - Shared data, synchronisation objects and inter-terminal messages
 *
 * Every piece of data that more than one task touches lives here, and every
 * access goes through a mutex. Tasks talk to each other only through
 * FreeRTOS queues and an event group, never through unprotected globals.
 *
 * Lock rules (prevents deadlock):
 *   1. Never hold two of these mutexes at the same time.
 *   2. Copy what you need, release the lock, THEN print / write files / sleep.
 *   3. Always take a lock with a timeout and report if it fails.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "config.h"

/* ---------------- Users / RFID cards ---------------- */
typedef enum { ROLE_STUDENT = 0, ROLE_STAFF, ROLE_SECURITY, ROLE_ADMIN } role_t;

typedef struct {
    char    uid[UID_LEN];
    char    name[NAME_LEN];
    uint8_t role;      /* role_t */
    uint8_t active;    /* 0 = card blocked */
} user_rec_t;          /* persisted in NVS */

/* ---------------- Configuration ---------------- */
#define CONFIG_MAGIC 0xC0C15002u
typedef struct {
    uint32_t magic;
    uint16_t open_min;        /* minutes after midnight */
    uint16_t close_min;
    uint16_t pir_timeout_s;
    uint16_t ldr_dark;
    uint16_t room_watts;
    uint8_t  warn_before_min;
    uint8_t  pw_salt[8];
    uint8_t  pw_hash[32];     /* SHA-256(salt || password) */
} sys_config_t;               /* persisted in NVS */

/* ---------------- Rooms ---------------- */
typedef enum { LIGHT_AUTO = 0, LIGHT_FORCE_ON, LIGHT_FORCE_OFF } light_mode_t;

typedef struct {
    bool     motion;          /* PIR output right now */
    bool     occupied;        /* motion seen within pir_timeout_s */
    bool     dark;            /* LDR says the room needs light */
    bool     light_on;
    uint8_t  mode;            /* light_mode_t */
    int      ldr_raw;
    time_t   last_motion;
    uint32_t lit_s;           /* seconds lights were on since boot */
    uint32_t occupied_s;      /* seconds room was occupied since boot */
    uint32_t saved_s;         /* seconds lights stayed OFF in an empty room during open hours */
} room_status_t;

/* ---------------- Events / logging ---------------- */
typedef enum {
    EV_GRANTED = 0, EV_DENIED, EV_ROOM, EV_ALERT, EV_CONFIG, EV_SYSTEM
} event_type_t;

typedef struct {
    time_t  ts;
    uint8_t type;             /* event_type_t */
    char    dir;              /* 'I' entry, 'O' exit, '-' n/a */
    char    uid[UID_LEN];
    char    name[NAME_LEN];
    char    msg[48];
} log_event_t;

/* ---------------- Card taps (entrance terminal input) ---------------- */
typedef enum { TAP_SRC_READER = 0, TAP_SRC_SERIAL } tap_src_t;
typedef struct {
    char    uid[UID_LEN];
    char    dir;              /* 'I' or 'O' */
    uint8_t source;
} tap_req_t;

/* ---------------- System-wide flags (event group) ---------------- */
#define EVT_BUILDING_OPEN  BIT0
#define EVT_EMERGENCY      BIT1
#define EVT_SD_OK          BIT2
#define EVT_CLOSING_SOON   BIT3
#define EVT_LOCKDOWN       BIT4   /* admin lockdown: all doors refuse normal cards */

/* ---------------- Synchronisation objects ---------------- */
extern SemaphoreHandle_t  g_state_mtx;  /* users[], presence[], rooms[] */
extern SemaphoreHandle_t  g_cfg_mtx;    /* g_cfg */
extern SemaphoreHandle_t  g_recent_mtx; /* recent-event ring buffer */
extern EventGroupHandle_t g_sys_events;
extern QueueHandle_t      g_log_q;      /* log_event_t  -> logger task */
extern QueueHandle_t      g_tap_q;      /* tap_req_t    -> entrance task */

/* ---------------- Shared data (only touch while holding the right mutex) ---------------- */
extern user_rec_t    g_users[MAX_USERS];
extern bool          g_inside[MAX_USERS];
extern time_t        g_inside_since[MAX_USERS];
extern int           g_user_count;
extern room_status_t g_rooms[ROOM_COUNT];

/* ---------------- API ---------------- */
void state_init(void);                      /* create mutexes/queues, load NVS data */
bool state_lock(void);                      /* g_state_mtx with timeout */
void state_unlock(void);

void cfg_get(sys_config_t *out);            /* thread-safe copy */
bool cfg_update(const sys_config_t *in);    /* thread-safe set + persist */

int  users_find(const char *uid);           /* caller must hold g_state_mtx */
bool users_add(const char *uid, const char *name, role_t role);
bool users_remove(const char *uid);
bool users_set_active(const char *uid, bool active);
int  occupancy_count(void);                 /* takes the lock itself */

/* Non-blocking: never stalls a door or a room because the SD card is slow. */
void log_event(event_type_t type, char dir, const char *uid, const char *name,
               const char *fmt, ...) __attribute__((format(printf, 5, 6)));
uint32_t log_dropped_count(void);

/* RAM ring buffer of recent events (filled by the logger task) */
void recent_push(const log_event_t *ev);
int  recent_copy(log_event_t *out, int max); /* newest last */

const char *role_name(uint8_t role);
const char *event_type_name(uint8_t type);
void fmt_time(time_t t, char *buf, size_t len); /* "YYYY-MM-DD HH:MM:SS" */
