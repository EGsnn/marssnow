/**
 * @file mars_elf.c
 * @brief ELF32 解析器（小端 Xtensa）
 */

#include "mars_elf.h"

#include <string.h>
#include <stdio.h>

static const char* kTag = "mars_elf";

/**
 * 解析 ELF 头并校验
 */
int mars_elf_parse(const uint8_t* data, uint32_t size, mars_elf_t* elf)
{
    if (!data || !elf) return -1;
    if (size < sizeof(mars_elf32_ehdr_t)) return -2;

    memset(elf, 0, sizeof(*elf));
    elf->data = data;
    elf->size = size;

    /* 校验魔数 */
    const uint8_t* ident = data;
    if (ident[0] != ELF_MAGIC0 || ident[1] != ELF_MAGIC1 ||
        ident[2] != ELF_MAGIC2 || ident[3] != ELF_MAGIC3) {
        return -3;
    }
    if (ident[5] != ELFDATA2LSB) {
        printf("[%s] not little-endian ELF\n", kTag);
        return -4;
    }

    /* 头部（小端直接读，ESP32-S3 CPU 小端） */
    memcpy(&elf->ehdr, data, sizeof(mars_elf32_ehdr_t));

    if (elf->ehdr.e_machine != EM_XTENSA) {
        printf("[%s] machine %d != Xtensa(94)\n", kTag, elf->ehdr.e_machine);
        return -5;
    }
    if (elf->ehdr.e_type != ET_REL && elf->ehdr.e_type != ET_EXEC) {
        printf("[%s] unsupported e_type %d\n", kTag, elf->ehdr.e_type);
        return -6;
    }

    /* section 表 */
    if (elf->ehdr.e_shoff == 0 || elf->ehdr.e_shnum == 0) {
        return -7;
    }
    if (elf->ehdr.e_shoff + (uint32_t)elf->ehdr.e_shnum * sizeof(mars_elf32_shdr_t) > size) {
        return -8;
    }
    elf->shdrs = (mars_elf32_shdr_t*)(data + elf->ehdr.e_shoff);
    elf->shnum = elf->ehdr.e_shnum;

    /* section 名字符串表 */
    if (elf->ehdr.e_shstrndx >= elf->shnum) return -9;
    {
        const mars_elf32_shdr_t* shstr = &elf->shdrs[elf->ehdr.e_shstrndx];
        if (shstr->sh_offset + shstr->sh_size > size) return -10;
        elf->shstrtab = (const char*)(data + shstr->sh_offset);
    }

    /* 扫描符号表和字符串表 */
    for (uint32_t i = 0; i < elf->shnum; i++) {
        const mars_elf32_shdr_t* sh = &elf->shdrs[i];
        if (sh->sh_type == SHT_SYMTAB) {
            elf->symtab = (mars_elf32_sym_t*)(data + sh->sh_offset);
            elf->symtab_count = sh->sh_size / sizeof(mars_elf32_sym_t);
            if (sh->sh_link < elf->shnum) {
                const mars_elf32_shdr_t* str = &elf->shdrs[sh->sh_link];
                elf->strtab = (const char*)(data + str->sh_offset);
                elf->strtab_size = str->sh_size;
            }
        }
    }

    return 0;
}

/**
 * 获取 section 名
 */
const char* mars_elf_section_name(const mars_elf_t* elf, uint32_t sh_name_off)
{
    if (!elf || !elf->shstrtab) return "";
    return elf->shstrtab + sh_name_off;
}

/**
 * 获取符号名
 */
const char* mars_elf_symbol_name(const mars_elf_t* elf, uint32_t st_name_off)
{
    if (!elf || !elf->strtab) return "";
    if (st_name_off >= elf->strtab_size) return "";
    return elf->strtab + st_name_off;
}

/**
 * 获取 section 在内存中的加载地址（由加载器分配后设置）
 * 加载器会把段基址写回 sh_addr，这里直接读取
 */
uint32_t mars_elf_section_load_addr(const mars_elf_t* elf, int32_t sec_index)
{
    if (!elf || sec_index < 0 || sec_index >= (int32_t)elf->shnum) return 0;
    return elf->shdrs[sec_index].sh_addr;
}
