# The sound of the device (docs/design Dev-Sound). audio_policy.cpp is pure logic, tested on the host in
# firmware/tests/audio_test.cpp. audio.cpp is the glue: the codec of the board, the queue, NVS.
idf_component_register(SRCS "audio_policy.cpp" "audio.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core waveshare__esp32_s3_touch_amoled_2_16 espressif__esp_codec_dev
                       PRIV_REQUIRES nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
