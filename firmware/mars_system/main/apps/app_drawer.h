#pragma once
#include <stdbool.h>
#include <stdint.h>

void app_drawer_open(void);
bool app_drawer_is_open(void);
void app_drawer_close(void);
void app_drawer_tick(int64_t now_ms);
void app_drawer_touch(bool down, uint16_t x, uint16_t y);
/* Consumed after fade-out; -1 means close without launching an app. */
int app_drawer_take_launch(void);
