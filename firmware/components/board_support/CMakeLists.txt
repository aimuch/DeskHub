idf_component_register(SRCS "board.cpp" "display_transfer.cpp" "display_buffers.cpp" "lvgl_psram.c" INCLUDE_DIRS "include" REQUIRES core esp_driver_i2c esp_driver_spi esp_lcd esp_psram spi_flash esp_timer espressif__esp_lcd_touch espressif__esp_lvgl_adapter waveshare__esp32_s3_touch_amoled_2_16 lvgl__lvgl WHOLE_ARCHIVE)
# Bound LCD DMA bounce allocations, and take the draw buffers from internal memory, without patching the managed BSP.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=spi_bus_initialize" "-Wl,--undefined=__wrap_spi_bus_initialize"
                      "-Wl,--wrap=esp_lv_adapter_register_display" "-Wl,--undefined=__wrap_esp_lv_adapter_register_display")
