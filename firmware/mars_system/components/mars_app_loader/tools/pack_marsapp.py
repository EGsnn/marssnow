#!/usr/bin/env python3
"""
MARSAPP v2 打包器：把 ELF（ET_REL .o）包装成 MARSAPP v2 应用包。

可选资源尾包（flags bit0）：
    [32-byte MARSRES header][48-byte entries][4-byte aligned file data]

资源尾包保持 v2 主包头和 ELF CRC 约定不变，因此旧包仍可被新
加载器读取。尾包自带总 CRC，每个文件也带 CRC，运行时直接从应用
分区 mmap，不把表情整体复制到 RAM。

包头 96 字节（与 mars_app_loader.c marsapp2_header_t 完全一致）：
    magic[8]="MARSAPP" version[2]=2 flags[2]
    elf_offset[4] elf_size[4] stack_size[4] heap_size[4]
    app_name[32] entry_symbol[32] crc32[4]
CRC32 覆盖 96 字节头中除 crc32 外的 92 字节 + ELF 数据（标准 CRC-32/ISO-HDLC）。

用法：
    python3 pack_marsapp.py <elf.o> <out.marsapp> \
        --name test_app --entry app_entry \
        [--stack 8192] [--heap 65536] [--assets path/to/assets]
"""

import argparse
import binascii
import struct
from pathlib import Path

MAGIC = b"MARSAPP"
MAGIC_LEN = 8
HEADER_SIZE = 96
VERSION = 2
FLAG_RESOURCES = 1 << 0

RESOURCE_MAGIC = b"MARSRES\x00"
RESOURCE_VERSION = 1
RESOURCE_HEADER_SIZE = 32
RESOURCE_ENTRY_SIZE = 48
RESOURCE_NAME_SIZE = 32
ICON_BYTES = 48 * 48 * 2


def align4(value: int) -> int:
    return (value + 3) & ~3


def pad32(name: str, size: int) -> bytes:
    b = name.encode("utf-8")
    if len(b) >= size:
        raise ValueError(f"'{name}' 必须短于 {size} 字节")
    return b + b"\x00" * (size - len(b))


def build_resources(asset_dir: Path | None, display_name: str | None = None,
                    icon_path: Path | None = None) -> tuple[bytes, list[tuple[str, int]]]:
    files = {}
    if asset_dir is not None:
        if not asset_dir.is_dir():
            raise ValueError(f"资源目录不存在: {asset_dir}")
        files = {p.name: p.read_bytes() for p in asset_dir.iterdir()
                 if p.is_file() and not p.name.startswith(".")}
    if display_name is not None or icon_path is not None:
        if not display_name or icon_path is None:
            raise ValueError("显示名称和图标必须一起提供")
        name = display_name.encode("utf-8")
        if not 0 < len(name) <= 63 or b"\x00" in name:
            raise ValueError("显示名称必须为 1～63 字节的 UTF-8 文本")
        icon = icon_path.read_bytes()
        if len(icon) != ICON_BYTES:
            raise ValueError(f"图标必须是 48×48 RGB565LE，共 {ICON_BYTES} 字节")
        if "appmeta.bin" in files:
            raise ValueError("资源目录已有 appmeta.bin")
        files["appmeta.bin"] = struct.pack("<8sHHHH64s", b"MARSMETA", 1, len(name), 48, 48,
                                            name.ljust(64, b"\x00")) + icon
    if not files:
        raise ValueError("资源目录为空")
    if len(files) > 64:
        raise ValueError("资源数量超过 64")

    table_size = len(files) * RESOURCE_ENTRY_SIZE
    data_offset = RESOURCE_HEADER_SIZE + table_size
    table = bytearray()
    payload = bytearray()
    report: list[tuple[str, int]] = []
    names: set[str] = set()

    for filename, data in sorted(files.items()):
        if filename in names:
            raise ValueError(f"重复资源名: {filename}")
        names.add(filename)
        name = pad32(filename, RESOURCE_NAME_SIZE)
        if not data:
            raise ValueError(f"资源为空: {filename}")
        while (data_offset + len(payload)) & 3:
            payload.append(0)
        offset = data_offset + len(payload)
        file_crc = binascii.crc32(data) & 0xFFFFFFFF
        table += struct.pack("<32sIIII", name, offset, len(data), file_crc, 0)
        payload += data
        report.append((filename, len(data)))

    total_size = RESOURCE_HEADER_SIZE + len(table) + len(payload)
    header = struct.pack(
        "<8sHHIIIII",
        RESOURCE_MAGIC,
        RESOURCE_VERSION,
        RESOURCE_ENTRY_SIZE,
        len(files),
        len(table),
        len(payload),
        total_size,
        0,
    )
    assert len(header) == RESOURCE_HEADER_SIZE
    archive_crc = binascii.crc32(header[:28] + table + payload) & 0xFFFFFFFF
    header = header[:28] + struct.pack("<I", archive_crc)
    return bytes(header + table + payload), report


def main():
    ap = argparse.ArgumentParser(description="MARSAPP v2 打包器")
    ap.add_argument("elf", type=Path, help="应用 ELF（ET_REL .o）")
    ap.add_argument("output", type=Path, help="输出 MARSAPP v2 包")
    ap.add_argument("--name", required=True, help="应用名（<=31 字节）")
    ap.add_argument("--entry", required=True, help="入口符号名（<=31 字节）")
    ap.add_argument("--stack", type=int, default=8192, help="应用栈大小（字节）")
    ap.add_argument("--heap", type=int, default=65536, help="应用堆大小（字节）")
    ap.add_argument("--assets", type=Path, help="可选的应用资源目录")
    ap.add_argument("--display-name", help="设备应用列表显示名称（需同时给图标）")
    ap.add_argument("--icon-rgb565", type=Path, help="48×48 RGB565 小端图标")
    args = ap.parse_args()

    elf_data = args.elf.read_bytes()
    if not elf_data.startswith(b"\x7fELF"):
        raise SystemExit(f"错误：{args.elf} 不是 ELF 文件")

    app_name = pad32(args.name, 32)
    entry = pad32(args.entry, 32)

    resources = b""
    resource_report: list[tuple[str, int]] = []
    flags = 0
    if args.assets is not None or args.display_name is not None or args.icon_rgb565 is not None:
        resources, resource_report = build_resources(args.assets, args.display_name, args.icon_rgb565)
        flags |= FLAG_RESOURCES

    # 包头（crc32 暂填 0）
    header = struct.pack(
        "<8sHHIIII32s32sI",
        MAGIC, VERSION, flags,
        HEADER_SIZE, len(elf_data),
        args.stack, args.heap,
        app_name, entry,
        0,
    )
    assert len(header) == HEADER_SIZE, f"包头长度异常: {len(header)}"

    # CRC32：header[0:92] + elf
    crc = binascii.crc32(header[:92] + elf_data) & 0xFFFFFFFF
    header = header[:92] + struct.pack("<I", crc)

    out = header + elf_data + resources
    args.output.write_bytes(out)

    print(f"✅ MARSAPP v2 包: {args.output}")
    print(f"   应用名: {args.name}  入口: {args.entry}")
    print(f"   ELF: {len(elf_data)} B  包: {len(out)} B")
    if resource_report:
        print(f"   资源: {len(resources)} B / {len(resource_report)} 个文件")
        for resource_name, resource_size in resource_report:
            print(f"     - {resource_name}: {resource_size} B")
    print(f"   栈: {args.stack} B  堆: {args.heap} B  CRC32: 0x{crc:08x}")
    if len(out) > 1536 * 1024:
        print("⚠️  超过 1.5 MiB 应用位容量！")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
