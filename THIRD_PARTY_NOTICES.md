# Third-party notices

Project-authored source is licensed under Apache-2.0. Bundled dependencies retain their own licenses and copyright notices. This repository is not a claim that all dependencies are Apache-2.0 or provided as source.

| Component | Version | License file |
| --- | --- | --- |
| `espressif2022__esp_emote_gfx` | 3.0.5 | [LICENSE](third_party/components/espressif2022__esp_emote_gfx/LICENSE) |
| `espressif__cjson` | 1.7.19~2 | [LICENSE](third_party/components/espressif__cjson/LICENSE) |
| `espressif__cmake_utilities` | 0.5.0 | [license.txt](third_party/components/espressif__cmake_utilities/license.txt) |
| `espressif__esp_audio_codec` | 2.5.0 | [LICENSE](third_party/components/espressif__esp_audio_codec/LICENSE) |
| `espressif__esp_codec_dev` | 1.6.2 | [LICENSE](third_party/components/espressif__esp_codec_dev/LICENSE) |
| `espressif__esp_lcd_touch` | 1.2.1 | [license.txt](third_party/components/espressif__esp_lcd_touch/license.txt) |
| `espressif__esp_new_jpeg` | 1.0.2 | [LICENSE](third_party/components/espressif__esp_new_jpeg/LICENSE) |
| `espressif__esp_websocket_client` | 1.8.0 | [LICENSE](third_party/components/espressif__esp_websocket_client/LICENSE) |
| `espressif__freetype` | 2.14.3~1 | [LICENSE](third_party/components/espressif__freetype/LICENSE) |
| `laride__heatshrink` | 0.4.1 | [LICENSE](third_party/components/laride__heatshrink/LICENSE) |
| `lvgl__lvgl` | 9.5.0 | [LICENCE.txt](third_party/components/lvgl__lvgl/LICENCE.txt) |

## Special license boundaries

- Espressif audio codec and new JPEG components include precompiled static libraries with Espressif-specific license terms. Review their LICENSE files before redistribution or use. Only ESP32-S3 binary variants are bundled.
- FreeType is used under the FreeType License (FTL); its notices and license texts are retained in the bundled source. Portions of this software are copyright © The FreeType Project (www.freetype.org). All rights reserved.
- Noto Sans SC and its generated Chinese bitmap are SIL OFL 1.1; see resources/fonts/OFL-NotoSansSC.txt. The community bitmap replaces the development build’s system-font output.
- LVGL Montserrat bitmap data in mars_canvas/round_fonts.h retains the source attribution and FONT-LICENSE.txt.
- SPD2010 panel driver preserves its upstream source notices in mars_bsp/vendor/spd2010.

## Community snapshot changes

Product applications, cloud/platform source, original repository history, product artwork and speech models are excluded. Dependency demo/test payloads, unused target binaries and the unused LVGL NemaGFX backend payload were omitted to limit redistribution. Vendor source notices remain in place. The top-level build redirects dependencies to third_party/components. Project build scripts are portable; the default Chinese bitmap and wallpaper are regenerated from documented sources. A preexisting audio host-test baseline race was corrected without changing the production audio implementation. Desktop host-test expectations follow the current APP text entry.
