#!/usr/bin/env python3
"""Build one supported Mars community board; never flash a device."""
import argparse, hashlib, json, os, shutil, subprocess, sys
from pathlib import Path
from board_profiles import ROOT, load, partition_layout


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("board", choices=("alientek-atk-box", "waveshare-esp32-s3-touch-lcd-1-46"))
    a = p.parse_args()
    if not shutil.which("idf.py") or not os.environ.get("IDF_PATH"):
        p.error("Activate ESP-IDF v6.1 first: . /path/to/esp-idf/export.sh")
    project = ROOT / "firmware/mars_system"
    out = ROOT / "build" / a.board
    out.mkdir(parents=True, exist_ok=True)
    fragment = out / "board.defaults"
    fragment.write_text("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y\nCONFIG_COMPILER_OPTIMIZATION_PERF=y\nCONFIG_FREERTOS_HZ=1000\n" if a.board.startswith("waveshare-") else "")
    env = dict(os.environ, IDF_COMPONENT_MANAGER="0")
    subprocess.run(["idf.py", "-C", str(project), "-B", str(out),
        "-D", "IDF_TARGET=esp32s3", "-D", f"SDKCONFIG={out / 'sdkconfig'}",
        "-D", f"SDKCONFIG_DEFAULTS={project / 'sdkconfig.defaults'};{fragment}",
        "-D", f"MARS_BOARD={a.board}", "build"], check=True, env=env)
    subprocess.run([sys.executable, str(project / "tools/build_shared_assets.py"),
        "--source", str(project / "resources/shared_assets"),
        "--output", str(out / "mars_shared_assets.bin"),
        "--keep", "mars_landscape.rgb565.bin"], check=True)
    binary = out / "box3_launcher.bin"
    parts = partition_layout(load(a.board), (out / "partition_table/partition-table.bin").read_bytes())
    target = next(part for part in parts if part["name"] == "ota_0")
    if binary.stat().st_size > target["size"]:
        raise ValueError("Firmware exceeds system partition")
    files = [binary, out / "bootloader/bootloader.bin", out / "partition_table/partition-table.bin", out / "mars_shared_assets.bin"]
    manifest = {"board": a.board, "hardwareVerified": False, "partitions": parts,
        "files": [{"path": f.name, "bytes": f.stat().st_size, "sha256": hashlib.sha256(f.read_bytes()).hexdigest()} for f in files]}
    (out / "build-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"Built {a.board}: {binary.stat().st_size} bytes; no device was flashed.")

if __name__ == "__main__":
    main()
