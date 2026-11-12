/*
 * access.h - Terminal A: main entrance (RFID entry/exit, doors, occupancy)
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>

#include <stdint.h>

void access_task(void *arg);
/* Polls the ENTRY and EXIT MFRC522 readers and queues taps. */
void rfid_task(void *arg);
/* true if that reader (0 = entry, 1 = exit) is answering; optional chip version */
bool access_reader_status(int door, uint8_t *version);
/* Text for LCD line 2 on the idle screen ("Tap your card" or a reader fault). */
void access_get_card_label(char *buf, size_t len);
/* Queue a tap from the serial console: dir 'I' or 'O'. */
bool access_queue_tap(const char *uid, char dir);
