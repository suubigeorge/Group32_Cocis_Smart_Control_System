/*
 * access.h - Terminal A: main entrance (RFID entry/exit, doors, occupancy)
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>

void access_task(void *arg);
/* Text for LCD line 2: which demo card is "in hand" (thread-safe copy). */
void access_get_card_label(char *buf, size_t len);
/* Queue a tap from the serial console: dir 'I' or 'O'. */
bool access_queue_tap(const char *uid, char dir);
