# Vendored ESP WebSocket Client

- Upstream: https://github.com/espressif/esp-protocols
- Pinned tag: `websocket-v1.8.0`
- Source directory: `components/esp_websocket_client`
- Original source, public header, CMakeLists, Kconfig, manifest and LICENSE retained.
- Local build: Component Manager is disabled by `build_firmware.sh`; the component is explicitly listed in `EXTRA_COMPONENT_DIRS`.
- Mars wrapper: `components/mars_services/src/mars_ws.c`.
