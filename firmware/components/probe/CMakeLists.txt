# Measurement probes for test builds (docs/desktop-app-design.md section 12). In a release build every option is off,
# probe_start() does nothing, lvgl_psram.c is empty and display_internal.c is not linked in. `bt` is always listed: the
# requirements are expanded before the configuration is known, and with Bluetooth off the component adds nothing.
set(probe_srcs "probe.c" "lvgl_psram.c")
if(CONFIG_DESKHUB_PROBE_DISPLAY_INTERNAL)
    list(APPEND probe_srcs "display_internal.c")
endif()
idf_component_register(SRCS ${probe_srcs} INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer heap esp_wifi bt lvgl__lvgl espressif__esp_lvgl_adapter WHOLE_ARCHIVE)
if(CONFIG_DESKHUB_PROBE_DISPLAY_INTERNAL)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lv_adapter_register_display")
endif()
if(CONFIG_DESKHUB_PROBE_LVGL_STACK_PSRAM)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lv_adapter_init")
endif()
