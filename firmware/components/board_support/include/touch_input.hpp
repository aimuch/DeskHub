#pragma once
#include <cstdint>
#include "esp_err.h"

namespace dh {
struct TouchDiagnostics {
    bool ready = false;
    bool read_in_progress = false;
    bool sample_valid = false;
    bool stale = true;
    bool recovering = true;
    uint32_t samples = 0;
    uint32_t read_errors = 0;
    uint32_t no_packet_reads = 0;
    uint32_t slow_reads = 0;
    uint32_t irq_events = 0;
    uint32_t queue_overflows = 0;
    uint32_t stale_cancellations = 0;
    uint32_t max_read_ms = 0;
    uint32_t max_callback_us = 0;
    int64_t sample_age_ms = -1;
};

// Start once, after the board display and touch device exist. Hardware reads
// run in a separate task; the adapter callback only consumes a bounded cache.
esp_err_t touch_input_start();
TouchDiagnostics touch_input_diagnostics();
}
