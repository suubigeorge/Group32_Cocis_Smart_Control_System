/*
 * rc522.h - MFRC522 (RC522) RFID reader driver for ESP-IDF (SPI)
 *
 * Supports several readers on one SPI bus (each has its own CS pin).
 * Reads the UID of ISO14443A cards: 4-byte (MIFARE Classic / Mini)
 * and 7-byte (MIFARE Ultralight / NFC tags).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/spi_master.h"

#define RC522_MAX_UID 10

typedef struct {
    spi_device_handle_t spi;
    int     cs_pin;
    uint8_t version;      /* VersionReg value read at start-up (0x91/0x92 = genuine) */
    bool    ok;           /* reader answered at start-up / last health check */
} rc522_t;

/* Hard-reset all readers sharing rst_pin (call once, before rc522_init). */
void rc522_hard_reset(int rst_pin);

/* Add a reader on an already-initialised SPI bus and configure it. */
esp_err_t rc522_init(rc522_t *r, spi_host_device_t host, int cs_pin);

/* Soft-reset and set up the chip again (e.g. after it was unplugged). */
esp_err_t rc522_configure(rc522_t *r);

/* Non-blocking poll: true + UID if a NEW card is on the reader.
 * The card is put to HALT after reading, so holding it on the reader
 * produces one read, not a stream of reads. */
bool rc522_read_uid(rc522_t *r, uint8_t *uid, uint8_t *uid_len);

/* Health check: true if the chip still answers (used to detect a
 * disconnected or tampered reader). */
bool rc522_alive(rc522_t *r);

/* "01020304" style string, upper-case, no separators. */
void rc522_uid_to_str(const uint8_t *uid, uint8_t len, char *out, size_t out_len);
