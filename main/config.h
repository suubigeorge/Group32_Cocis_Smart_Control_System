/*
 * config.h - Pin map and system-wide constants
 * CoCIS Smart Control and Monitoring System (ESP32 / ESP-IDF / Wokwi)
 *
 * One ESP32 hosts three logical "terminals" as separate FreeRTOS tasks:
 *   Terminal A - Main entrance (RFID entry reader + exit reader, two doors)
 *   Terminal B - Lecture Room 1 (PIR + LDR + lights)
 *   Terminal C - Lecture Room 2 (PIR + LDR + lights)
 */
#pragma once

/* ---------- Shared I2C bus: LCD 16x2 (0x27) + DS1307 RTC (0x68) ---------- */
#define PIN_I2C_SDA          21
#define PIN_I2C_SCL          22
#define LCD_I2C_ADDR         0x27
#define RTC_I2C_ADDR         0x68

/* ---------- SD card (SPI) ---------- */
#define PIN_SD_CS            5
#define PIN_SD_SCK           18
#define PIN_SD_MOSI          23
#define PIN_SD_MISO          19
#define SD_MOUNT_POINT       "/sdcard"

/* ---------- Terminal A: main entrance ---------- */
#define PIN_SERVO_ENTRY      13   /* entry door lock/turnstile */
#define PIN_SERVO_EXIT       14   /* exit door lock/turnstile  */
#define PIN_BTN_ENTRY_TAP    32   /* "tap card on ENTRY reader" */
#define PIN_BTN_EXIT_TAP     33   /* "tap card on EXIT reader"  */
#define PIN_BTN_CARD_SELECT  25   /* pick which demo card is in hand */
#define PIN_BTN_EMERGENCY    26   /* fire / emergency release (toggle) */
#define PIN_BUZZER           15

/* ---------- Terminals B & C: lecture rooms ---------- */
#define PIN_PIR_ROOM1        27
#define PIN_PIR_ROOM2        4
#define PIN_LIGHT_ROOM1      16
#define PIN_LIGHT_ROOM2      17
#define PIN_LDR_ROOM1        34   /* ADC1_CH6 */
#define PIN_LDR_ROOM2        35   /* ADC1_CH7 */

/* ---------- Behaviour defaults (all changeable from the admin menu) ---------- */
#define DEFAULT_OPEN_HHMM        (7 * 60)        /* 07:00 */
#define DEFAULT_CLOSE_HHMM       (22 * 60)       /* 22:00 */
#define DEFAULT_PIR_TIMEOUT_S    30              /* room empty after 30 s of no motion */
#define DEFAULT_LDR_DARK         2000            /* ADC raw above this = dark */
#define DEFAULT_ROOM_WATTS       400             /* lighting load per room, for energy stats */
#define DEFAULT_WARN_BEFORE_MIN  15              /* closing warning */
#define DEFAULT_ADMIN_PASSWORD   "admin123"

#define DOOR_OPEN_MS             3000
#define SERVO_LOCKED_DEG         0
#define SERVO_OPEN_DEG           90

#define MAX_USERS                32
#define UID_LEN                  12   /* 8 hex chars + NUL, with headroom */
#define NAME_LEN                 24
#define ROOM_COUNT               2

#define LOG_QUEUE_LEN            32
#define TAP_QUEUE_LEN            8
#define RECENT_LOG_LEN           40   /* events kept in RAM for quick viewing */

#define LOGIN_MAX_ATTEMPTS       3
#define LOGIN_LOCKOUT_S          60
#define SESSION_TIMEOUT_S        300
#define UNKNOWN_CARD_ALARM_COUNT 3    /* this many unknown cards ... */
#define UNKNOWN_CARD_WINDOW_S    60   /* ... within this window raises an alarm */
