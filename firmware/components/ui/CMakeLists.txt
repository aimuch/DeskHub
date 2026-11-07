idf_component_register(
    SRCS "ui.cpp" "ui_wifi.cpp" "ui_kit.cpp" "ui_pages.cpp" "ui_model.cpp" "ui_theme.cpp" "ui_text.cpp" "ui_store.cpp" "ui_icons.cpp" "ui_rename.cpp"
         "btr_ui_16.c" "btr_ui_20.c" "btr_ui_28.c" "font_clock_72.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi_change core board_support charge audio agent_client esp_system heap nvs_flash waveshare__esp32_s3_touch_amoled_2_16 espressif__esp_lvgl_adapter)
