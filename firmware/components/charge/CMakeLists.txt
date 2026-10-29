# Charge protection (docs/charge-protection-design.md). charge_policy.cpp is pure logic, tested on the host in
# firmware/tests/charge_test.cpp. charge.cpp and charge_store.cpp are the glue: the power chip, the clock, NVS.
idf_component_register(SRCS "charge_policy.cpp" "charge.cpp" "charge_store.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES board_support core
                       PRIV_REQUIRES nvs_flash)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
