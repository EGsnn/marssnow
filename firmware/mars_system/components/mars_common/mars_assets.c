#include "mars_assets.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#define WALLPAPER_NAME "mars_landscape.rgb565.bin"
#define WALLPAPER_BYTES (320u * 240u * sizeof(uint16_t))
/* mars_assets_v2.bin package ABI: fixed 32-byte name + 12-byte metadata. */
#define MARS_ASSET_NAME_LENGTH 32u

static const char *TAG = "mars_assets";
static bool s_checked;
static const uint16_t *s_landscape;
static esp_partition_mmap_handle_t s_landscape_map;

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

const uint16_t *mars_assets_landscape(void)
{
    if (s_checked) return s_landscape;
    s_checked = true;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (!partition) return NULL;

    uint8_t header[12];
    if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK) return NULL;
    const uint32_t count = read_u32(header);
    const uint32_t stored_len = read_u32(header + 8);
    const uint32_t entry_len = MARS_ASSET_NAME_LENGTH + 12;
    if (count == 0 || count > 128 ||
        (uint64_t)count * entry_len > stored_len ||
        stored_len > partition->size - sizeof(header)) return NULL;

    const uint32_t data_start = sizeof(header) + count * entry_len;
    const uint32_t data_len = stored_len - count * entry_len;
    uint8_t entry[MARS_ASSET_NAME_LENGTH + 12];
    for (uint32_t i = 0; i < count; ++i) {
        if (esp_partition_read(partition, sizeof(header) + i * entry_len,
                               entry, sizeof(entry)) != ESP_OK) return NULL;
        if (strncmp((const char *)entry, WALLPAPER_NAME,
                    MARS_ASSET_NAME_LENGTH) != 0) continue;
        const uint32_t size = read_u32(entry + MARS_ASSET_NAME_LENGTH);
        const uint32_t offset = read_u32(entry + MARS_ASSET_NAME_LENGTH + 4);
        if (size != WALLPAPER_BYTES || offset > data_len ||
            data_len - offset < WALLPAPER_BYTES + 2) return NULL;
        const uint32_t file_offset = data_start + offset;
        uint8_t marker[2];
        if (esp_partition_read(partition, file_offset, marker, sizeof(marker)) != ESP_OK ||
            marker[0] != 0x5a || marker[1] != 0x5a) return NULL;
        const void *mapped = NULL;
        if (esp_partition_mmap(partition, file_offset + sizeof(marker), WALLPAPER_BYTES,
                               ESP_PARTITION_MMAP_DATA, &mapped, &s_landscape_map) != ESP_OK) {
            ESP_LOGW(TAG, "landscape mmap failed");
            return NULL;
        }
        s_landscape = mapped;
        ESP_LOGI(TAG, "mapped shared landscape from assets partition");
        return s_landscape;
    }
    ESP_LOGW(TAG, "landscape missing from assets partition");
    return NULL;
}
