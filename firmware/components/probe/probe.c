// Measurement probes for test builds (docs/desktop-app-design.md section 12): what the BLE stack costs next to the
// display and Wi-Fi. Prints with printf, not ESP_LOG, so the lines pass the release log level (warnings only).
#include "probe.h"
#include "sdkconfig.h"

#if CONFIG_DESKHUB_PROBE_HEAP || CONFIG_DESKHUB_PROBE_BLE
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_int ble_state;   // 0 off, 1 advertising, 2 connected
#endif

#if CONFIG_DESKHUB_PROBE_BLE
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static uint8_t own_addr_type;
static void advertise(void);

// 128-bit UUIDs made up for this probe only.
static const ble_uuid128_t kService = BLE_UUID128_INIT(0x5f, 0x0a, 0x7b, 0x1e, 0x52, 0x6d, 0x4b, 0x9c, 0x8e, 0x21, 0x44, 0x48, 0x50, 0x52, 0x4f, 0x42);
static const ble_uuid128_t kHeap = BLE_UUID128_INIT(0x5f, 0x0a, 0x7b, 0x1e, 0x52, 0x6d, 0x4b, 0x9c, 0x8e, 0x21, 0x44, 0x48, 0x50, 0x48, 0x45, 0x41);

static int read_heap(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    char text[24];
    const int n = snprintf(text, sizeof(text), "%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return os_mbuf_append(ctxt->om, text, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kService.u,
     .characteristics = (struct ble_gatt_chr_def[]){{.uuid = &kHeap.u, .access_cb = read_heap, .flags = BLE_GATT_CHR_F_READ}, {0}}},
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) atomic_store(&ble_state, 2);
        else advertise();
        printf("DHPROBE_EVENT {\"ble\":\"connect\",\"status\":%d}\n", event->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("DHPROBE_EVENT {\"ble\":\"disconnect\",\"reason\":%d}\n", event->disconnect.reason);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (const uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) { printf("DHPROBE_EVENT {\"ble\":\"adv_fields_failed\"}\n"); return; }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) atomic_store(&ble_state, 1);
    else printf("DHPROBE_EVENT {\"ble\":\"adv_failed\",\"rc\":%d}\n", rc);
}

static void on_sync(void) {
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) { printf("DHPROBE_EVENT {\"ble\":\"no_address\"}\n"); return; }
    advertise();
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void start_ble(void) {
    const int64_t before = (int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (nimble_port_init() != ESP_OK) { printf("DHPROBE_EVENT {\"ble\":\"init_failed\"}\n"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(kServices) != 0 || ble_gatts_add_svcs(kServices) != 0) { printf("DHPROBE_EVENT {\"ble\":\"gatt_failed\"}\n"); return; }
    ble_svc_gap_device_name_set("DeskHub-probe");
    nimble_port_freertos_init(host_task);
    printf("DHPROBE_EVENT {\"ble\":\"started\",\"internal_cost\":%lld}\n", before - (int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
#endif

#if CONFIG_DESKHUB_PROBE_HEAP || CONFIG_DESKHUB_PROBE_BLE
static void report(void *param) {
    (void)param;
    for (;;) {
        wifi_ap_record_t ap;
        const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        printf("DHPROBE {\"t\":%lld,\"int_free\":%u,\"int_big\":%u,\"int_min\":%u,\"dma_free\":%u,\"dma_big\":%u,\"psram_free\":%u,\"wifi_rssi\":%d,\"ble\":%d}\n",
               esp_timer_get_time() / 1000,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), rssi, atomic_load(&ble_state));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif

void probe_start_early(void) {
#if CONFIG_DESKHUB_PROBE_BLE_EARLY
    start_ble();
#endif
}

void probe_start(void) {
#if CONFIG_DESKHUB_PROBE_BLE && !CONFIG_DESKHUB_PROBE_BLE_EARLY
    start_ble();
#endif
#if CONFIG_DESKHUB_PROBE_HEAP || CONFIG_DESKHUB_PROBE_BLE
    xTaskCreate(report, "dh_probe", 4096, NULL, 2, NULL);
#endif
}
