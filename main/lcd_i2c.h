/*
 * lcd_i2c.h - 16x2 LCD over a PCF8574 I2C backpack, safe to call from any task
 */
#pragma once
#include <stdbool.h>

void lcd_init(void);
/* Show a message for hold_ms; idle screens won't overwrite it until then. */
void lcd_message(const char *line1, const char *line2, int hold_ms);
/* Idle screen update - ignored while a message is being held. */
void lcd_idle(const char *line1, const char *line2);
bool lcd_ok(void);
