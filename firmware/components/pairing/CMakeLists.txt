# Hotspot pairing, the pure logic (docs/pairing-design.md). No hardware here: the hotspot, the setup page, the TLS client
# and the save path are glue that reports to the Session. Tested on the host in firmware/tests/pairing_test.cpp.
idf_component_register(SRCS "pairing_package.cpp" "pairing_input.cpp" "pairing_session.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES core espressif__cjson)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
