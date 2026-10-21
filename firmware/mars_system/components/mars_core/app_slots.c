#include "app_slots.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "app_slots";

/* Executable MARSAPP v2 header (must match pack_marsapp.py and loader). */
#define MARSAPP_MAGIC "MARSAPP"
#define MARSAPP_MAGIC_LEN 8
#define MARSAPP_VERSION 2
#define MARSAPP_HEADER_SIZE 96
#define MARSAPP_FLAG_RESOURCES (1u << 0)
#define MARSRES_MAGIC "MARSRES"
#define MARSRES_HEADER_SIZE 32
#define APP_SECTOR_SIZE 4096u
#define APP_RECORD_MAGIC "MARSREC"

typedef struct __attribute__((packed)) {
    char     magic[MARSAPP_MAGIC_LEN];
    uint16_t version;
    uint16_t flags;
    uint32_t elf_offset;
    uint32_t elf_size;
    uint32_t stack_size;
    uint32_t heap_size;
    char     app_name[32];
    char     entry_symbol[32];
    uint32_t crc32;
} marsapp_header_t;

typedef struct __attribute__((packed)) {
    char     magic[8];
    uint16_t version;
    uint16_t entry_size;
    uint32_t file_count;
    uint32_t table_size;
    uint32_t data_size;
    uint32_t total_size;
    uint32_t crc32;
} marsres_header_t;

typedef struct __attribute__((packed)) {
    char name[32];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint32_t flags;
} marsres_entry_t;

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t name_size;
    uint16_t width;
    uint16_t height;
    char display_name[64];
} appmeta_t;

/* The record header is written last, after its package was written and verified.
 * A missing header leaves the previous installed app intact after power loss. */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t logical_slot;
    uint32_t sequence;
    uint32_t package_size;
    uint32_t package_crc32;
    uint32_t header_crc32;
} app_record_t;

static uint32_t align_sector(uint32_t value)
{
    return (value + APP_SECTOR_SIZE - 1u) & ~(APP_SECTOR_SIZE - 1u);
}

static uint32_t crc32_step(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0);
    }
    return crc;
}

static void read_app_metadata(const esp_partition_t *p, uint32_t archive_offset,
                              const marsres_header_t *resources, app_slot_info_t *info)
{
    for (uint32_t i = 0; i < resources->file_count; ++i) {
        marsres_entry_t entry;
        uint32_t table_offset = archive_offset + MARSRES_HEADER_SIZE + i * sizeof(entry);
        if (esp_partition_read(p, table_offset, &entry, sizeof(entry)) != ESP_OK) return;
        if (memcmp(entry.name, "appmeta.bin", 12) != 0 || entry.name[11] != '\0') continue;
        if (entry.flags != 0 || entry.size != sizeof(appmeta_t) + 48u * 48u * 2u ||
            entry.offset < MARSRES_HEADER_SIZE + resources->table_size ||
            entry.offset > resources->total_size ||
            entry.size > resources->total_size - entry.offset) return;
        const uint32_t data_offset = archive_offset + entry.offset;
        appmeta_t meta;
        if (esp_partition_read(p, data_offset, &meta, sizeof(meta)) != ESP_OK ||
            memcmp(meta.magic, "MARSMETA", 8) != 0 || meta.version != 1 ||
            meta.name_size == 0 || meta.name_size > 63 ||
            meta.width != 48 || meta.height != 48 ||
            memchr(meta.display_name, '\0', meta.name_size) != NULL) return;
        uint8_t chunk[256];
        uint32_t crc = 0xFFFFFFFFu;
        for (uint32_t off = 0; off < entry.size; off += sizeof(chunk)) {
            size_t size = entry.size - off < sizeof(chunk) ? entry.size - off : sizeof(chunk);
            if (esp_partition_read(p, data_offset + off, chunk, size) != ESP_OK) return;
            crc = crc32_step(crc, chunk, size);
        }
        if ((crc ^ 0xFFFFFFFFu) != entry.crc32) return;
        memcpy(info->display_name, meta.display_name, meta.name_size);
        info->display_name[meta.name_size] = '\0';
        info->icon_offset = data_offset + sizeof(meta);
        return;
    }
}

static app_slot_info_t s_slots[APP_SLOT_COUNT];
static bool s_scanned;

static bool is_blank(const esp_partition_t *p)
{
    uint8_t buf[32];
    if (esp_partition_read(p, 0, buf, sizeof(buf)) != ESP_OK) return true;
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0xFF) return false;
    }
    return true;
}

/* Parse MARSAPP header and fill app info */
static void parse_app_header(const esp_partition_t *p, uint32_t base,
                             uint32_t capacity, app_slot_info_t *info)
{
    marsapp_header_t hdr;
    if (capacity < sizeof(hdr) || esp_partition_read(p, base, &hdr, sizeof(hdr)) != ESP_OK) {
        return;
    }
    if (memcmp(hdr.magic, MARSAPP_MAGIC, 7) != 0 || hdr.magic[7] != '\0' ||
        hdr.version != MARSAPP_VERSION || (hdr.flags & ~MARSAPP_FLAG_RESOURCES) != 0 ||
        hdr.elf_offset < MARSAPP_HEADER_SIZE ||
        hdr.elf_size == 0 || hdr.elf_offset > capacity ||
        hdr.elf_size > capacity - hdr.elf_offset) {
        return; /* not a valid MARSAPP package */
    }
    info->app_version = hdr.version;
    info->app_code_size = hdr.elf_size;
    info->app_asset_size = 0;
    if ((hdr.flags & MARSAPP_FLAG_RESOURCES) != 0) {
        const uint32_t resource_offset = hdr.elf_offset + hdr.elf_size;
        marsres_header_t resources;
        if (resource_offset <= capacity - sizeof(resources) &&
            esp_partition_read(p, base + resource_offset, &resources, sizeof(resources)) == ESP_OK &&
            memcmp(resources.magic, MARSRES_MAGIC, 7) == 0 && resources.magic[7] == '\0' &&
            resources.version == 1 && resources.entry_size == 48 &&
            resources.file_count > 0 && resources.file_count <= 64 &&
            resources.table_size == resources.file_count * resources.entry_size &&
            resources.total_size == MARSRES_HEADER_SIZE + resources.table_size + resources.data_size &&
            resources.total_size <= capacity - resource_offset) {
            info->app_asset_size = resources.total_size;
            info->used_bytes = resource_offset + resources.total_size;
            read_app_metadata(p, base + resource_offset, &resources, info);
        } else {
            return;
        }
    } else {
        info->used_bytes = hdr.elf_offset + hdr.elf_size;
    }
    memcpy(info->app_name, hdr.app_name, sizeof(info->app_name) - 1);
    info->app_name[sizeof(info->app_name) - 1] = '\0';
    info->package_size = info->used_bytes;
    info->is_valid_app = true;
}

static bool record_package_crc_valid(const esp_partition_t *p, uint32_t offset,
                                     uint32_t size, uint32_t expected)
{
    uint8_t chunk[256];
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t pos = 0; pos < size; pos += sizeof(chunk)) {
        size_t n = size - pos < sizeof(chunk) ? size - pos : sizeof(chunk);
        if (esp_partition_read(p, offset + pos, chunk, n) != ESP_OK) return false;
        crc = crc32_step(crc, chunk, n);
    }
    return (crc ^ 0xFFFFFFFFu) == expected;
}

void app_slots_scan(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    static const char *labels[APP_PARTITION_COUNT] = {"app_0", "app_1", "app_2"};
    const esp_partition_t *partitions[APP_PARTITION_COUNT] = {0};
    uint32_t first_ends[APP_PARTITION_COUNT] = {0};
    int found = 0;
    for (int i = 0; i < APP_PARTITION_COUNT; ++i) {
        const esp_partition_t *p = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, labels[i]);
        if (p == NULL) {
            ESP_LOGW(TAG, "%s partition not found", labels[i]);
            continue;
        }
        s_slots[i].label = labels[i];
        s_slots[i].offset = p->address;
        s_slots[i].total_size = p->size;
        if (!is_blank(p)) {
            char magic[8];
            if (esp_partition_read(p, 0, magic, sizeof(magic)) != ESP_OK) continue;
            if (memcmp(magic, APP_RECORD_MAGIC, sizeof(magic)) != 0) {
                s_slots[i].occupied = true;
                parse_app_header(p, 0, p->size, &s_slots[i]);
                if (!s_slots[i].is_valid_app) {
                    ESP_LOGW(TAG, "%s has an unknown legacy payload; skip appended records", labels[i]);
                    continue;
                }
                first_ends[i] = align_sector(s_slots[i].package_size);
                ++found;
            }
        }
        partitions[i] = p;
    }
    /* Resolve every legacy slot before applying records. A record in app_0 may
     * update logical slot 1 or 2, so a later legacy scan must not overwrite it. */
    for (int i = 0; i < APP_PARTITION_COUNT; ++i) {
        const esp_partition_t *p = partitions[i];
        if (p == NULL) continue;
        /* Look at sector starts after the legacy package. Old erased tails may
         * contain stale bytes, so non-record sectors are skipped safely. */
        for (uint32_t off = first_ends[i]; off + APP_SECTOR_SIZE <= p->size; off += APP_SECTOR_SIZE) {
            app_record_t rec;
            if (esp_partition_read(p, off, &rec, sizeof(rec)) != ESP_OK) break;
            if (memcmp(rec.magic, APP_RECORD_MAGIC, 8) != 0) continue;
            uint32_t crc = crc32_step(0xFFFFFFFFu, (const uint8_t *)&rec,
                                      offsetof(app_record_t, header_crc32)) ^ 0xFFFFFFFFu;
            if (rec.version != 1 || rec.header_crc32 != crc ||
                rec.logical_slot >= APP_SLOT_COUNT || rec.sequence == 0 ||
                rec.package_size < MARSAPP_HEADER_SIZE ||
                rec.package_size > p->size - off - APP_SECTOR_SIZE) continue;
            uint32_t base = off + APP_SECTOR_SIZE;
            app_slot_info_t candidate = {0};
            candidate.label = labels[i];
            candidate.occupied = true;
            candidate.offset = p->address + base;
            candidate.package_base = base;
            candidate.total_size = p->size;
            candidate.sequence = rec.sequence;
            parse_app_header(p, base, rec.package_size, &candidate);
            if (candidate.is_valid_app && candidate.package_size == rec.package_size &&
                record_package_crc_valid(p, base, rec.package_size, rec.package_crc32) &&
                rec.sequence > s_slots[rec.logical_slot].sequence &&
                (!s_slots[rec.logical_slot].is_valid_app ||
                 strcmp(candidate.app_name, s_slots[rec.logical_slot].app_name) == 0)) {
                s_slots[rec.logical_slot] = candidate;
                ++found;
            }
            off += align_sector(rec.package_size);
        }
    }
    s_scanned = true;
    ESP_LOGI(TAG, "scan done: %d records, %d active apps", found, app_slots_valid_count());
}

int app_slots_installed_count(void)
{
    int n = 0;
    for (int i = 0; i < APP_SLOT_COUNT; ++i) {
        if (s_slots[i].occupied) ++n;
    }
    return n;
}

int app_slots_valid_count(void)
{
    int n = 0;
    for (int i = 0; i < APP_SLOT_COUNT; ++i) {
        if (s_slots[i].is_valid_app) ++n;
    }
    return n;
}

bool app_slots_get_info(int index, app_slot_info_t *out)
{
    if (!s_scanned || out == NULL || index < 0 || index >= APP_SLOT_COUNT) return false;
    *out = s_slots[index];
    return true;
}
