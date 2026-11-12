/*
 * rc522.c - MFRC522 RFID reader driver (ESP-IDF spi_master).
 * Owner: Access control.
 *
 * How a card is read (ISO/IEC 14443-3 type A):
 *   1. REQA   - "is any idle card there?"  card answers ATQA (2 bytes)
 *   2. ANTICOLLISION (per cascade level) - card sends 4 UID bytes + BCC check byte
 *   3. SELECT - choose that card; SAK byte says if more UID bytes follow
 *      (7-byte UIDs need a second cascade level)
 *   4. HLTA   - put the card to sleep so it is read once per presentation
 *
 * The RC522 does the radio work; we talk to it over SPI by reading and
 * writing its registers and its 64-byte FIFO.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "rc522.h"

static const char *TAG = "rc522";

/* ---- MFRC522 registers ---- */
#define CommandReg      0x01
#define ComIEnReg       0x02
#define ComIrqReg       0x04
#define DivIrqReg       0x05
#define ErrorReg        0x06
#define FIFODataReg     0x09
#define FIFOLevelReg    0x0A
#define ControlReg      0x0C
#define BitFramingReg   0x0D
#define CollReg         0x0E
#define ModeReg         0x11
#define TxModeReg       0x12
#define RxModeReg       0x13
#define TxControlReg    0x14
#define TxASKReg        0x15
#define CRCResultRegH   0x21
#define CRCResultRegL   0x22
#define ModWidthReg     0x24
#define TModeReg        0x2A
#define TPrescalerReg   0x2B
#define TReloadRegH     0x2C
#define TReloadRegL     0x2D
#define VersionReg      0x37

/* ---- MFRC522 commands ---- */
#define PCD_Idle        0x00
#define PCD_CalcCRC     0x03
#define PCD_Transceive  0x0C
#define PCD_SoftReset   0x0F

/* ---- card (PICC) commands ---- */
#define PICC_REQA       0x26
#define PICC_SEL_CL1    0x93
#define PICC_SEL_CL2    0x95
#define PICC_SEL_CL3    0x97
#define PICC_HLTA       0x50
#define PICC_CASCADE_TAG 0x88

/* ---------------- register access over SPI ---------------- */
static void wr(rc522_t *r, uint8_t reg, uint8_t val)
{
    spi_transaction_t t = {
        .length = 16,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {(uint8_t)((reg << 1) & 0x7E), val},
    };
    spi_device_polling_transmit(r->spi, &t);
}

static uint8_t rd(rc522_t *r, uint8_t reg)
{
    spi_transaction_t t = {
        .length = 16,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {(uint8_t)(((reg << 1) & 0x7E) | 0x80), 0},
    };
    spi_device_polling_transmit(r->spi, &t);
    return t.rx_data[1];
}

/* Write several bytes to one register in ONE chip-select (like the Arduino
 * MFRC522 library does for the FIFO). */
static void wr_burst(rc522_t *r, uint8_t reg, const uint8_t *data, int n)
{
    uint8_t buf[1 + 32];
    if (n > 32) n = 32;            /* FIFO frames here are at most 9 bytes */
    buf[0] = (uint8_t)((reg << 1) & 0x7E);
    memcpy(buf + 1, data, n);
    spi_transaction_t t = {.length = 8 * (1 + n), .tx_buffer = buf};
    spi_device_polling_transmit(r->spi, &t);
}

/* Read n bytes from one register in ONE chip-select. */
static void rd_burst(rc522_t *r, uint8_t reg, uint8_t *out, int n)
{
    uint8_t tx[1 + 32], rx[1 + 32];
    if (n > 32) n = 32;
    memset(tx, ((reg << 1) & 0x7E) | 0x80, n);
    tx[n] = 0;
    spi_transaction_t t = {.length = 8 * (1 + n), .tx_buffer = tx, .rx_buffer = rx};
    spi_device_polling_transmit(r->spi, &t);
    memcpy(out, rx + 1, n);
}

static void set_bits(rc522_t *r, uint8_t reg, uint8_t mask) { wr(r, reg, rd(r, reg) | mask); }
static void clr_bits(rc522_t *r, uint8_t reg, uint8_t mask) { wr(r, reg, rd(r, reg) & ~mask); }

/* ---------------- CRC_A calculated by the RC522 ---------------- */
static bool calc_crc(rc522_t *r, const uint8_t *data, int len, uint8_t out[2])
{
    wr(r, CommandReg, PCD_Idle);
    wr(r, DivIrqReg, 0x04);            /* clear CRCIRq */
    wr(r, FIFOLevelReg, 0x80);         /* flush FIFO */
    wr_burst(r, FIFODataReg, data, len);
    wr(r, CommandReg, PCD_CalcCRC);
    int64_t deadline = esp_timer_get_time() + 20000;
    while (esp_timer_get_time() < deadline) {
        if (rd(r, DivIrqReg) & 0x04) {
            wr(r, CommandReg, PCD_Idle);
            out[0] = rd(r, CRCResultRegL);
            out[1] = rd(r, CRCResultRegH);
            return true;
        }
    }
    return false;
}

/* ---------------- send to card, receive its answer ---------------- */
/* Last transceive result, for the "[rfid]" trace lines */
static uint8_t s_last_irq, s_last_err;
static int     s_last_n;

static bool transceive(rc522_t *r, const uint8_t *tx, int tx_len, uint8_t *rx, int *rx_len,
                       uint8_t tx_last_bits)
{
    s_last_irq = s_last_err = 0;
    s_last_n = 0;
    wr(r, CommandReg, PCD_Idle);
    wr(r, ComIrqReg, 0x7F);            /* clear all interrupt flags */
    wr(r, FIFOLevelReg, 0x80);         /* flush FIFO */
    wr_burst(r, FIFODataReg, tx, tx_len);
    wr(r, BitFramingReg, tx_last_bits);
    wr(r, CommandReg, PCD_Transceive);
    set_bits(r, BitFramingReg, 0x80);  /* StartSend */

    /* A card answers within about 1 ms; give up after ~10 ms */
    bool done = false;
    int64_t deadline = esp_timer_get_time() + 10000;
    uint8_t irq = 0;
    for (;;) {
        irq = rd(r, ComIrqReg);
        if (irq & 0x30) { done = true; break; }        /* RxIRq or IdleIRq */
        if (irq & 0x01) break;                          /* TimerIRq: no card answered */
        if (esp_timer_get_time() > deadline) break;
        vTaskDelay(1);                                  /* 1 ms: let other tasks run */
    }
    clr_bits(r, BitFramingReg, 0x80);
    s_last_irq = irq;
    if (!done) return false;

    uint8_t err = rd(r, ErrorReg);
    int n = rd(r, FIFOLevelReg);
    s_last_err = err;
    s_last_n = n;
    if (err & 0x13) return false;                       /* buffer overflow, parity, protocol */
    if (err & 0x08) return false;                       /* collision: two cards at once */

    if (rx && rx_len) {
        if (n > *rx_len) n = *rx_len;
        if (n > 0) rd_burst(r, FIFODataReg, rx, n);
        *rx_len = n;
    }
    return true;
}

/* ---------------- public API ---------------- */
void rc522_hard_reset(int rst_pin)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << rst_pin, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    gpio_set_level(rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));     /* oscillator start-up */
}

esp_err_t rc522_init(rc522_t *r, spi_host_device_t host, int cs_pin)
{
    memset(r, 0, sizeof(*r));
    r->cs_pin = cs_pin;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,   /* RC522 supports up to 10 MHz */
        .mode = 0,
        .spics_io_num = cs_pin,
        .queue_size = 2,
    };
    esp_err_t err = spi_bus_add_device(host, &dev, &r->spi);
    if (err != ESP_OK) { r->spi = NULL; return err; }
    return rc522_configure(r);
}

esp_err_t rc522_configure(rc522_t *r)
{
    if (!r->spi) return ESP_ERR_INVALID_STATE;
    wr(r, CommandReg, PCD_SoftReset);
    vTaskDelay(pdMS_TO_TICKS(50));

    wr(r, TxModeReg, 0x00);
    wr(r, RxModeReg, 0x00);
    wr(r, ModWidthReg, 0x26);
    wr(r, TModeReg, 0x80);             /* timer starts automatically after sending */
    wr(r, TPrescalerReg, 0xA9);        /* 40 kHz timer tick */
    wr(r, TReloadRegH, 0x00);          /* 200 ticks = 5 ms "no card" timeout */
    wr(r, TReloadRegL, 0xC8);
    wr(r, TxASKReg, 0x40);             /* 100% ASK modulation */
    wr(r, ModeReg, 0x3D);              /* CRC preset 0x6363 */
    set_bits(r, TxControlReg, 0x03);   /* antenna on */

    r->version = rd(r, VersionReg);
    r->ok = r->version != 0x00 && r->version != 0xFF;
    ESP_LOGI(TAG, "reader on CS %d: version 0x%02X %s", r->cs_pin, r->version,
             r->ok ? "(OK)" : "(NOT RESPONDING)");
    return r->ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool rc522_alive(rc522_t *r)
{
    if (!r->spi) return false;
    uint8_t v = rd(r, VersionReg);
    r->ok = v != 0x00 && v != 0xFF;
    return r->ok;
}

static void halt_card(rc522_t *r)
{
    uint8_t buf[4] = {PICC_HLTA, 0x00};
    if (!calc_crc(r, buf, 2, &buf[2])) return;
    transceive(r, buf, 4, NULL, NULL, 0);   /* a halted card does not answer: timeout is normal */
}

bool rc522_read_uid(rc522_t *r, uint8_t *uid, uint8_t *uid_len)
{
    if (!r->spi) return false;

    /* 1. REQA (7-bit short frame) */
    uint8_t cmd = PICC_REQA, atqa[2];
    int n = sizeof(atqa);
    clr_bits(r, CollReg, 0x80);
    if (!transceive(r, &cmd, 1, atqa, &n, 0x07) || n != 2) {
        if (s_last_n || s_last_err)    /* something answered but not cleanly */
            printf("[rfid] CS%d REQA: irq=%02X err=%02X fifo=%d\n", r->cs_pin, s_last_irq, s_last_err, s_last_n);
        return false;
    }
    printf("[rfid] CS%d card detected (ATQA %02X %02X)\n", r->cs_pin, atqa[0], atqa[1]);

    /* 2+3. anticollision + select, one cascade level at a time */
    static const uint8_t sel_cmd[3] = {PICC_SEL_CL1, PICC_SEL_CL2, PICC_SEL_CL3};
    uint8_t len = 0;
    for (int level = 0; level < 3; level++) {
        uint8_t ac[2] = {sel_cmd[level], 0x20}, resp[5];
        n = sizeof(resp);
        if (!transceive(r, ac, 2, resp, &n, 0) || n != 5) {
            printf("[rfid] CS%d anticollision failed: irq=%02X err=%02X fifo=%d\n", r->cs_pin, s_last_irq, s_last_err, s_last_n);
            return false;
        }
        if ((resp[0] ^ resp[1] ^ resp[2] ^ resp[3]) != resp[4]) {   /* BCC check */
            printf("[rfid] CS%d bad BCC\n", r->cs_pin);
            return false;
        }

        uint8_t sel[9] = {sel_cmd[level], 0x70, resp[0], resp[1], resp[2], resp[3], resp[4]};
        if (!calc_crc(r, sel, 7, &sel[7])) {
            printf("[rfid] CS%d CRC unit timeout\n", r->cs_pin);
            return false;
        }
        uint8_t sak[3];
        n = sizeof(sak);
        if (!transceive(r, sel, 9, sak, &n, 0) || n < 1) {
            printf("[rfid] CS%d select failed: irq=%02X err=%02X fifo=%d\n", r->cs_pin, s_last_irq, s_last_err, s_last_n);
            return false;
        }

        if (resp[0] == PICC_CASCADE_TAG) {          /* more UID bytes follow */
            memcpy(uid + len, resp + 1, 3);
            len += 3;
            if (!(sak[0] & 0x04)) return false;
        } else {
            memcpy(uid + len, resp, 4);
            len += 4;
            break;
        }
    }
    *uid_len = len;

    /* 4. HLTA: one read per presentation */
    halt_card(r);
    return true;
}

void rc522_uid_to_str(const uint8_t *uid, uint8_t len, char *out, size_t out_len)
{
    size_t p = 0;
    out[0] = 0;
    for (int i = 0; i < len && p + 2 < out_len; i++) p += snprintf(out + p, out_len - p, "%02X", uid[i]);
}
