/**
 * @file mars_reloc.h
 * @brief XTensa 重定位器
 */

#pragma once

#include "mars_elf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 符号解析回调：由加载器实现
 * @param name 符号名
 * @param ctx  上下文
 * @return 符号地址，0 表示未找到
 */
typedef uint32_t (*mars_sym_resolver_t)(const char* name, void* ctx);

/**
 * 对 ELF 执行重定位
 *
 * @param elf        已解析的 ELF
 * @param resolver   外部符号解析回调（NULL 表示所有符号都在内部）
 * @param resolver_ctx 回调上下文
 * @param verbose    是否打印详细日志
 * @return 0 成功，负值错误码
 */
int mars_reloc_apply(const mars_elf_t* elf,
                     mars_sym_resolver_t resolver,
                     void* resolver_ctx,
                     bool verbose);

/**
 * 把 L32R 指令的立即数写入指令
 * @param insn 指向 3 字节指令的指针（已解析出寄存器）
 * @param reg   目标寄存器（保持不变）
 * @param imm16 立即数
 */
void mars_reloc_write_l32r_imm(uint8_t* insn, uint8_t reg, int16_t imm16);

/**
 * 从指令读取 L32R 的目标寄存器
 */
uint8_t mars_reloc_read_l32r_reg(const uint8_t* insn);

#ifdef __cplusplus
}
#endif
