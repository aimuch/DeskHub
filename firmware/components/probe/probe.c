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
static atomic_int echo_count, echo_bytes;

#if CONFIG_DESKHUB_PROBE_BLE_SECURE
// E3 (docs/desktop-app-design.md 13.5): every characteristic wants an encrypted, authenticated link.
#define DH_READ (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN)
#define DH_WRITE (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)
void ble_store_config_init(void);
#else
#define DH_READ BLE_GATT_CHR_F_READ
#define DH_WRITE BLE_GATT_CHR_F_WRITE
#endif

// 128-bit UUIDs made up for this probe only.
static const ble_uuid128_t kService = BLE_UUID128_INIT(0x5f, 0x0a, 0x7b, 0x1e, 0x52, 0x6d, 0x4b, 0x9c, 0x8e, 0x21, 0x44, 0x48, 0x50, 0x52, 0x4f, 0x42);
static const ble_uuid128_t kHeap = BLE_UUID128_INIT(0x5f, 0x0a, 0x7b, 0x1e, 0x52, 0x6d, 0x4b, 0x9c, 0x8e, 0x21, 0x44, 0x48, 0x50, 0x48, 0x45, 0x41);
static const ble_uuid128_t kEcho = BLE_UUID128_INIT(0x5f, 0x0a, 0x7b, 0x1e, 0x52, 0x6d, 0x4b, 0x9c, 0x8e, 0x21, 0x44, 0x48, 0x45, 0x43, 0x48, 0x4f);
static uint16_t echo_handle;

// The Mac writes up to 244 bytes; the same bytes go back as a notification, for the round trip.
static int echo(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    uint8_t buf[244];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    atomic_fetch_add(&echo_count, 1);
    atomic_fetch_add(&echo_bytes, len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (om && ble_gatts_notify_custom(conn, echo_handle, om) != 0) printf("DHPROBE_EVENT {\"ble\":\"notify_failed\"}\n");
    return 0;
}

static int read_heap(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    char text[24];
    const int n = snprintf(text, sizeof(text), "%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return os_mbuf_append(ctxt->om, text, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kService.u,
     .characteristics = (struct ble_gatt_chr_def[]){{.uuid = &kHeap.u, .access_cb = read_heap, .flags = DH_READ},
                                                    {.uuid = &kEcho.u, .access_cb = echo, .flags = DH_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &echo_handle},
                                                    {0}}},
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
    case BLE_GAP_EVENT_MTU:
        printf("DHPROBE_EVENT {\"ble\":\"mtu\",\"value\":%d}\n", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        printf("DHPROBE_EVENT {\"ble\":\"subscribe\",\"notify\":%d}\n", event->subscribe.cur_notify);
        return 0;
#if CONFIG_DESKHUB_PROBE_BLE_SECURE
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        const int found = ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0;
        printf("DHPROBE_EVENT {\"ble\":\"encrypted\",\"status\":%d,\"authenticated\":%d,\"bonded\":%d,\"int_free\":%u}\n", event->enc_change.status,
               found ? desc.sec_state.authenticated : -1, found ? desc.sec_state.bonded : -1, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            // A test build accepts on its own; the release shows the digits on the screen and waits for a tap.
            printf("DHPROBE_EVENT {\"ble\":\"numcmp\",\"digits\":\"%06lu\"}\n", (unsigned long)event->passkey.params.numcmp);
            struct ble_sm_io io = {0};
            io.action = BLE_SM_IOACT_NUMCMP;
            io.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
        printf("DHPROBE_EVENT {\"ble\":\"repeat_pairing\"}\n");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
#endif
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
#if CONFIG_DESKHUB_PROBE_BLE_SECURE
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;   // numeric comparison
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
#endif
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(kServices) != 0 || ble_gatts_add_svcs(kServices) != 0) { printf("DHPROBE_EVENT {\"ble\":\"gatt_failed\"}\n"); return; }
    ble_svc_gap_device_name_set("DeskHub-probe");
    nimble_port_freertos_init(host_task);
    printf("DHPROBE_EVENT {\"ble\":\"started\",\"internal_cost\":%lld}\n", before - (int64_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
#endif

#if CONFIG_DESKHUB_PROBE_BLE
#define ECHO_COUNT atomic_load(&echo_count)
#define ECHO_BYTES atomic_load(&echo_bytes)
#else
#define ECHO_COUNT 0
#define ECHO_BYTES 0
#endif
#if CONFIG_DESKHUB_PROBE_HEAP || CONFIG_DESKHUB_PROBE_BLE
static void report(void *param) {
    (void)param;
    for (;;) {
        wifi_ap_record_t ap;
        const int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        printf("DHPROBE {\"t\":%lld,\"int_free\":%u,\"int_big\":%u,\"int_min\":%u,\"dma_free\":%u,\"dma_big\":%u,\"psram_free\":%u,\"wifi_rssi\":%d,\"ble\":%d,\"echo\":%d,\"echo_bytes\":%d}\n",
               esp_timer_get_time() / 1000,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), rssi, atomic_load(&ble_state),
               ECHO_COUNT, ECHO_BYTES);
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
    // The stack in PSRAM: the probe must not take the internal memory it measures, nor fail to start when it is short.
    if (xTaskCreateWithCaps(report, "dh_probe", 4096, NULL, 2, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        printf("DHPROBE_EVENT {\"probe\":\"task_failed\"}\n");
#endif
}
