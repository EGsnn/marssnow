#!/usr/bin/env python3
"""
MARSAPP v2 离线全链路验证器
模拟 mars_app_loader 的完整流程（不烧真机）：
  1. 解析 MARSAPP v2 包头 + CRC 校验
  2. 解析 ELF（ET_REL, Xtensa, 小端）：段表/符号表/重定位表
  3. 布局段（模拟：代码区/数据区分配，地址连续性）
  4. 重定位应用：
     - 内部符号（STT_SECTION / 已定义符号）
     - 系统符号（对照符号表定义源 mars_exports_def.c 的名称清单）

用法：
  python3 verify_package.py <app.marsapp> <symtab_def.c> [--verbose]
  （symtab_def.c = mars_exports_def.c，唯一事实来源；旧 mars_symtab.c 已废弃）
"""

import argparse
import binascii
import re
import struct
import sys
from pathlib import Path

FLAG_RESOURCES = 1 << 0
RESOURCE_MAGIC = b"MARSRES\x00"
RESOURCE_HEADER_SIZE = 32
RESOURCE_ENTRY_SIZE = 48

# ELF 常量
ET_REL = 1
EM_XTENSA = 94
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHN_UNDEF = 0
STT_NOTYPE = 0
STT_OBJECT = 1
STT_FUNC = 2
STT_SECTION = 3

R_XTENSA_NONE = 0
R_XTENSA_32 = 1
R_XTENSA_ASM_EXPAND = 11
R_XTENSA_SLOT0_OP = 20
R_XTENSA_GLOB_DAT = 35
R_XTENSA_JMP_SLOT = 36
R_XTENSA_RELATIVE = 37
R_XTENSA_PLT = 38
R_XTENSA_PLT_ACTUAL = 6

SUPPORTED_RELOC = {R_XTENSA_32, R_XTENSA_GLOB_DAT, R_XTENSA_RELATIVE,
                   R_XTENSA_JMP_SLOT, R_XTENSA_PLT, R_XTENSA_PLT_ACTUAL,
                   R_XTENSA_SLOT0_OP, R_XTENSA_NONE}
IGNORED_RELOC = {R_XTENSA_ASM_EXPAND}


def load_symtab(c_path: Path):
    """从符号表定义源提取符号名列表。

    事实来源 = mars_exports_def.c（MARS_EXP_ENTRY(sym) 聚合数组，
    地址由链接器在最终链接时填充）。兼容解析已宏展开的
    { "name", (uint32_t)(uintptr_t)(&(name)) } 形式。
    返回 {name: None}（地址一律不在此校验）。
    """
    txt = c_path.read_text(encoding="utf-8")
    syms = {}
    # 1) 主格式：MARS_EXP_ENTRY(sym)
    for m in re.finditer(r'MARS_EXP_ENTRY\(([a-zA-Z_][a-zA-Z0-9_]*)\)', txt):
        syms[m.group(1)] = None
    # 2) 兼容已展开条目：{ "name", (uint32_t)... } 或 { "name", 0x...u }
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(?:0x[0-9a-fA-F]+u?|\(uint32_t\)[^}]*?)\s*\}', txt):
        syms.setdefault(m.group(1), None)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg", type=Path, help="MARSAPP v2 包")
    ap.add_argument("symtab", type=Path, help="符号表定义源（mars_exports_def.c）")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    d = open(args.pkg, "rb").read()
    errors = []

    # ── 1. 包头 + CRC ──
    magic, ver, flags, eoff, esize, stack, heap = struct.unpack_from("<8sHHIIII", d, 0)
    name = d[28:60].split(b"\x00")[0].decode()
    entry = d[60:92].split(b"\x00")[0].decode()
    crc_stored = struct.unpack_from("<I", d, 92)[0]
    crc_calc = binascii.crc32(d[:92] + d[eoff:eoff+esize]) & 0xFFFFFFFF
    if magic != b"MARSAPP\x00":
        errors.append("包头 magic 错误")
    if ver != 2:
        errors.append(f"版本 {ver} != 2")
    if crc_stored != crc_calc:
        errors.append("CRC 不匹配")
    if eoff + esize > len(d):
        errors.append("ELF 越界")
    print(f"[1] 包头: name={name} entry={entry} stack={stack} heap={heap} CRC={'OK' if crc_stored==crc_calc else 'FAIL'}")

    # 可选资源尾包。主包 CRC 只覆盖 ELF，资源由尾包和单文件 CRC 保护。
    known_flags = FLAG_RESOURCES
    if flags & ~known_flags:
        errors.append(f"不支持的 flags: 0x{flags:04x}")
    if flags & FLAG_RESOURCES:
        roff = eoff + esize
        if roff + RESOURCE_HEADER_SIZE > len(d):
            errors.append("资源包头越界")
        else:
            rmagic, rver, entry_size, count, table_size, data_size, total_size, rcrc = \
                struct.unpack_from("<8sHHIIIII", d, roff)
            if rmagic != RESOURCE_MAGIC or rver != 1:
                errors.append("资源包 magic/version 错误")
            if entry_size != RESOURCE_ENTRY_SIZE or table_size != count * entry_size:
                errors.append("资源表尺寸错误")
            if count == 0 or count > 64:
                errors.append(f"资源数量非法: {count}")
            if total_size != RESOURCE_HEADER_SIZE + table_size + data_size or roff + total_size != len(d):
                errors.append("资源包总长度错误")
            rcalc = binascii.crc32(d[roff:roff+28] + d[roff+32:roff+total_size]) & 0xFFFFFFFF
            if rcrc != rcalc:
                errors.append("资源包 CRC 不匹配")
            names = set()
            for i in range(count):
                ent = roff + RESOURCE_HEADER_SIZE + i * entry_size
                raw_name, offset, size, file_crc, resource_flags = struct.unpack_from("<32sIIII", d, ent)
                resource_name = raw_name.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
                if not resource_name or resource_name in names:
                    errors.append(f"资源名非法或重复: {resource_name!r}")
                names.add(resource_name)
                if resource_flags != 0 or offset < RESOURCE_HEADER_SIZE + table_size or offset + size > total_size:
                    errors.append(f"资源条目越界: {resource_name}")
                    continue
                payload = d[roff+offset:roff+offset+size]
                if (binascii.crc32(payload) & 0xFFFFFFFF) != file_crc:
                    errors.append(f"资源 CRC 不匹配: {resource_name}")
            print(f"    资源尾包: files={count} bytes={total_size} CRC={'OK' if rcrc==rcalc else 'FAIL'}")
    elif len(d) != eoff + esize:
        errors.append("无资源标志但包尾仍有数据")

    # ── 2. ELF 解析 ──
    e = d[eoff:eoff+esize]
    if e[:4] != b"\x7fELF":
        errors.append("ELF 魔数错误")
    e_type, e_machine = struct.unpack_from("<HH", e, 16)
    e_shoff = struct.unpack_from("<I", e, 32)[0]
    e_shnum = struct.unpack_from("<H", e, 48)[0]
    e_shstrndx = struct.unpack_from("<H", e, 50)[0]
    if e_type != ET_REL:
        errors.append(f"e_type {e_type} != ET_REL(1)")
    if e_machine != EM_XTENSA:
        errors.append(f"machine {e_machine} != Xtensa(94)")
    if e_shoff == 0 or e_shnum == 0:
        errors.append("无 section 表")
    print(f"[2] ELF: type=REL machine=Xtensa sections={e_shnum}")

    shdrs = []
    for i in range(e_shnum):
        off = e_shoff + i * 40
        sh = struct.unpack_from("<IIIIIIIIII", e, off)
        shdrs.append(sh)

    shstr_off = shdrs[e_shstrndx][4]
    shstr_size = shdrs[e_shstrndx][5]
    shstrtab = e[shstr_off:shstr_off + shstr_size]

    def sec_name(i):
        sh = shdrs[i]
        return shstrtab[sh[0]:].split(b"\x00")[0].decode()

    strtabs = {}
    symtab = []          # (name_data_index 由 link 决定)
    sym_link = None
    relas = {}
    for i, sh in enumerate(shdrs):
        t = sh[1]
        if t == SHT_STRTAB:
            strtabs[i] = e[sh[4]:sh[4]+sh[5]]
        elif t == SHT_SYMTAB:
            sym_link = sh[6]  # sh_link 指向符号字符串表
            nsym = sh[5] // 16
            for j in range(nsym):
                so = sh[4] + j * 16
                symtab.append(struct.unpack_from("<IIIBBH", e, so))
        elif t == SHT_RELA:
            nrela = sh[5] // 12
            rs = []
            for j in range(nrela):
                ro = sh[4] + j * 12
                rs.append(struct.unpack_from("<IiI", e, ro))
            relas[i] = (sh[7], rs)  # sh_info=目标段, 重定位项

    sym_strtab = strtabs.get(sym_link, b"")

    def sym_name(st):
        return sym_strtab[st[0]:].split(b"\x00")[0].decode() if st[0] < len(sym_strtab) else "?"

    print(f"[3] 符号: {len(symtab)} 重定位段: {len(relas)}")

    # ── 3. 布局段（模拟加载器分配）──
    code_addr = 0x42000000  # PSRAM 可执行基址（模拟）
    data_addr = 0x3f000000  # PSRAM 数据基址（模拟）
    sec_addrs = {}
    for i, sh in enumerate(shdrs):
        n = sec_name(i)
        alignment = sh[8] or 1
        if sh[1] == SHT_NOBITS and n.startswith((".bss", ".data")):
            data_addr = (data_addr + alignment - 1) & ~(alignment - 1)
            sec_addrs[i] = data_addr
            data_addr += sh[5]
        elif sh[1] == SHT_PROGBITS and n.startswith((".text", ".literal", ".init_array")):
            alignment = max(4, alignment)
            code_addr = (code_addr + alignment - 1) & ~(alignment - 1)
            sec_addrs[i] = code_addr
            code_addr += (sh[5] + 3) & ~3
        elif sh[1] == SHT_PROGBITS and n.startswith((".data", ".rodata")):
            data_addr = (data_addr + alignment - 1) & ~(alignment - 1)
            sec_addrs[i] = data_addr
            data_addr += sh[5]
    print(f"[4] 布局: code={code_addr - 0x42000000} B data={data_addr - 0x3f000000} B")

    # ── 4. 符号解析 + 重定位 ──
    symtab_map = load_symtab(args.symtab)
    print(f"[5] 系统符号表: {len(symtab_map)} 条")

    unresolved = set()
    bad_types = set()
    resolved_extern = 0
    for target_idx, (_, rs) in relas.items():
        target_name = sec_name(target_idx)
        for r_offset, r_info, r_addend in rs:
            r_type = r_info & 0xff
            r_sym = r_info >> 8
            if r_type not in SUPPORTED_RELOC and r_type not in IGNORED_RELOC:
                bad_types.add(r_type)
                continue
            if r_type in (R_XTENSA_NONE, R_XTENSA_ASM_EXPAND):
                continue
            if r_sym >= len(symtab):
                unresolved.add(f"sym#{r_sym}@{target_name}")
                continue
            st = symtab[r_sym]
            sname = sym_name(st)
            st_type = st[3] & 0xf
            if st[5] == SHN_UNDEF:  # 外部符号 → 系统符号表
                if sname in symtab_map:
                    resolved_extern += 1
                else:
                    unresolved.add(sname)
            elif st_type == STT_SECTION:
                if st[5] in sec_addrs:
                    resolved_extern += 1
            else:
                if st[5] in sec_addrs:
                    resolved_extern += 1
                else:
                    unresolved.add(sname)

    print(f"[6] 重定位: 外部符号已解析={resolved_extern} 未解析={len(unresolved)} 不支持类型={len(bad_types)}")
    if unresolved:
        print("   未解析符号:", sorted(unresolved))
        errors.append(f"{len(unresolved)} 个未解析符号")
    if bad_types:
        print("   不支持重定位类型:", sorted(bad_types))
        errors.append(f"不支持的重定位类型: {sorted(bad_types)}")

    # ── 5. 入口符号 ──
    entry_found = False
    for st in symtab:
        if (st[3] & 0xf) == STT_FUNC and st[5] != SHN_UNDEF:
            if sym_name(st) == entry:
                entry_found = True
                break
    if not entry_found:
        errors.append(f"入口符号 '{entry}' 未找到")
    print(f"[7] 入口 '{entry}': {'找到' if entry_found else '缺失'}")

    print()
    if errors:
        print("❌ 离线验证失败:")
        for er in errors:
            print("  -", er)
        sys.exit(1)
    print("✅ 离线验证全部通过：包结构 / CRC / ELF / 段布局 / 动态链接符号解析 / 入口 均符合加载器约定")


if __name__ == "__main__":
    main()
