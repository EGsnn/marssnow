/**
 * @file mars_app_loader.c
 * @brief 火星动态应用加载器实现
 *
 * MARSAPP v2 包格式：
 *   [96 字节包头][ELF 文件（elf32-xtensa-le, ET_REL）]
 *   [可选 MARSRES 资源尾包（索引 + 直接 mmap 的只读文件）]
 *
 * 加载流程：
 *   1. 读分区 → 校验包头/CRC
 *   2. 解析 ELF（段表/符号表/重定位表）
 *   3. 布局：
 *      - 代码/常量池：内部可执行 RAM
 *      - 只读数据/数据/BSS：PSRAM
 *      - 栈：PSRAM 独立分配（xTaskCreateStaticPinnedToCore）
 *      - 堆：PSRAM 独立池（first-fit）
 *   4. 重定位（内部符号 + 系统符号表）
 *   5. 创建独立 FreeRTOS 任务运行入口
 *
 * 为何不用 Flash XIP：mmap 只读，重定位需要写 .literal/.text 段，因此
 * 当前版本把代码段搬到可写的内部 IRAM。ESP32-S3 的普通 PSRAM 不能直接
 * 作为任意动态应用代码区使用。
 */

#include "mars_app_loader.h"
#include "mars_elf.h"
#include "mars_reloc.h"
#include "mars_app_heap.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mars_services.h"
#include "app_slots.h"

static const char* kTag = "app_loader";
static void memory_snapshot(const char *stage)
{
    ESP_LOGI(kTag, "MEM[%s] internal=%u largest=%u psram=%u exec_free=%u exec_largest=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC));
}

/* ═══════════════════════════════════════════
 *  MARSAPP v2 包头
 * ═══════════════════════════════════════════ */

#define MARSAPP2_MAGIC    "MARSAPP"
#define MARSAPP2_MAGIC_LEN 8
#define MARSAPP2_HEADER_SIZE 96
#define MARSAPP2_VERSION  2
#define MARSAPP2_FLAG_RESOURCES (1u << 0)
#define MARSAPP2_KNOWN_FLAGS MARSAPP2_FLAG_RESOURCES

#define MARSRES_MAGIC "MARSRES"
#define MARSRES_MAGIC_LEN 8
#define MARSRES_VERSION 1
#define MARSRES_HEADER_SIZE 32
#define MARSRES_ENTRY_SIZE 48
#define MARSRES_MAX_FILES 64

typedef struct __attribute__((packed)) {
    char     magic[MARSAPP2_MAGIC_LEN];   /* "MARSAPP" */
    uint16_t version;                     /* 2 */
    uint16_t flags;                       /* bit0: 包含 MARSRES 资源 */
    uint32_t elf_offset;                  /* ELF 数据偏移 */
    uint32_t elf_size;                    /* ELF 数据大小 */
    uint32_t stack_size;                  /* 应用栈大小 */
    uint32_t heap_size;                   /* 应用堆大小 */
    char     app_name[32];                /* 应用名 */
    char     entry_symbol[32];            /* 入口符号名 */
    uint32_t crc32;                       /* 校验 header[0:92] + ELF */
} marsapp2_header_t;

typedef struct __attribute__((packed)) {
    char     magic[MARSRES_MAGIC_LEN];
    uint16_t version;
    uint16_t entry_size;
    uint32_t file_count;
    uint32_t table_size;
    uint32_t data_size;
    uint32_t total_size;
    uint32_t crc32;
} marsres_header_t;

typedef struct __attribute__((packed)) {
    char     name[32];
    uint32_t offset;
    uint32_t size;
    uint32_t crc32;
    uint32_t flags;
} marsres_entry_t;

#define MARS_APP_PRIO 5

/* ═══════════════════════════════════════════
 *  应用运行上下文
 * ═══════════════════════════════════════════ */

typedef struct {
    bool             in_use;
    mars_app_state_t state;
    char             name[32];

    const esp_partition_t* partition;
    uint32_t package_base;
    uint32_t package_size;
    marsapp2_header_t header;

    /* 内存 */
    void*            code_ptr;           /* 代码/常量池（内部 IRAM） */
    uint32_t         code_cap;
    void*            data_ptr;           /* 数据段（PSRAM） */
    uint32_t         data_cap;           /* 数据+BSS 总容量 */
    void*            stack_ptr;          /* 任务栈（PSRAM） */
    mars_app_heap_t  heap;               /* 独立堆 */
    void*            heap_alloc_ptr;     /* heap_caps_malloc 原始指针（heap.pool 可能已对齐） */

    /* ELF 解析结果 */
    mars_elf_t       elf;
    void*            elf_buf;            /* ELF 数据缓冲（PSRAM） */
    uint32_t         elf_buf_size;

    /* 任务 */
    TaskHandle_t     task;
    StaticTask_t*    task_tcb;           /* 静态 TCB（内部 RAM） */
    uint32_t         entry_addr;
    void             (*on_close)(void);
    void             (*on_key)(int key);
    void             (*on_touch)(uint16_t x, uint16_t y);
    uint32_t         crash_count;

    /* 应用自带资源（直接 mmap app_N，不占 PSRAM 副本） */
    const uint8_t*   resource_map;
    esp_partition_mmap_handle_t resource_map_handle;
    uint32_t         resource_map_size;
    uint32_t         resource_count;

    /* 统计 */
    uint32_t         start_ms;
} mars_app_t;

static mars_app_t s_apps[MARS_APP_SLOTS];
static bool s_initialized = false;

/* 当前运行应用（供 malloc 跳板使用） */
static mars_app_t* s_current_app = NULL;

static int current_app_resource_get(const char* name, const void** data, size_t* size);

/* ═══════════════════════════════════════════
 *  系统符号表（linker section "mars_exports" 自动收集，
 *  条目定义见 src/mars_exports_def.c，边界符号由 ld 生成）
 * ═══════════════════════════════════════════ */

#include "mars_export.h"

/* 符号表定义见 src/mars_exports_def.c（地址链接期填充，自动随最终链接更新） */
extern const mars_symtab_entry_t mars_symtab[];
extern const uint32_t mars_symtab_count;

/* ═══════════════════════════════════════════
 *  CRC32
 * ═══════════════════════════════════════════ */

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320u;
            else crc >>= 1;
        }
    }
    return crc;
}

static int validate_and_map_resources(mars_app_t* app)
{
    if ((app->header.flags & MARSAPP2_FLAG_RESOURCES) == 0) return 0;

    const uint32_t resource_offset = app->header.elf_offset + app->header.elf_size;
    marsres_header_t rh;
    if (resource_offset > app->package_size ||
        MARSRES_HEADER_SIZE > app->package_size - resource_offset ||
        esp_partition_read(app->partition, app->package_base + resource_offset, &rh, sizeof(rh)) != ESP_OK) {
        return -1;
    }
    if (memcmp(rh.magic, MARSRES_MAGIC, MARSRES_MAGIC_LEN) != 0 ||
        rh.version != MARSRES_VERSION || rh.entry_size != MARSRES_ENTRY_SIZE ||
        rh.file_count == 0 || rh.file_count > MARSRES_MAX_FILES ||
        rh.table_size != rh.file_count * MARSRES_ENTRY_SIZE ||
        rh.total_size != MARSRES_HEADER_SIZE + rh.table_size + rh.data_size ||
        rh.total_size > app->package_size - resource_offset) {
        ESP_LOGE(kTag, "[%s] invalid resource archive header", app->name);
        return -2;
    }

    const void* mapped = NULL;
    esp_err_t err = esp_partition_mmap(app->partition, app->package_base + resource_offset, rh.total_size,
                                       ESP_PARTITION_MMAP_DATA, &mapped,
                                       &app->resource_map_handle);
    if (err != ESP_OK || mapped == NULL) {
        ESP_LOGE(kTag, "[%s] resource mmap failed: %s", app->name, esp_err_to_name(err));
        return -3;
    }
    app->resource_map = (const uint8_t*)mapped;
    app->resource_map_size = rh.total_size;
    app->resource_count = rh.file_count;

    uint32_t crc = crc32_update(0xFFFFFFFFu, app->resource_map,
                                offsetof(marsres_header_t, crc32));
    crc = crc32_update(crc, app->resource_map + sizeof(marsres_header_t),
                       rh.total_size - sizeof(marsres_header_t)) ^ 0xFFFFFFFFu;
    if (crc != rh.crc32) {
        ESP_LOGE(kTag, "[%s] resource archive CRC mismatch", app->name);
        return -4;
    }

    uint32_t previous_end = MARSRES_HEADER_SIZE + rh.table_size;
    for (uint32_t i = 0; i < rh.file_count; ++i) {
        marsres_entry_t entry;
        memcpy(&entry, app->resource_map + MARSRES_HEADER_SIZE + i * MARSRES_ENTRY_SIZE,
               sizeof(entry));
        if (entry.name[0] == '\0' || memchr(entry.name, '\0', sizeof(entry.name)) == NULL ||
            entry.flags != 0 || (entry.offset & 3u) != 0 || entry.offset < previous_end ||
            entry.offset > rh.total_size || entry.size > rh.total_size - entry.offset) {
            ESP_LOGE(kTag, "[%s] invalid resource entry %u", app->name, (unsigned)i);
            return -5;
        }
        for (uint32_t j = 0; j < i; ++j) {
            marsres_entry_t prior;
            memcpy(&prior, app->resource_map + MARSRES_HEADER_SIZE + j * MARSRES_ENTRY_SIZE,
                   sizeof(prior));
            if (strcmp(prior.name, entry.name) == 0) {
                ESP_LOGE(kTag, "[%s] duplicate resource '%s'", app->name, entry.name);
                return -6;
            }
        }
        uint32_t file_crc = crc32_update(0xFFFFFFFFu, app->resource_map + entry.offset,
                                         entry.size) ^ 0xFFFFFFFFu;
        if (file_crc != entry.crc32) {
            ESP_LOGE(kTag, "[%s] resource CRC mismatch: %s", app->name, entry.name);
            return -7;
        }
        previous_end = entry.offset + entry.size;
    }

    ESP_LOGI(kTag, "[%s] resources ready: %u files, %u bytes", app->name,
             (unsigned)rh.file_count, (unsigned)rh.total_size);
    return 0;
}

static int current_app_resource_get(const char* name, const void** data, size_t* size)
{
    if (name == NULL || data == NULL || size == NULL || s_current_app == NULL ||
        !s_current_app->in_use || s_current_app->resource_map == NULL) {
        return -1;
    }
    mars_app_t* app = s_current_app;
    for (uint32_t i = 0; i < app->resource_count; ++i) {
        marsres_entry_t entry;
        memcpy(&entry, app->resource_map + MARSRES_HEADER_SIZE + i * MARSRES_ENTRY_SIZE,
               sizeof(entry));
        if (strcmp(entry.name, name) == 0) {
            *data = app->resource_map + entry.offset;
            *size = entry.size;
            return 0;
        }
    }
    return -2;
}

/* ═══════════════════════════════════════════
 *  系统符号查找
 * ═══════════════════════════════════════════ */

uint32_t mars_symtab_lookup(const char* name)
{
    for (uint32_t i = 0; i < mars_symtab_count; i++) {
        if (strcmp(mars_symtab[i].name, name) == 0) {
            return mars_symtab[i].addr;
        }
    }
    return 0;
}

/* malloc/free 跳板：把应用内存分配到当前应用独立堆 */
static void* app_malloc_jump(size_t size);
static void app_free_jump(void* ptr);
static void* app_calloc_jump(size_t n, size_t size);
static void* app_realloc_jump(void* ptr, size_t size);

static void* app_malloc_jump(size_t size)
{
    if (s_current_app && s_current_app->in_use) {
        return mars_heap_malloc(&s_current_app->heap, size);
    }
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void app_free_jump(void* ptr)
{
    if (s_current_app && s_current_app->in_use) {
        mars_heap_free(&s_current_app->heap, ptr);
        return;
    }
    free(ptr);
}

/* 符号解析回调 */
static uint32_t sym_resolver(const char* name, void* ctx)
{
    (void)ctx;
    /* 1. malloc/free 跳板优先（应用内存必须进独立堆） */
    if (strcmp(name, "malloc") == 0)  return (uint32_t)(uintptr_t)app_malloc_jump;
    if (strcmp(name, "free") == 0)    return (uint32_t)(uintptr_t)app_free_jump;
    if (strcmp(name, "calloc") == 0)  return (uint32_t)(uintptr_t)app_calloc_jump;
    if (strcmp(name, "realloc") == 0) return (uint32_t)(uintptr_t)app_realloc_jump;

    /* 2. 系统符号表 */
    uint32_t addr = mars_symtab_lookup(name);
    if (addr) return addr;

    ESP_LOGE(kTag, "unresolved symbol: %s", name);
    return 0;
}

/* calloc/realloc 跳板 */
static void* app_calloc_jump(size_t n, size_t size)
{
    void* p = app_malloc_jump(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

static void* app_realloc_jump(void* ptr, size_t size)
{
    if (!s_current_app || !s_current_app->in_use) return realloc(ptr, size);
    return mars_heap_realloc(&s_current_app->heap, ptr, size);
}

/* ═══════════════════════════════════════════
 *  应用任务
 * ═══════════════════════════════════════════ */

static void app_task(void* arg)
{
    mars_app_t* app = (mars_app_t*)arg;
    s_current_app = app;
    app->state = MARS_APP_STATE_RUNNING;
    app->start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(kTag, "[%s] entry @ 0x%x", app->name, app->entry_addr);
    memory_snapshot("app-entry");

    /* 调用应用入口 */
    typedef void (*entry_fn_t)(void);
    entry_fn_t entry = (entry_fn_t)(uintptr_t)app->entry_addr;
    entry();

    /* 正常返回 */
    app->state = MARS_APP_STATE_EXITED;
    s_current_app = NULL;
    ESP_LOGI(kTag, "[%s] exited", app->name);
    memory_snapshot("app-exited");

    /* Keep the static task object alive until mars_app_stop() owns the
     * teardown. Clearing the handle before self-delete lets another task free
     * this task's stack while the kernel is still using it. */
    for (;;) {
        vTaskSuspend(NULL);
    }
}

/* 栈溢出 hook（在组件外注册，这里提供辅助） */
void mars_app_on_stack_overflow(mars_app_t* app)
{
    ESP_LOGE(kTag, "[%s] STACK OVERFLOW! crash_count=%u", app->name, app->crash_count);
    app->state = MARS_APP_STATE_CRASHED;
    app->crash_count++;
}

/* ═══════════════════════════════════════════
 *  段布局（全部搬到 RAM，代码段可执行）
 * ═══════════════════════════════════════════ */

/**
 * 段分类：
 *   - 代码类：.text / .literal / .init_array → 内部 IRAM
 *   - 数据类：.rodata* / .data* / .bss* → PSRAM
 *   - 忽略：.comment / .xt.lit / .xt.prop / .xtensa.info / .symtab / .strtab / .rela.*
 */
static bool is_code_section(const char* name)
{
    return strncmp(name, ".text", 5) == 0 ||
           strncmp(name, ".literal", 8) == 0 ||
           strncmp(name, ".init_array", 11) == 0;
}

static bool is_data_section(const char* name)
{
    return strncmp(name, ".rodata", 7) == 0 ||
           strncmp(name, ".data", 5) == 0 ||
           strncmp(name, ".bss", 4) == 0;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment)
{
    if (alignment <= 1) return value;
    return (value + alignment - 1) & ~(alignment - 1);
}

/* MALLOC_CAP_EXEC on ESP32-S3 is 32-bit-access-only memory.  Writing it via
 * memset/memcpy may emit byte stores and raise LoadStoreError, so populate it
 * one aligned word at a time. */
static void exec_memzero(void* dst, uint32_t size)
{
    volatile uint32_t* out = (volatile uint32_t*)dst;
    for (uint32_t i = 0; i < align_up_u32(size, 4) / 4; ++i) out[i] = 0;
}

static void exec_memcpy(void* dst, const void* src, uint32_t size)
{
    volatile uint32_t* out = (volatile uint32_t*)dst;
    const uint8_t* in = (const uint8_t*)src;
    for (uint32_t i = 0; i < align_up_u32(size, 4) / 4; ++i) {
        uint32_t word = 0;
        const uint32_t base = i * 4;
        for (uint32_t byte = 0; byte < 4 && base + byte < size; ++byte) {
            word |= (uint32_t)in[base + byte] << (byte * 8);
        }
        out[i] = word;
    }
}

static int layout_segments(mars_app_t* app)
{
    const mars_elf_t* elf = &app->elf;
    uint32_t code_size = 0, data_size = 0;
    uint32_t code_alignment = 4, data_alignment = 4;

    /* 计算代码段与数据段总大小（对齐后） */
    for (uint32_t i = 0; i < elf->shnum; i++) {
        mars_elf32_shdr_t* sh = &elf->shdrs[i];
        const char* name = mars_elf_section_name(elf, sh->sh_name);
        if (!name) continue;

        if (sh->sh_type == SHT_NOBITS) {
            if (is_data_section(name)) {
                uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
                if (align > data_alignment) data_alignment = align;
                data_size = align_up_u32(data_size, align) + sh->sh_size;
            }
            continue;
        }
        if (sh->sh_type != SHT_PROGBITS) continue;

        if (is_code_section(name)) {
            uint32_t align = sh->sh_addralign > 4 ? sh->sh_addralign : 4;
            if (align > code_alignment) code_alignment = align;
            code_size = align_up_u32(code_size, align) + align_up_u32(sh->sh_size, 4);
        } else if (is_data_section(name)) {
            uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
            if (align > data_alignment) data_alignment = align;
            data_size = align_up_u32(data_size, align) + sh->sh_size;
        }
    }

    ESP_LOGI(kTag, "[%s] segments: code=%u data+bss=%u", app->name, code_size, data_size);
    memory_snapshot("before-code");

    /* ESP-IDF 会为 MALLOC_CAP_EXEC 自动补 MALLOC_CAP_32BIT。EXEC 与 8BIT
     * 在 Xtensa 目标上是互斥请求，组合使用会被 heap_caps 直接拒绝。 */
    if (code_size > 0) {
        app->code_ptr = heap_caps_aligned_alloc(code_alignment, code_size, MALLOC_CAP_EXEC);
        if (!app->code_ptr) {
            ESP_LOGE(kTag, "[%s] no EXEC internal RAM for code (%u B); PSRAM exec mapping not yet implemented",
                     app->name, code_size);
            return -1;
        }
        ESP_LOGI(kTag, "[%s] code @0x%x cap=%u (EXEC internal)", app->name,
                 (uint32_t)(uintptr_t)app->code_ptr, code_size);
        app->code_cap = code_size;
        exec_memzero(app->code_ptr, code_size);
    }

    /* 分配数据区（PSRAM） */
    if (data_size > 0) {
        /* Sizes above are computed from offset zero. The allocation base must
         * meet the largest ELF alignment too: ordinary malloc may be only
         * four-byte aligned, causing an extra four-byte gap at an int64_t and
         * placing the last BSS word beyond the zeroed allocation. */
        app->data_ptr = heap_caps_aligned_alloc(data_alignment, data_size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!app->data_ptr) {
            ESP_LOGE(kTag, "[%s] no PSRAM for data (%u B)", app->name, data_size);
            return -1;
        }
        app->data_cap = data_size;
        memset(app->data_ptr, 0, data_size);
    }

    /* 分配段地址并拷贝内容 */
    uint32_t code_cur = (uint32_t)(uintptr_t)app->code_ptr;
    uint32_t data_cur = (uint32_t)(uintptr_t)app->data_ptr;

    for (uint32_t i = 0; i < elf->shnum; i++) {
        mars_elf32_shdr_t* sh = &elf->shdrs[i];
        const char* name = mars_elf_section_name(elf, sh->sh_name);
        if (!name) continue;

        /* NOBITS：只设地址，清零（整体已清零，跳过拷贝） */
        if (sh->sh_type == SHT_NOBITS) {
            if (is_data_section(name)) {
                uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
                data_cur = (data_cur + align - 1) & ~(align - 1);
                sh->sh_addr = data_cur;
                data_cur += sh->sh_size;
            } else {
                sh->sh_addr = 0;
            }
            continue;
        }

        if (sh->sh_type == SHT_PROGBITS) {
            if (is_code_section(name)) {
                uint32_t align = sh->sh_addralign > 4 ? sh->sh_addralign : 4;
                code_cur = (code_cur + align - 1) & ~(align - 1);
                sh->sh_addr = code_cur;
                exec_memcpy((void*)(uintptr_t)code_cur,
                            app->elf_buf + sh->sh_offset, sh->sh_size);
                code_cur += align_up_u32(sh->sh_size, 4);
            } else if (is_data_section(name)) {
                uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
                data_cur = (data_cur + align - 1) & ~(align - 1);
                sh->sh_addr = data_cur;
                memcpy((void*)(uintptr_t)data_cur,
                       app->elf_buf + sh->sh_offset, sh->sh_size);
                data_cur += sh->sh_size;
            } else {
                /* 其他 PROGBITS（元数据等）不布局 */
                sh->sh_addr = 0;
            }
        } else {
            sh->sh_addr = 0;
        }
    }

    return 0;
}

static uint32_t find_defined_symbol(const mars_elf_t* elf, const char* wanted,
                                    uint8_t symbol_type)
{
    for (uint32_t i = 0; i < elf->symtab_count; ++i) {
        const mars_elf32_sym_t* sym = &elf->symtab[i];
        if (ELF32_ST_TYPE(sym->st_info) != symbol_type || sym->st_shndx == SHN_UNDEF ||
            sym->st_shndx >= elf->shnum) {
            continue;
        }
        const char* name = mars_elf_symbol_name(elf, sym->st_name);
        if (name != NULL && strcmp(name, wanted) == 0) {
            return elf->shdrs[sym->st_shndx].sh_addr + sym->st_value;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════
 *  加载流程
 * ═══════════════════════════════════════════ */

void mars_app_loader_init(void)
{
    if (s_initialized) return;
    memset(s_apps, 0, sizeof(s_apps));
    s_initialized = true;
    mars_services_set_app_resource_provider(current_app_resource_get);
    ESP_LOGI(kTag, "loader initialized (%u symbols exported)", mars_symtab_count);
}

int mars_app_load(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];

    if (app->in_use) return -2;  /* 已加载 */

    /* 1. Resolve logical app id to its containing partition and package offset. */
    app_slot_info_t location;
    if (!app_slots_get_info(slot, &location) || !location.is_valid_app) return -3;
    const char *label = location.label;
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) {
        ESP_LOGE(kTag, "partition %s not found", label);
        return -3;
    }
    app->partition = part;
    app->package_base = location.package_base;
    app->package_size = location.package_size;

    /* 2. 读包头 */
    marsapp2_header_t hdr;
    esp_err_t err = esp_partition_read(part, app->package_base, &hdr, sizeof(hdr));
    if (err != ESP_OK) return -4;
    if (memcmp(hdr.magic, MARSAPP2_MAGIC, MARSAPP2_MAGIC_LEN) != 0) {
        ESP_LOGE(kTag, "%s: no MARSAPP magic", label);
        return -5;
    }
    if (hdr.version != MARSAPP2_VERSION) {
        ESP_LOGE(kTag, "%s: unsupported version %d", label, hdr.version);
        return -6;
    }
    if ((hdr.flags & ~MARSAPP2_KNOWN_FLAGS) != 0) {
        ESP_LOGE(kTag, "%s: unsupported flags 0x%x", label, (unsigned)hdr.flags);
        return -6;
    }
    if (hdr.app_name[0] == '\0' || memchr(hdr.app_name, '\0', sizeof(hdr.app_name)) == NULL ||
        hdr.entry_symbol[0] == '\0' ||
        memchr(hdr.entry_symbol, '\0', sizeof(hdr.entry_symbol)) == NULL) {
        ESP_LOGE(kTag, "%s: invalid app name or entry symbol", label);
        return -7;
    }
    if (hdr.elf_offset < MARSAPP2_HEADER_SIZE || hdr.elf_size == 0 ||
        hdr.elf_offset > app->package_size || hdr.elf_size > app->package_size - hdr.elf_offset) {
        ESP_LOGE(kTag, "%s: ELF exceeds partition", label);
        return -8;
    }

    /* 3. 读 ELF 到 PSRAM 缓冲 */
    void* buf = heap_caps_malloc(hdr.elf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return -9;
    err = esp_partition_read(part, app->package_base + hdr.elf_offset, buf, hdr.elf_size);
    if (err != ESP_OK) {
        heap_caps_free(buf);
        return -10;
    }

    uint32_t crc = crc32_update(0xFFFFFFFFu, (const uint8_t*)&hdr,
                                offsetof(marsapp2_header_t, crc32));
    crc = crc32_update(crc, (const uint8_t*)buf, hdr.elf_size) ^ 0xFFFFFFFFu;
    if (crc != hdr.crc32) {
        ESP_LOGE(kTag, "%s: CRC mismatch (expected=%08x actual=%08x)",
                 label, (unsigned)hdr.crc32, (unsigned)crc);
        heap_caps_free(buf);
        return -11;
    }

    /* 4. 解析 ELF */
    mars_elf_t* elf = &app->elf;
    int rc = mars_elf_parse((const uint8_t*)buf, hdr.elf_size, elf);
    if (rc != 0) {
        ESP_LOGE(kTag, "%s: ELF parse failed (%d)", label, rc);
        heap_caps_free(buf);
        return -12;
    }

    app->in_use = true;
    app->state = MARS_APP_STATE_LOADED;
    app->header = hdr;
    app->elf_buf = buf;
    app->elf_buf_size = hdr.elf_size;
    memcpy(app->name, hdr.app_name, sizeof(app->name) - 1);
    app->name[sizeof(app->name) - 1] = '\0';

    if (validate_and_map_resources(app) != 0) {
        mars_app_unload(slot);
        return -16;
    }

    /* 5. 布局段 */
    if (layout_segments(app) != 0) {
        mars_app_unload(slot);
        return -12;
    }

    /* 6. 初始化独立堆 */
    uint32_t heap_size = hdr.heap_size ? hdr.heap_size : 64 * 1024;
    void* heap_pool = heap_caps_malloc(heap_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!heap_pool) {
        mars_app_unload(slot);
        return -13;
    }
    app->heap_alloc_ptr = heap_pool;
    mars_heap_init(&app->heap, heap_pool, heap_size);

    /* 7. 重定位 */
    if (mars_reloc_apply(elf, sym_resolver, NULL, false) != 0) {
        ESP_LOGE(kTag, "%s: relocation failed", label);
        mars_app_unload(slot);
        return -14;
    }

    /* 8. 找入口符号 */
    uint32_t entry = find_defined_symbol(elf, hdr.entry_symbol, STT_FUNC);
    if (entry == 0) {
        ESP_LOGE(kTag, "%s: entry symbol '%s' not found", label, hdr.entry_symbol);
        mars_app_unload(slot);
        return -15;
    }
    app->entry_addr = entry;
    app->on_close = (void (*)(void))(uintptr_t)find_defined_symbol(elf, "app_on_close", STT_FUNC);
    app->on_key = (void (*)(int))(uintptr_t)find_defined_symbol(elf, "app_on_key", STT_FUNC);
    app->on_touch = (void (*)(uint16_t, uint16_t))(uintptr_t)find_defined_symbol(
        elf, "app_on_touch", STT_FUNC);

    ESP_LOGI(kTag, "[%s] loaded: code=0x%x data=0x%x entry=0x%x",
             app->name, (uint32_t)(uintptr_t)app->code_ptr,
             (uint32_t)(uintptr_t)app->data_ptr, entry);

    return 0;
}

int mars_app_start(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];
    if (!app->in_use) return -2;
    if (app->state == MARS_APP_STATE_RUNNING) return 0;
    if (app->task != NULL) return -6;

    /* 分配应用栈（PSRAM）与静态 TCB（内部 RAM） */
    uint32_t stack_size = app->header.stack_size ? app->header.stack_size : 8 * 1024;
    app->stack_ptr = heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!app->stack_ptr) return -3;

    app->task_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!app->task_tcb) {
        heap_caps_free(app->stack_ptr);
        app->stack_ptr = NULL;
        return -4;
    }
    memset(app->task_tcb, 0, sizeof(StaticTask_t));

    app->task = xTaskCreateStaticPinnedToCore(
        app_task, app->name,
        stack_size / sizeof(StackType_t),
        app, MARS_APP_PRIO,
        (StackType_t*)app->stack_ptr,
        app->task_tcb,
        1);
    if (app->task == NULL) {
        heap_caps_free(app->task_tcb);
        app->task_tcb = NULL;
        heap_caps_free(app->stack_ptr);
        app->stack_ptr = NULL;
        return -5;
    }

    return 0;
}

int mars_app_stop(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];
    if (!app->in_use) return -2;
    if (app->task) {
        if (app->on_close != NULL) {
            app->on_close();
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (app->task != NULL) {
            vTaskDelete(app->task);
            app->task = NULL;
        }
    }
    if (s_current_app == app) {
        s_current_app = NULL;
    }
    if (app->task_tcb) {
        heap_caps_free(app->task_tcb);
        app->task_tcb = NULL;
    }
    if (app->stack_ptr) {
        heap_caps_free(app->stack_ptr);
        app->stack_ptr = NULL;
    }
    app->state = MARS_APP_STATE_LOADED;
    return 0;
}

int mars_app_unload(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];
    if (!app->in_use) return -2;

    mars_app_stop(slot);

    if (app->resource_map != NULL) {
        esp_partition_munmap(app->resource_map_handle);
        app->resource_map = NULL;
        app->resource_map_handle = 0;
        app->resource_map_size = 0;
        app->resource_count = 0;
    }

    if (app->code_ptr) {
        heap_caps_free(app->code_ptr);
        app->code_ptr = NULL;
    }
    if (app->elf_buf) {
        heap_caps_free(app->elf_buf);
        app->elf_buf = NULL;
    }
    if (app->data_ptr) {
        heap_caps_free(app->data_ptr);
        app->data_ptr = NULL;
    }
    if (app->heap_alloc_ptr) {
        heap_caps_free(app->heap_alloc_ptr);
        app->heap_alloc_ptr = NULL;
        memset(&app->heap, 0, sizeof(app->heap));
    }

    memset(app, 0, sizeof(*app));
    memory_snapshot("app-unloaded");
    return 0;
}

int mars_app_launch(int slot)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    int rc = 0;
    if (!s_apps[slot].in_use) {
        rc = mars_app_load(slot);
        if (rc != 0) return rc;
    }
    return mars_app_start(slot);
}

int mars_app_handle_key(int slot, int key)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];
    if (!app->in_use || app->state != MARS_APP_STATE_RUNNING || app->on_key == NULL) return -2;
    app->on_key(key);
    return 0;
}

int mars_app_handle_touch(int slot, uint16_t x, uint16_t y)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS) return -1;
    mars_app_t* app = &s_apps[slot];
    if (!app->in_use || app->state != MARS_APP_STATE_RUNNING || app->on_touch == NULL) return -2;
    app->on_touch(x, y);
    return 0;
}

int mars_app_get_info(int slot, mars_app_info_t* info)
{
    if (slot < 0 || slot >= MARS_APP_SLOTS || !info) return -1;
    const mars_app_t* app = &s_apps[slot];
    if (!app->in_use) {
        memset(info, 0, sizeof(*info));
        info->slot = slot;
        info->state = MARS_APP_STATE_EMPTY;
        return 0;
    }
    memset(info, 0, sizeof(*info));
    info->slot = slot;
    info->state = app->state;
    strncpy(info->name, app->name, sizeof(info->name) - 1);
    info->load_addr = (uint32_t)(uintptr_t)app->code_ptr;
    info->data_addr = (uint32_t)(uintptr_t)app->data_ptr;
    info->stack_size = app->header.stack_size;
    info->heap_size = app->heap.pool_size;
    uint32_t used = 0, peak = 0, blocks = 0;
    mars_heap_stats(&app->heap, &used, &peak, &blocks);
    info->heap_used = used;
    info->heap_peak = peak;
    info->crash_count = app->crash_count;
    if (app->state == MARS_APP_STATE_RUNNING) {
        info->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000) - app->start_ms;
    }
    return 0;
}

int mars_app_scan_all(void)
{
    int count = 0;
    for (int i = 0; i < MARS_APP_SLOTS; i++) {
        if (s_apps[i].in_use) count++;
    }
    return count;
}
