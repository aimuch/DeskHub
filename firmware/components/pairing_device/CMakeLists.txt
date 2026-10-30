# Hotspot pairing on the device (docs/pairing-design.md section 13): hotspot, setup page, TLS redemption, button.
# The decisions are in components/pairing (pure, tested on the host); this is the part that touches hardware.
idf_component_register(SRCS "pairing_device.cpp" "radio.cpp" "setup_server.cpp" "redeem_client.cpp"
                       INCLUDE_DIRS "include" "."
                       REQUIRES core pairing board_support audio ui esp_wifi esp_netif esp_event esp_http_server esp_http_client esp-tls lwip esp_driver_gpio esp_hw_support)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
