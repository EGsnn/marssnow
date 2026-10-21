#!/usr/bin/env python3
"""Generate redistributable community font and a simple RGB565 wallpaper."""
import hashlib, json, struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
ROOT = Path(__file__).resolve().parents[1]
source = ROOT / "resources/fonts/NotoSansSC-VF.ttf"
assert hashlib.sha256(source.read_bytes()).hexdigest() == "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
font = ImageFont.truetype(str(source), 16)
chars = {}
for q in range(1, 95):
    for w in range(1, 95):
        try:
            c = bytes([q + 0xa0, w + 0xa0]).decode("gb2312")
            chars[ord(c)] = (q - 1) * 94 + w - 1
        except UnicodeDecodeError:
            pass
# Retain GB2312 region offsets; allocate every region, including unused slots.
bitmap = bytearray(94 * 94 * 32)
for cp, idx in sorted(chars.items()):
    image = Image.new("L", (32, 32))
    ImageDraw.Draw(image).text((4, 0), chr(cp), font=font, fill=255)
    box = image.getbbox()
    if not box:
        continue
    glyph = image.crop(box)
    glyph.thumbnail((16, 16), Image.Resampling.LANCZOS)
    out = Image.new("L", (16, 16))
    out.paste(glyph, ((16 - glyph.width) // 2, (16 - glyph.height) // 2))
    for y in range(16):
        row = sum((1 << (15 - x)) for x in range(16) if out.getpixel((x, y)) > 100)
        struct.pack_into(">H", bitmap, idx * 32 + y * 2, row)
index = b"".join(struct.pack("<HH", cp, idx) for cp, idx in sorted(chars.items()))
data = b"FCN1" + struct.pack("<III", len(chars), 16, 16 + len(index)) + index + bitmap
(ROOT / "firmware/mars_system/components/mars_common/font/font_cn.bin").write_bytes(data)
assets = ROOT / "firmware/mars_system/resources/shared_assets"
assets.mkdir(parents=True, exist_ok=True)
# Original procedural gradient; no product artwork or speech model.
pixels = bytearray()
for y in range(240):
    for x in range(320):
        red, green, blue = 10 + y * 20 // 240, 17 + x * 12 // 320, 30 + y * 18 // 240
        pixels += struct.pack("<H", ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3))
name = "mars_landscape.rgb565.bin"
(assets / name).write_bytes(pixels)
(assets / "manifest.json").write_text(json.dumps({"files": [{"name": name, "width": 320, "height": 240, "sha256": hashlib.sha256(pixels).hexdigest()}]}, indent=2) + "\n")
print(f"Generated OFL font: {len(chars)} glyphs; procedural wallpaper: {len(pixels)} bytes")
