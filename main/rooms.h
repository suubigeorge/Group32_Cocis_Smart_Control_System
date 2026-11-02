/*
 * rooms.h - Terminals B and C: lecture room occupancy + automatic lighting
 */
#pragma once
#include "app_state.h"

/* One task per room; pass the room index (0 or 1) as the task argument. */
void room_task(void *arg);
bool rooms_set_mode(int room, light_mode_t mode);
