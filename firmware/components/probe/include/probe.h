#pragma once
// Measurement probes for test builds (docs/desktop-app-design.md section 12). Does nothing unless a probe is switched on
// in the configuration of the build; no release build switches one on.
#ifdef __cplusplus
extern "C" {
#endif
void probe_start_early(void);   // call right after the core and the board are up, before the display and Wi-Fi
void probe_start(void);   // call once, after the display, Wi-Fi and the other tasks have started
#ifdef __cplusplus
}
#endif
