#pragma once
#include <cstddef>
extern unsigned restart_calls;  // counted by the preview harness
inline void esp_restart() { ++restart_calls; }
inline size_t esp_get_free_heap_size() { return 1024 * 1024; }
