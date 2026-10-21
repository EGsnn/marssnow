#!/usr/bin/env python3
"""Build the Mars shared-assets package without app-owned resources.

The current product keeps the desktop wallpaper and speech-recognition model in
the system assets partition. XiaoZhi EAF animations live in its MARSAPP resource
archive, while fonts are provided by the system firmware.
"""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


HEADER = struct.Struct("<III")
ENTRY = struct.Struct("<32sIIHH")
DEFAULT_KEEP = ("srmodels.bin", "mars_landscape.rgb565.bin")


def parse_package(path: Path) -> list[tuple[str, bytes, int, int]]:
    if path.is_dir():
        manifest = json.loads((path / "manifest.json").read_text())
        items = []
        for item in manifest["files"]:
            name = item["name"]
            if Path(name).name != name:
                raise ValueError(f"invalid shared asset filename: {name}")
            items.append((name, (path / name).read_bytes(), item["width"], item["height"]))
        return items
    package = path.read_bytes()
    if len(package) < HEADER.size:
        raise ValueError("assets package is shorter than its header")
    count, checksum, stored_size = HEADER.unpack_from(package)
    body = package[HEADER.size :]
    if len(body) != stored_size or sum(body) & 0xFFFF != checksum:
        raise ValueError("assets package length or checksum is invalid")
    table_size = count * ENTRY.size
    if count == 0 or table_size > len(body):
        raise ValueError("assets package table is invalid")
    table, data = body[:table_size], body[table_size:]
    result = []
    for index in range(count):
        raw_name, size, offset, width, height = ENTRY.unpack_from(table, index * ENTRY.size)
        name = raw_name.split(b"\0", 1)[0].decode("utf-8")
        if offset > len(data) or size + 2 > len(data) - offset:
            raise ValueError(f"asset {name!r} is outside the package")
        if data[offset : offset + 2] != b"\x5a\x5a":
            raise ValueError(f"asset {name!r} has no data marker")
        result.append((name, data[offset + 2 : offset + 2 + size], width, height))
    return result


def build_package(items: list[tuple[str, bytes, int, int]]) -> bytes:
    table = bytearray()
    data = bytearray()
    for name, content, width, height in items:
        encoded = name.encode("utf-8")
        if len(encoded) >= 32:
            raise ValueError(f"asset name is too long: {name}")
        table += ENTRY.pack(encoded.ljust(32, b"\0"), len(content), len(data), width, height)
        data += b"\x5a\x5a" + content
    body = bytes(table + data)
    return HEADER.pack(len(items), sum(body) & 0xFFFF, len(body)) + body


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--keep", action="append", default=[])
    parser.add_argument("--max-size", type=lambda value: int(value, 0), default=0x280000)
    args = parser.parse_args()

    keep = tuple(args.keep) or DEFAULT_KEEP
    by_name = {item[0]: item for item in parse_package(args.source)}
    missing = [name for name in keep if name not in by_name]
    if missing:
        raise ValueError(f"missing source assets: {', '.join(missing)}")
    result = build_package([by_name[name] for name in keep])
    if len(result) > args.max_size:
        raise ValueError(f"shared assets {len(result)} exceeds partition {args.max_size}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(result)
    print(f"shared assets: {len(result)} bytes, {len(keep)} files")
    for name in keep:
        print(f"  {name}: {len(by_name[name][1])} bytes")


if __name__ == "__main__":
    main()
