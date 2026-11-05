idf_component_register(SRCS "wifi_change.cpp" "wifi_change_policy.cpp" INCLUDE_DIRS "include" REQUIRES core esp_wifi esp_event freertos)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
