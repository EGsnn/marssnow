#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PARTITION_COUNT 3
#define APP_SLOT_COUNT 10
#define APP_NAME_MAX 32

typedef struct {
    const char *label;      /* containing app_0 / app_1 / app_2 partition */
    uint32_t offset;        /* absolute package flash offset */
    uint32_t package_base;  /* package offset within containing partition */
    uint32_t package_size;  /* complete MARSAPP byte length */
    uint32_t total_size;    /* containing partition size */
    uint32_t sequence;      /* 0 for legacy package; larger for appended updates */
    bool     occupied;      /* partition is not blank */
    uint32_t used_bytes;    /* bytes up to the last non-0xFF byte */
    bool     is_valid_app;  /* contains a valid MARSAPP package */
    uint16_t app_version;   /* application version */
    uint32_t app_code_size; /* code segment size */
    uint32_t app_asset_size; /* asset segment size */
    char     app_name[APP_NAME_MAX]; /* application name (if valid) */
    char     display_name[64]; /* optional UTF-8 name from appmeta.bin */
    uint32_t icon_offset; /* 48x48 RGB565LE icon offset within partition */
} app_slot_info_t;

/** Scan legacy packages and committed records in app_0..app_2. */
void app_slots_scan(void);

/** Number of non-empty slots (call after app_slots_scan). */
int app_slots_installed_count(void);

/** Number of valid app packages (call after app_slots_scan). */
int app_slots_valid_count(void);

/** Get info for slot index 0..APP_SLOT_COUNT-1. Returns false if index invalid. */
bool app_slots_get_info(int index, app_slot_info_t *out);

#ifdef __cplusplus
}
#endif
