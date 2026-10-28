idf_component_register(SRCS "policy.cpp" "core.cpp" "config_jobs.cpp" INCLUDE_DIRS "include" REQUIRES esp_app_format nvs_flash esp_timer espressif__cjson esp_system esp_netif esp_wifi esp_event lwip esp_hw_support)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
