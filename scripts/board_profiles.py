"""Board registry shared by builds, firmware descriptors and host validation."""
import json
import re
import struct
from pathlib import Path
from ui_profiles import resolve as resolve_ui, c_values as ui_c_values

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "firmware" / "boards"
CHIPS = {"esp32s3": "xtensa", "esp32c3": "riscv"}


def validate(profile):
    """Reject unsafe combinations before configuration or artifact creation."""
    if profile["schemaVersion"] != 1:
        raise ValueError("unsupported board schema")
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", profile["id"]):
        raise ValueError("invalid board id")
    if CHIPS.get(profile["chip"]) != profile["architecture"]:
        raise ValueError("unsupported chip or architecture mismatch")
    if profile["flashBytes"] not in (4 << 20, 8 << 20, 16 << 20, 32 << 20):
        raise ValueError("unsupported flash size")
    if type(profile["psramBytes"]) is not int or profile["psramBytes"] < 0:
        raise ValueError("invalid PSRAM size")
    if profile["chip"] == "esp32c3" and profile["psramBytes"]:
        raise ValueError("C3 profile cannot declare external PSRAM")
    if profile["radio"]["classicBluetooth"]:
        raise ValueError("these chip targets do not support classic Bluetooth")
    display = profile["display"]
    if not 1 <= display["width"] <= 1024 or not 1 <= display["height"] <= 1024:
        raise ValueError("invalid display dimensions")
    if display["colorDepth"] != 16 or display["bus"] not in ("spi", "i80", "qspi"):
        raise ValueError("unsupported panel format")
    if display["touch"] != bool(profile["input"]["touchController"]):
        raise ValueError("touch capability/controller mismatch")
    if profile["chip"] == "esp32c3" and display["bus"] == "i80":
        raise ValueError("C3 has no I80 LCD peripheral")
    required = {"i2c_sda", "i2c_scl", "lcd_cs", "lcd_dc", "key",
                "i2s_mclk", "i2s_bclk", "i2s_ws", "i2s_dout", "i2s_din"}
    if display["bus"] == "qspi":
        required |= {"lcd_sclk", "lcd_backlight", *(f"lcd_d{i}" for i in range(4)),
                     "power_enable", "power_key", "touch_int", "mic_bclk", "mic_ws"}
    else:
        required |= ({"lcd_mosi", "lcd_sclk", "lcd_backlight"} if display["bus"] == "spi"
                     else {"lcd_wr", "lcd_rd", *(f"lcd_d{i}" for i in range(8))})
    viewport = display.get("viewport", {"x": 0, "y": 0, "width": display["width"], "height": display["height"]})
    if any(type(viewport[k]) is not int for k in ("x", "y", "width", "height")) or (
            viewport["x"] < 0 or viewport["y"] < 0 or viewport["width"] <= 0 or viewport["height"] <= 0
            or viewport["x"] + viewport["width"] > display["width"]
            or viewport["y"] + viewport["height"] > display["height"]):
        raise ValueError("invalid display viewport")
    if display.get("shape") == "round":
        if display["width"] != display["height"]:
            raise ValueError("round display must be square")
        radius = display["width"] / 2
        for x in (viewport["x"], viewport["x"] + viewport["width"] - 1):
            for y in (viewport["y"], viewport["y"] + viewport["height"] - 1):
                if (x - radius) ** 2 + (y - radius) ** 2 > radius ** 2:
                    raise ValueError("viewport corners outside round display")
    if display["bus"] == "qspi":
        if (profile["chip"] != "esp32s3" or display["controller"] != "spd2010"
                or display["width"] % 4 or profile["psramBytes"] < (8 << 20)):
            raise ValueError("SPD2010 QSPI requires S3, aligned width and PSRAM")
        expander = profile.get("expander", {})
        bits = [expander.get("touchResetBit"), expander.get("displayResetBit")]
        if (expander.get("controller") != "tca9554"
                or not 1 <= expander.get("address", 0) < 127
                or any(type(bit) is not int or not 0 <= bit < 8 for bit in bits)
                or bits[0] == bits[1] or not 1 <= profile["input"].get("touchAddress", 0) < 127):
            raise ValueError("invalid expander reset or touch address")
    if profile["audio"]["codec"] == "es8311" and not 1 <= profile["audio"].get("address", 0) < 127:
        raise ValueError("invalid ES8311 I2C address")
    if profile["audio"]["codec"] not in ("es8311", "pcm5101"):
        raise ValueError("unsupported audio backend")
    if not required <= profile["pins"].keys():
        raise ValueError("missing required pin")
    gpio_limit = 21 if profile["chip"] == "esp32c3" else 48
    for name, pin in profile["pins"].items():
        if not re.fullmatch(r"[a-z][a-z0-9_]*", name) or type(pin) is not int or not -1 <= pin <= gpio_limit:
            raise ValueError(f"invalid pin: {name}")
    used = [pin for pin in profile["pins"].values() if pin >= 0]
    if len(used) != len(set(used)):
        raise ValueError("conflicting pin assignments")
    windows = profile["input"].get("adcWindowsMv", [])
    if profile["input"]["type"] == "adc-ladder":
        if profile["pins"]["key"] != 0:
            raise ValueError("current ADC ladder driver requires GPIO0 / ADC1 channel 0")
        if len(windows) != profile["input"]["keys"]:
            raise ValueError("ADC windows/key count mismatch")
        end = 0
        for lo, hi in windows:
            if lo < end or not lo < hi <= 3300:
                raise ValueError("overlapping or invalid ADC windows")
            end = hi
    if not profile["firmwares"] or set(profile["firmwares"]) - {"mars", "probe"}:
        raise ValueError("unsupported firmware")
    runtime = profile.get("marsRuntime", "full")
    if runtime not in ("full", "compact"):
        raise ValueError("unsupported Mars runtime")
    if runtime == "compact" and (profile["id"] != "folotoy-ai-passport" or profile["chip"] != "esp32c3"
            or profile["flashBytes"] != 8 << 20 or profile["psramBytes"] != 0
            or (display["width"], display["height"]) != (240, 320) or display["touch"]
            or profile["dynamicPackageFormat"] is not None):
        raise ValueError("compact runtime requires the verified C3 Passport hardware and no dynamic packages")
    if "mars" in profile["firmwares"] and runtime == "full" and (profile["chip"] != "esp32s3"
            or profile["psramBytes"] < 8 << 20 or profile["flashBytes"] != 16 << 20
            or (viewport["width"], viewport["height"]) != (320, 240)):
        raise ValueError("current Mars requires S3, 16 MB Flash, 8 MB PSRAM and 320x240 application viewport")
    if profile["dynamicPackageFormat"] not in (None, "marsapp-v2-xtensa"):
        raise ValueError("unsupported dynamic package format")
    if profile["dynamicPackageFormat"] and (profile["architecture"] != "xtensa" or not profile["psramBytes"]):
        raise ValueError("current dynamic loader requires Xtensa and PSRAM")
    resolve_ui(profile)
    return profile


def load(board_id):
    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", board_id):
        raise ValueError("invalid board id")
    profile = validate(json.loads((REGISTRY / f"{board_id}.json").read_text()))
    if profile["id"] != board_id:
        raise ValueError("board id/file mismatch")
    return profile


def all_profiles():
    return [load(path.stem) for path in sorted(REGISTRY.glob("*.json"))]


def partition_layout(profile, data):
    """Check generated partition bounds against this exact board's Flash."""
    entries = []
    for offset in range(0, len(data) - 31, 32):
        if data[offset:offset + 2] != b"\xaaP":
            if data[offset:offset + 2] != b"\xeb\xeb" and data[offset:offset + 32] != b"\xff" * 32:
                raise ValueError("invalid partition table entry")
            break
        _, kind, subtype, address, size, label, _ = struct.unpack_from("<2sBBII16sI", data, offset)
        name = label.split(b"\0", 1)[0].decode("ascii")
        if not name or address < 0x9000 or not size or address + size > profile["flashBytes"]:
            raise ValueError("partition outside board Flash")
        if any(part["name"] == name for part in entries):
            raise ValueError("duplicate partition name")
        entries.append({"name": name, "type": kind, "subtype": subtype, "address": address, "size": size})
    if not entries:
        raise ValueError("empty partition table")
    ordered = sorted(entries, key=lambda part: part["address"])
    for previous, current in zip(ordered, ordered[1:]):
        if previous["address"] + previous["size"] > current["address"]:
            raise ValueError("overlapping partitions")
    return entries


def generate(profile, output):
    """Generate one C header from the registry; no vendor header is edited."""
    validate(profile)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    display = profile["display"]
    compact = json.dumps(profile, ensure_ascii=True, separators=(",", ":"))
    values = {
        "ID": profile["id"], "MODEL": profile["model"],
        "MANUFACTURER": profile["manufacturer"], "REVISION": profile["hardwareRevision"],
        "CHIP": profile["chip"], "ARCHITECTURE": profile["architecture"],
        "FLASH_BYTES": profile["flashBytes"], "PSRAM_BYTES": profile["psramBytes"],
        "DISPLAY_WIDTH": display["width"], "DISPLAY_HEIGHT": display["height"],
        "DISPLAY_CLOCK_HZ": display["clockHz"], "HAS_TOUCH": int(display["touch"]),
        "KEY_COUNT": profile["input"]["keys"],
        "CODEC_ADDRESS": profile["audio"].get("address", 0),
        "AUDIO_PCM5101": int(profile["audio"]["codec"] == "pcm5101"),
        "DISPLAY_ROUND": int(display.get("shape") == "round"),
        "UI_WIDTH": display.get("viewport", display)["width"],
        "UI_HEIGHT": display.get("viewport", display)["height"],
        "UI_X": display.get("viewport", {}).get("x", 0),
        "UI_Y": display.get("viewport", {}).get("y", 0),
        "TOUCH_ADDRESS": profile["input"].get("touchAddress", 0),
        "EXPANDER_ADDRESS": profile.get("expander", {}).get("address", 0),
        "TOUCH_RESET_BIT": profile.get("expander", {}).get("touchResetBit", 0),
        "DISPLAY_RESET_BIT": profile.get("expander", {}).get("displayResetBit", 0),
        "PROFILE_JSON": compact,
    }
    values.update(ui_c_values(profile))
    values.update({f"PIN_{name.upper()}": pin for name, pin in profile["pins"].items()})
    if profile["battery"]["gauge"]:
        values["BATTERY_ADDRESS"] = profile["battery"]["address"]
    for i, (lo, hi) in enumerate(profile["input"].get("adcWindowsMv", []), 1):
        values[f"KEY_{i}_MIN_MV"] = lo
        values[f"KEY_{i}_MAX_MV"] = hi
    lines = ["/* Generated from firmware/boards; do not edit. */", "#pragma once"]
    for key, value in values.items():
        is_ui_policy = key.startswith("UI_") and key not in ("UI_X", "UI_Y", "UI_WIDTH", "UI_HEIGHT")
        prefix = "MARS_" if is_ui_policy else "MARS_BOARD_"
        literal = json.dumps(value) if isinstance(value, str) else value
        lines.append(f"#define {prefix}{key} {literal}")
    text = "\n".join(lines) + "\n"
    header = output / "mars_board_config.h"
    if not header.exists() or header.read_text() != text:
        header.write_text(text)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("board")
    parser.add_argument("--target", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    profile = load(args.board)
    if profile["chip"] != args.target:
        parser.error(f"{args.board} requires {profile['chip']}, got {args.target}")
    generate(profile, args.output)
