# Flash writes from tasks with a PSRAM stack go through here (include/flash_worker.hpp).
idf_component_register(SRCS "flash_worker.cpp" INCLUDE_DIRS "include" PRIV_REQUIRES esp_hw_support freertos)
