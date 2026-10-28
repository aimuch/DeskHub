idf_component_register(SRCS "management.cpp" "config_wire.cpp" "update_policy.cpp" INCLUDE_DIRS "include" REQUIRES core board_support ui esp_http_client app_update esp_app_format bootloader_support mbedtls nvs_flash esp_system esp_hw_support espressif__cjson)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
