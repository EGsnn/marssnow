/**
 * @file mars_reloc.c
 * @brief XTensa 重定位器实现
 *
 * 支持实测工具链（xtensa-esp32s3-elf-gcc 15.2.0）输出的重定位类型：
 *   R_XTENSA_32        (1)  ：绝对 32 位地址（literal 池 / GOT / 数据指针）
 *   R_XTENSA_PLT_ACTUAL(6)  ：函数地址（简化 PLT，直接写入函数地址）
 *   R_XTENSA_SLOT0_OP  (20) ：L32R 指令立即数
 *
 * 忽略链接器元数据重定位（.xt.lit / .xt.prop 等）。
 */

#include "mars_reloc.h"
#include "mars_elf.h"

#include <stdio.h>

/* Relocation targets may live in MALLOC_CAP_EXEC IRAM.  On ESP32-S3 that
 * memory only permits aligned 32-bit data accesses. */
static uint8_t exec_read_u8(const void* address)
{
    uintptr_t value = (uintptr_t)address;
    const volatile uint32_t* word = (const volatile uint32_t*)(value & ~(uintptr_t)3);
    return (uint8_t)(*word >> ((value & 3) * 8));
}

static void exec_write_u8(void* address, uint8_t byte)
{
    uintptr_t value = (uintptr_t)address;
    volatile uint32_t* word = (volatile uint32_t*)(value & ~(uintptr_t)3);
    const uint32_t shift = (uint32_t)(value & 3) * 8;
    uint32_t current = *word;
    current = (current & ~(0xffu << shift)) | ((uint32_t)byte << shift);
    *word = current;
}

static void exec_write_u32(void* address, uint32_t value)
{
    uint8_t* out = (uint8_t*)address;
    exec_write_u8(out + 0, (uint8_t)(value));
    exec_write_u8(out + 1, (uint8_t)(value >> 8));
    exec_write_u8(out + 2, (uint8_t)(value >> 16));
    exec_write_u8(out + 3, (uint8_t)(value >> 24));
}

/* L32R 编码（实测）：
 *   字节0 = (reg << 4) | 0x01
 *   字节1..2 = imm16（小端序，16 位有符号）
 *   目标地址 = ((PC + 3) & ~3) + (imm16 << 2)
 */

void mars_reloc_write_l32r_imm(uint8_t* insn, uint8_t reg, int16_t imm16)
{
    exec_write_u8(insn + 0, (uint8_t)((reg << 4) | 0x01));
    exec_write_u8(insn + 1, (uint8_t)(imm16 & 0xff));
    exec_write_u8(insn + 2, (uint8_t)((imm16 >> 8) & 0xff));
}

uint8_t mars_reloc_read_l32r_reg(const uint8_t* insn)
{
    return (uint8_t)((exec_read_u8(insn) >> 4) & 0xf);
}

/* ═══════════════════════════════════════════
 *  符号地址解析
 * ═══════════════════════════════════════════ */

/**
 * 解析符号地址
 * @return 0 表示符号未找到
 */
static uint32_t resolve_symbol(const mars_elf_t* elf,
                               mars_sym_resolver_t resolver, void* ctx,
                               const mars_elf32_sym_t* sym,
                               bool verbose)
{
    /* STT_SECTION：符号值是 section 的加载基址 */
    if (ELF32_ST_TYPE(sym->st_info) == STT_SECTION) {
        uint32_t base = elf->shdrs[sym->st_shndx].sh_addr;
        if (verbose) {
            printf("  [section sym] sec#%d base=0x%x\n", sym->st_shndx, (unsigned)base);
        }
        return base;
    }

    const char* name = mars_elf_symbol_name(elf, sym->st_name);

    /* 未定义符号（外部符号）：查系统符号表 */
    if (sym->st_shndx == SHN_UNDEF) {
        if (!name || !name[0]) return 0;
        if (resolver) {
            uint32_t addr = resolver(name, ctx);
            if (verbose) {
                printf("  [extern] %s -> 0x%x\n", name, (unsigned)addr);
            }
            return addr;
        }
        printf("  !! undefined symbol: %s (no resolver)\n", name);
        return 0;
    }

    /* 内部定义符号：段基址 + 符号偏移 */
    if (sym->st_shndx < elf->shnum) {
        uint32_t base = elf->shdrs[sym->st_shndx].sh_addr;
        uint32_t addr = base + sym->st_value;
        if (verbose) {
            printf("  [local] %s (sec#%d) = 0x%x + 0x%x = 0x%x\n",
                   name ? name : "?", sym->st_shndx, (unsigned)base, (unsigned)sym->st_value, (unsigned)addr);
        }
        return addr;
    }

    return 0;
}

/* ═══════════════════════════════════════════
 *  重定位应用
 * ═══════════════════════════════════════════ */

int mars_reloc_apply(const mars_elf_t* elf,
                     mars_sym_resolver_t resolver,
                     void* resolver_ctx,
                     bool verbose)
{
    if (!elf || !elf->data) return -1;

    int rc = 0;

    for (uint32_t i = 0; i < elf->shnum; i++) {
        const mars_elf32_shdr_t* sh = &elf->shdrs[i];
        if (sh->sh_type != SHT_RELA) continue;

        const char* sec_name = mars_elf_section_name(elf, sh->sh_name);
        uint32_t target_idx = sh->sh_info;   /* 重定位目标 section */
        if (target_idx >= elf->shnum) continue;

        const mars_elf32_shdr_t* target = &elf->shdrs[target_idx];
        const char* target_name = mars_elf_section_name(elf, target->sh_name);

        /* 目标 section 的加载地址 */
        uint32_t target_base = target->sh_addr;
        if (target_base == 0) {
            /* section 未加载（如 .xt.prop），跳过 */
            if (verbose) {
                printf("[reloc] skip %s (not loaded)\n", target_name);
            }
            continue;
        }

        uint32_t n = sh->sh_size / sizeof(mars_elf32_rela_t);
        const mars_elf32_rela_t* relas =
            (const mars_elf32_rela_t*)(elf->data + sh->sh_offset);

        if (verbose) {
            printf("[reloc] %s -> %s (%u entries)\n", sec_name, target_name, (unsigned)n);
        }

        for (uint32_t j = 0; j < n; j++) {
            const mars_elf32_rela_t* rela = &relas[j];
            uint32_t r_type = ELF32_R_TYPE(rela->r_info);
            uint32_t r_sym  = ELF32_R_SYM(rela->r_info);
            uint32_t place  = target_base + rela->r_offset;

            /* 目标位置必须在可写内存（RAM）中 */
            if (place == 0) continue;

            switch (r_type) {
            case R_XTENSA_NONE:
                break;

            case R_XTENSA_32:
            case R_XTENSA_GLOB_DAT:
            case R_XTENSA_RELATIVE:
            case R_XTENSA_JMP_SLOT: {
                if (r_sym >= elf->symtab_count) { rc = -2; break; }
                const mars_elf32_sym_t* sym = &elf->symtab[r_sym];
                uint32_t s = resolve_symbol(elf, resolver, resolver_ctx, sym, verbose);
                if (s == 0 && ELF32_ST_TYPE(sym->st_info) != STT_SECTION) {
                    /* 允许 section 符号为 0（如空段） */
                    printf("[reloc] FAIL: cannot resolve %s\n",
                           mars_elf_symbol_name(elf, sym->st_name));
                    rc = -3;
                    break;
                }
                /* Xtensa R_XTENSA_32 is partial-inplace even in SHT_RELA:
                 * GCC stores the section-local string/function offset in the
                 * target word, with a zero explicit addend. Preserve both. */
                uint32_t implicit_addend = 0;
                if (r_type == R_XTENSA_32) {
                    for (unsigned b = 0; b < 4; ++b) {
                        implicit_addend |= (uint32_t)exec_read_u8(
                            (const void *)(uintptr_t)(place + b)) << (b * 8);
                    }
                }
                uint32_t value = s + (uint32_t)rela->r_addend + implicit_addend;
                /* 小端写入 32 位地址（兼容 32-bit-only IRAM） */
                exec_write_u32((void*)(uintptr_t)place, value);
                if (verbose) {
                    printf("  [%2u] R_XTENSA_32 @0x%x = 0x%x (sym %s addend %d)\n",
                           (unsigned)j, (unsigned)place, (unsigned)value,
                           mars_elf_symbol_name(elf, sym->st_name),
                           (int)rela->r_addend);
                }
                break;
            }

            case R_XTENSA_PLT:
            case R_XTENSA_PLT_ACTUAL: {
                if (r_sym >= elf->symtab_count) { rc = -4; break; }
                const mars_elf32_sym_t* sym = &elf->symtab[r_sym];
                uint32_t s = resolve_symbol(elf, resolver, resolver_ctx, sym, verbose);
                if (s == 0) {
                    printf("[reloc] FAIL: cannot resolve PLT %s\n",
                           mars_elf_symbol_name(elf, sym->st_name));
                    rc = -5;
                    break;
                }
                uint32_t value = s + (uint32_t)rela->r_addend;
                exec_write_u32((void*)(uintptr_t)place, value);
                if (verbose) {
                    printf("  [%2u] R_XTENSA_PLT @0x%x = 0x%x (%s)\n",
                           (unsigned)j, (unsigned)place, (unsigned)value,
                           mars_elf_symbol_name(elf, sym->st_name));
                }
                break;
            }

            case R_XTENSA_SLOT0_OP: {
                if (r_sym >= elf->symtab_count) { rc = -6; break; }
                const mars_elf32_sym_t* sym = &elf->symtab[r_sym];

                /* SLOT0_OP also marks section-local branches (j/bbci/bltu...).
                 * Moving the whole section preserves those relative offsets, so
                 * they must stay byte-for-byte unchanged.  The old loader treated
                 * every SLOT0_OP as L32R and corrupted the loop-back jump. */
                const uint8_t opcode = exec_read_u8((const void*)(uintptr_t)place) & 0x0f;
                if (opcode != 0x01) {
                    if (sym->st_shndx == target_idx) {
                        if (verbose) {
                            printf("  [%2u] keep section-local SLOT0_OP @0x%x\n",
                                   (unsigned)j, (unsigned)place);
                        }
                        break;
                    }
                    printf("[reloc] FAIL: unsupported cross-section SLOT0_OP @0x%x\n",
                           (unsigned)place);
                    rc = -8;
                    break;
                }

                /* L32R: target = symbol address + addend (usually literal pool). */
                uint32_t s = resolve_symbol(elf, resolver, resolver_ctx, sym, verbose);
                if (s == 0) {
                    printf("[reloc] FAIL: cannot resolve SLOT0 %s\n",
                           mars_elf_symbol_name(elf, sym->st_name));
                    rc = -7;
                    break;
                }
                uint32_t target_addr = s + (uint32_t)rela->r_addend;
                /* L32R 指令地址 = place（r_offset 是段内偏移，已加段基址） */
                uint32_t pc = place;
                uint32_t pc_aligned = (pc + 3) & ~3u;
                int32_t diff = (int32_t)(target_addr - pc_aligned);
                int16_t imm16 = (int16_t)(diff >> 2);

                uint8_t* insn = (uint8_t*)(uintptr_t)place;
                uint8_t reg = mars_reloc_read_l32r_reg(insn);
                mars_reloc_write_l32r_imm(insn, reg, imm16);
                if (verbose) {
                    printf("  [%2u] L32R @0x%x -> 0x%x (pc_a=0x%x imm=%d)\n",
                           (unsigned)j, (unsigned)place, (unsigned)target_addr, (unsigned)pc_aligned, (int)imm16);
                }
                break;
            }

            default:
                /* 忽略链接器元数据（.xt.lit / .xt.prop / ASM_EXPAND 等） */
                if (verbose) {
                    printf("  [%2u] ignore reloc type %u\n", (unsigned)j, (unsigned)r_type);
                }
                break;
            }

            if (rc != 0) break;
        }
        if (rc != 0) break;
    }

    return rc;
}
