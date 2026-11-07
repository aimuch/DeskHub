#pragma once
// Flash is written with the caches off, and then a task whose stack is in PSRAM has no stack: ESP-IDF asserts and the chip
// restarts (2026-11-07, 0.3.18 on the device: a tap on "icons" saved the appearance from the LVGL task, whose stack is in
// PSRAM since 0.3.16). Every flash write from such a task goes through flash_call: it runs on a small worker task with an
// internal stack and the caller waits. On an internal stack it simply calls.
#include <type_traits>
#include <utility>

namespace dh {
void flash_worker_start();                          // early, right after Bluetooth
// false: `fn` did NOT run (no worker, and the caller's stack is in PSRAM). The caller reports "not saved"; it never crashes.
bool flash_call(void (*fn)(void *), void *ctx);
template <class F> bool flash_run(F &&f) {
    using Fn = std::remove_reference_t<F>;
    return flash_call([](void *p) { (*static_cast<Fn *>(p))(); }, &f);
}
// The result of `f` when it ran, else `failed`.
template <class R, class F> R flashed(R failed, F &&f) {
    R r = failed;
    if (!flash_run([&] { r = f(); })) return failed;
    return r;
}
}  // namespace dh
