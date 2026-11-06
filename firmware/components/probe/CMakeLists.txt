# Measurement probes for test builds (docs/desktop-app-design.md section 12). In a release build both options are off and
# probe_start() does nothing. `bt` is always listed: the requirements are expanded before the configuration is known, and
# with Bluetooth off the component adds nothing to the image.
idf_component_register(SRCS "probe.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_timer heap esp_wifi bt)
