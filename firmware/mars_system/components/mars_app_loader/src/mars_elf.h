/**
 * @file mars_elf.h
 * @brief ELF32 格式定义（小端，Xtensa ESP32-S3）
 *
 * 应用包内的 ELF 由 xtensa-esp32s3-elf-gcc 生成（elf32-xtensa-le）。
 * 本文件定义加载器所需的 ELF 结构。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════
 *  ELF 常量
 * ═══════════════════════════════════════════ */

#define ELF_MAGIC0       0x7f
#define ELF_MAGIC1       'E'
#define ELF_MAGIC2       'L'
#define ELF_MAGIC3       'F'

/* e_type */
#define ET_REL           1      /* 可重定位文件 */
#define ET_EXEC          2      /* 可执行文件 */

/* e_machine */
#define EM_XTENSA        94

/* 端序 */
#define ELFDATA2LSB      1      /* 小端 */

/* sh_type */
#define SHT_PROGBITS     1
#define SHT_SYMTAB       2
#define SHT_STRTAB       3
#define SHT_RELA         4
#define SHT_NOBITS       8
#define SHT_REL          9

/* st_info 位 */
#define STB_LOCAL        0
#define STB_GLOBAL       1
#define STT_NOTYPE       0
#define STT_OBJECT       1
#define STT_FUNC         2
#define STT_SECTION      3

/* st_shndx */
#define SHN_UNDEF        0
#define SHN_ABS          0xfff1

/* Xtensa 重定位类型（实测工具链值） */
#define R_XTENSA_NONE        0
#define R_XTENSA_32          1
#define R_XTENSA_R_32        2
#define R_XTENSA_ASM_EXPAND  11
#define R_XTENSA_SLOT0_OP    20
#define R_XTENSA_GLOB_DAT    35
#define R_XTENSA_JMP_SLOT    36
#define R_XTENSA_RELATIVE    37
#define R_XTENSA_PLT         38   /* 工具链实际输出为 6，见下 */
/* 实测：xtensa-esp32s3-elf-gcc 输出 R_XTENSA_PLT = 6 */
#define R_XTENSA_PLT_ACTUAL  6

/* ═══════════════════════════════════════════
 *  ELF 结构（小端）
 * ═══════════════════════════════════════════ */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} mars_elf32_ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} mars_elf32_shdr_t;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} mars_elf32_sym_t;

/* RELA：带显式加数 */
typedef struct {
    uint32_t r_offset;
    uint32_t r_info;    /* 低 8 位 type，高 24 位符号索引 */
    int32_t  r_addend;
} mars_elf32_rela_t;

/* 便捷宏 */
#define ELF32_R_SYM(info)   ((info) >> 8)
#define ELF32_R_TYPE(info)  ((info) & 0xff)
#define ELF32_ST_TYPE(info) ((info) & 0xf)
#define ELF32_ST_BIND(info) ((info) >> 4)

/**
 * 解析后的 ELF 视图（加载器内部使用）
 */
typedef struct {
    /* 原始数据 */
    const uint8_t* data;
    uint32_t       size;

    /* 头部 */
    mars_elf32_ehdr_t ehdr;

    /* section 表 */
    mars_elf32_shdr_t* shdrs;     /* 指向 data + e_shoff */
    uint32_t           shnum;

    /* 字符串表 */
    const char*  shstrtab;        /* section 名字符串表 */
    const char*  strtab;          /* 符号名字符串表 */
    uint32_t     strtab_size;

    /* 符号表 */
    mars_elf32_sym_t* symtab;
    uint32_t          symtab_count;

    /* 需要加载的段（按 sh_offset 排序的 PROGBITS/NOBITS） */
    int32_t      load_count;      /* 加载段数量 */
} mars_elf_t;

/**
 * 解析 ELF 文件
 * @param data ELF 数据
 * @param size 数据大小
 * @param elf 输出解析结果
 * @return 0 成功，负值错误码
 */
int mars_elf_parse(const uint8_t* data, uint32_t size, mars_elf_t* elf);

/**
 * 获取 section 名
 */
const char* mars_elf_section_name(const mars_elf_t* elf, uint32_t sh_name_off);

/**
 * 获取符号名
 */
const char* mars_elf_symbol_name(const mars_elf_t* elf, uint32_t st_name_off);

/**
 * 获取 section 在内存中的加载地址（由加载器分配后设置）
 */
uint32_t mars_elf_section_load_addr(const mars_elf_t* elf, int32_t sec_index);

#ifdef __cplusplus
}
#endif
