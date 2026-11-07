# The Bluetooth link to the DeskHub App (docs/desktop-app-design.md 13). ble_protocol.cpp is pure and tested on the host;
# ble_link.cpp is the NimBLE side and does nothing when Bluetooth is off in the configuration.
# ble_update.cpp is the pure part of the firmware update over Bluetooth (13.11), tested on the host too.
idf_component_register(SRCS "ble_protocol.cpp" "ble_update.cpp" "ble_link.cpp" INCLUDE_DIRS "include"
                       REQUIRES core
                       PRIV_REQUIRES bt esp_timer espressif__cjson app_update esp_partition mbedtls flash_worker)
