#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mars_board_config.h"
#if MARS_BOARD_DISPLAY_ROUND
bool round_ui_init(void);
bool round_ui_active(void);
bool round_ui_busy(void);
void round_ui_poll(int64_t now_ms);
const char *round_ui_take_action(void);
bool round_ui_action(const char *action);
void round_ui_suspend(void);
void round_ui_app_enter(void);
bool round_ui_app_native(void);
bool round_ui_app_map_touch(uint16_t,uint16_t,uint16_t*,uint16_t*);
void round_ui_app_fallback(void);
void round_ui_resume(void);
#else
static inline bool round_ui_init(void){return false;}
static inline bool round_ui_active(void){return false;}
static inline bool round_ui_busy(void){return false;}
static inline void round_ui_poll(int64_t now){(void)now;}
static inline const char *round_ui_take_action(void){return 0;}
static inline bool round_ui_action(const char*s){(void)s;return false;}
static inline void round_ui_suspend(void){}
static inline void round_ui_app_enter(void){}
static inline bool round_ui_app_native(void){return false;}
static inline bool round_ui_app_map_touch(uint16_t x,uint16_t y,uint16_t*a,uint16_t*b){(void)x;(void)y;(void)a;(void)b;return false;}
static inline void round_ui_app_fallback(void){}
static inline void round_ui_resume(void){}
#endif
