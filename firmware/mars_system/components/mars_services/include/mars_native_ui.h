#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional versioned drawing interface. Kernel 2.5.21+, same package on new
 * rectangular kernels falls back to legacy gfx. No driver/task handles escape. */
typedef struct {
 uint32_t version,width,height,round;
 int (*begin)(int logical_width,int logical_height);
 void (*clear)(uint16_t);
 void (*rect)(int,int,int,int,uint16_t);
 void (*text)(int,int,const char*,int,uint16_t);
 int (*text_width)(const char*,int);
 void (*bitmap)(int,int,int,int,int,const uint16_t*);
 void (*legacy_layer)(void);
 void (*present)(void);
} mars_native_ui_v1_t;
const mars_native_ui_v1_t *mars_native_ui_get(uint32_t version);
/* Kernel registration during launcher startup, not an app export. */
void mars_native_ui_register(const mars_native_ui_v1_t *);
#ifdef __cplusplus
}
#endif
