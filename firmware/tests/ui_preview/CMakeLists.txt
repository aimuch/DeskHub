cmake_minimum_required(VERSION 3.20)
project(deskhub_ui_preview LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(FIRMWARE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LVGL_ROOT "${FIRMWARE}/managed_components/lvgl__lvgl")
if(NOT EXISTS "${LVGL_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Resolve the firmware dependencies first (idf.py reconfigure); managed LVGL 9.4 is required")
endif()
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_LIST_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LVGL_ROOT}" lvgl)

# The real UI sources, the baseline display policy, and the simulated services in harness.cpp.
set(UI "${FIRMWARE}/components/ui")
add_executable(deskhub_ui_preview harness.cpp
  "${UI}/ui.cpp" "${UI}/ui_kit.cpp" "${UI}/ui_pages.cpp" "${UI}/ui_model.cpp" "${UI}/ui_theme.cpp" "${UI}/ui_text.cpp"
  "${FIRMWARE}/components/core/policy.cpp" "${FIRMWARE}/components/charge/charge_policy.cpp"
  "${UI}/btr_ui_16.c" "${UI}/btr_ui_20.c" "${UI}/btr_ui_28.c" "${UI}/font_clock_72.c")
target_include_directories(deskhub_ui_preview PRIVATE stubs
  "${FIRMWARE}/components/core/include" "${FIRMWARE}/components/board_support/include"
  "${FIRMWARE}/components/agent_client/include" "${FIRMWARE}/components/charge/include" "${UI}/include")
target_compile_definitions(deskhub_ui_preview PRIVATE UI_CHARS_FILE="${FIRMWARE}/fonts/btr_ui.chars.txt")
target_compile_options(deskhub_ui_preview PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror>)
target_link_libraries(deskhub_ui_preview PRIVATE lvgl::lvgl)
enable_testing()
add_test(NAME device_ui_preview COMMAND deskhub_ui_preview)

# Carried from the old preview: the touch-orientation regression builds the real board.cpp against real LVGL.
add_executable(deskhub_touch_orientation_test "${FIRMWARE}/../scripts/ui_preview/touch_orientation_test.cpp" "${FIRMWARE}/components/board_support/board.cpp")
target_compile_definitions(deskhub_touch_orientation_test PRIVATE DESKHUB_TEST_REAL_LVGL=1)
target_include_directories(deskhub_touch_orientation_test PRIVATE
  "${FIRMWARE}/tests/board_stubs" "${FIRMWARE}/components/core/include" "${FIRMWARE}/components/board_support/include")
target_compile_options(deskhub_touch_orientation_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(deskhub_touch_orientation_test PRIVATE lvgl::lvgl)
add_test(NAME waveshare_touch_orientation COMMAND deskhub_touch_orientation_test)
