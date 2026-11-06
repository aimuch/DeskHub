# Measurement probes for test builds (docs/desktop-app-design.md section 12). In a release build every option is off,
# probe_start() does nothing and lvgl_psram.c is empty. `bt` is always listed: the requirements are expanded before the
# configuration is known, and with Bluetooth off the component adds nothing to the image.
idf_component_register(SRCS "probe.c" "lvgl_psram.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer heap esp_wifi bt lvgl__lvgl WHOLE_ARCHIVE)
