# Third-party sources

DeskHub code is provided under the Apache License 2.0 in LICENSE. Upstream code, fonts and linked libraries retain their own licenses.

| Source | Use | License / reference |
| --- | --- | --- |
| Waveshare ESP32-S3-Touch-AMOLED-2.16 examples | Board/API/build baseline; commit `225a62bff11b5d0a0b607873860d39485a9a9685` | Apache-2.0, [repository](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16) |
| Waveshare BSP 2.0.1 | Managed board/display/touch implementation | See managed component's LICENSE; [registry](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_2_16/versions/2.0.1) |
| ESP-IDF v6.0.2 | Firmware framework, drivers, networking, NVS and TLS integration | Apache-2.0 and component-specific notices, [source](https://github.com/espressif/esp-idf/tree/v6.0.2) |
| LVGL 9.4.0 | UI toolkit | MIT, [source](https://github.com/lvgl/lvgl/tree/v9.4.0) |
| Google Fonts Noto Sans SC | Embedded Chinese glyph subset; upstream commit `e7db6ca603fd4862010383ff17ffd6cc4ffac1c0` | SIL Open Font License 1.1; full notice in `firmware/fonts/OFL.txt` |
| Adobe Source Han Sans CN Regular | Embedded UI glyph subset `btr_ui_28` / `btr_ui_20` / `btr_ui_16` (28, 20 and 16 px, 3875 glyphs), copied from the SmartHub project; upstream adobe-fonts/source-han-sans 2.005R | SIL Open Font License 1.1; full notice in `firmware/fonts/SourceHanSans-OFL.txt`; the subset is renamed `btr_ui` as the Reserved Font Name rule requires |
| FastAPI / Starlette / Pydantic / HTTPX | Agent API, validation and HTTP client | MIT / BSD-3-Clause; exact distributions in `agent/uv.lock` |
| Uvicorn | Local HTTP/TLS server | BSD-3-Clause |
| psutil | Actual host telemetry | BSD-3-Clause |
| cryptography | Local certificate/key generation | Apache-2.0 OR BSD-3-Clause |
| pyserial 3.5 | USB serial configuration tool | BSD-3-Clause |
| pytest / pytest-asyncio / jsonschema | Development verification only | MIT |

All transitive firmware dependencies and component hashes are recorded in `firmware/dependencies.lock`. Python dependencies are pinned in `agent/uv.lock`; USB transport dependencies are pinned in `tools/requirements.txt`. Packaged releases must retain license files from their actual resolved dependencies.

BetterDisplay is an external program supplied separately by its owner. DeskHub does not redistribute it or grant its license. Its installed version, CLI output format, session prerequisites and hardware compatibility require verification. The Agent uses its documented fixed command-line interface.

