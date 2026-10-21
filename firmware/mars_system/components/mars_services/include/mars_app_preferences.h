#pragma once
#include <stdint.h>
/* App-local small settings. namespace must start with app_, max 15 chars;
 * key: ASCII letters/digits/underscore, max 15 chars. No service credentials.
 * Returns 0, -2 missing, -22 invalid, -5 storage/commit failure. */
/* System startup only; creates one internal-RAM NVS worker. Not app ABI. */
int mars_app_preferences_init(void);
int mars_app_pref_get_u32(const char *app_namespace, const char *key, uint32_t *value);
int mars_app_pref_set_u32(const char *app_namespace, const char *key, uint32_t value);
