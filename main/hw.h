/*
 * hw.h - Low-level hardware helpers (I2C bus, servos, buzzer, ADC, GPIO)
 */
#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef enum { DOOR_ENTRY = 0, DOOR_EXIT = 1 } door_t;

void hw_init(void);
i2c_master_bus_handle_t hw_i2c_bus(void);

void hw_door_set(door_t door, bool open);      /* servo: locked / open */
void hw_beep(int ms, int times);               /* blocking, short */
void hw_tone(bool on);

bool hw_pir(int room);                         /* room = 0 or 1 */
void hw_light(int room, bool on);
int  hw_ldr_raw(int room);                     /* thread-safe ADC read */
bool hw_button_pressed(int gpio);              /* raw level (active low) */
