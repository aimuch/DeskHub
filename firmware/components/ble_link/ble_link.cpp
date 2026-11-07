// NimBLE side of the link (docs/desktop-app-design.md 13). Pure rules live in ble_protocol.cpp.
#include "ble_link.hpp"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "core.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <cstring>
#include <mutex>

extern "C" void ble_store_config_init(void);

namespace dh::ble {
namespace {
const char *TAG = "ble";
// 7A5E000x-3C4B-4E0D-9B6A-44534B48ED01 (little-endian here): service 1, info 2, control 3, events 4.
#define DH_UUID(n) BLE_UUID128_INIT(0x01, 0xED, 0x48, 0x4B, 0x53, 0x44, 0x6A, 0x9B, 0x0D, 0x4E, 0x4B, 0x3C, n, 0x00, 0x5E, 0x7A)
const ble_uuid128_t kService = DH_UUID(0x01), kInfo = DH_UUID(0x02), kControl = DH_UUID(0x03), kEvents = DH_UUID(0x04);

std::mutex mu;
bool running = false, connected = false, secure = false, subscribed = false, confirm_pending = false;
uint16_t conn = BLE_HS_CONN_HANDLE_NONE, events_handle = 0, confirm_conn = BLE_HS_CONN_HANDLE_NONE;
uint32_t digits = 0;
int64_t digits_shown = 0;
uint8_t own_addr_type = 0;
std::vector<Source> list;
std::string current_id;
SourcesDraft draft;
Requests requests;
PairingWindow window;
esp_timer_handle_t ticker = nullptr;

int64_t now_ms() { return esp_timer_get_time() / 1000; }
void set_command(const std::string &text) {
    std::string copy = text;
    mutate([](Snapshot &s, void *p) { s.command = *static_cast<std::string *>(p); }, &copy);
}
std::string name_of(const std::string &id) {
    for (const auto &s : list) if (s.id == id) return s.name;
    return id;
}
int bond_count() {
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n;
}
void reject_digits_locked() {
    if (!confirm_pending) return;
    ble_sm_io io{};
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = 0;
    ble_sm_inject_io(confirm_conn, &io);
    confirm_pending = false;
}
void drop_link_state_locked() {
    connected = secure = subscribed = false;
    conn = BLE_HS_CONN_HANDLE_NONE;
    list.clear();
    current_id.clear();
    draft = SourcesDraft{};
    requests.reset();
    confirm_pending = false;
}

int read_info(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *) {
    const std::string text = info_json(snapshot().device_id, VERSION);
    return os_mbuf_append(ctxt->om, text.data(), static_cast<uint16_t>(text.size())) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
int write_control(uint16_t handle, uint16_t, ble_gatt_access_ctxt *ctxt, void *) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    char buf[kMaxMessage + 1];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, kMaxMessage, &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    const Message m = parse(std::string(buf, len));
    std::string notice;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!secure || handle != conn) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (m.op == Op::Bad) { ESP_LOGW(TAG, "refused: %s", m.error.c_str()); return BLE_ATT_ERR_UNLIKELY; }
        if (m.op == Op::Result) {
            if (auto id = requests.finish(m.result)) notice = result_text(m.result, name_of(*id));
        } else if (m.op == Op::Current) {
            current_id = m.current;
        } else {
            std::vector<Source> complete;
            const Apply a = apply(draft, m, &complete);
            if (a == Apply::Complete) list = complete;
            if (a == Apply::Rejected) { ESP_LOGW(TAG, "source list refused"); return BLE_ATT_ERR_UNLIKELY; }
        }
    }
    if (!notice.empty()) set_command(notice);
    return 0;
}

int events_access(uint16_t, uint16_t, ble_gatt_access_ctxt *, void *) { return 0; }   // notify only

// The service table, filled field by field (C++ has no partial designated initializers without warnings).
ble_gatt_chr_def g_chars[4];
ble_gatt_svc_def g_services[2];
void make_services() {
    memset(g_chars, 0, sizeof(g_chars));
    memset(g_services, 0, sizeof(g_services));
    g_chars[0].uuid = &kInfo.u;
    g_chars[0].access_cb = read_info;
    g_chars[0].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
    g_chars[1].uuid = &kControl.u;
    g_chars[1].access_cb = write_control;
    g_chars[1].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
    g_chars[2].uuid = &kEvents.u;
    g_chars[2].access_cb = events_access;
    g_chars[2].flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
    g_chars[2].val_handle = &events_handle;
    g_services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_services[0].uuid = &kService.u;
    g_services[0].characteristics = g_chars;
}

void advertise();
int gap_event(ble_gap_event *e, void *) {
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT: {
        std::lock_guard<std::mutex> lock(mu);
        if (e->connect.status == 0) { connected = true; conn = e->connect.conn_handle; }
        else advertise();
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        bool had_request;
        {
            std::lock_guard<std::mutex> lock(mu);
            had_request = requests.waiting();
            drop_link_state_locked();
        }
        if (had_request) set_command("电脑断开了，切换没有结果");
        advertise();
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        ble_gap_conn_desc d{};
        std::lock_guard<std::mutex> lock(mu);
        if (e->enc_change.status == 0 && ble_gap_conn_find(e->enc_change.conn_handle, &d) == 0) {
            secure = d.sec_state.encrypted && d.sec_state.authenticated && d.sec_state.bonded;
            if (secure && window.is_open(now_ms())) window.close();   // the computer is paired: the window has done its job
        } else secure = false;
        return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (e->passkey.params.action != BLE_SM_IOACT_NUMCMP) return 0;
        std::lock_guard<std::mutex> lock(mu);
        if (!window.is_open(now_ms())) {   // K1: no new computer outside the window
            ble_sm_io io{};
            io.action = BLE_SM_IOACT_NUMCMP;
            io.numcmp_accept = 0;
            ble_sm_inject_io(e->passkey.conn_handle, &io);
            return 0;
        }
        confirm_pending = true;
        confirm_conn = e->passkey.conn_handle;
        digits = e->passkey.params.numcmp;
        digits_shown = now_ms();
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        std::lock_guard<std::mutex> lock(mu);
        if (!window.is_open(now_ms())) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        ble_gap_conn_desc d{};
        if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        std::lock_guard<std::mutex> lock(mu);
        if (e->subscribe.attr_handle == events_handle) subscribed = e->subscribe.cur_notify;
        return 0;
    }
    default:
        return 0;
    }
}

void advertise() {
    if (ble_gap_adv_active()) return;
    ble_hs_adv_fields f{};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &kService;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;
    ble_hs_adv_fields r{};
    const char *name = ble_svc_gap_device_name();
    r.name = reinterpret_cast<const uint8_t *>(name);
    r.name_len = static_cast<uint8_t>(strlen(name));
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    ble_gap_adv_params p{};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(own_addr_type, nullptr, BLE_HS_FOREVER, &p, gap_event, nullptr);
}

void on_sync() {
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) return;
    std::lock_guard<std::mutex> lock(mu);
    running = true;
    advertise();
}

// Twice a second: a request without a result runs out; digits nobody confirmed are refused; the window closes on time.
void tick(void *) {
    bool expired = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        const int64_t now = now_ms();
        expired = requests.expire(now);
        if (confirm_pending && (now - digits_shown >= kConfirmMs || !window.is_open(now))) reject_digits_locked();
    }
    if (expired) set_command("电脑没有回音，不知道切了没有");
}
}  // namespace

void start() {
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;   // numeric comparison
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;   // a third computer pushes out the oldest bond
    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    make_services();
    if (ble_gatts_count_cfg(g_services) != 0 || ble_gatts_add_svcs(g_services) != 0) { ESP_LOGE(TAG, "gatt failed"); return; }
    const std::string id = snapshot().device_id;
    const std::string name = "DeskHub-" + (id.size() >= 4 ? id.substr(id.size() - 4) : id);
    ble_svc_gap_device_name_set(name.c_str());
    esp_timer_create_args_t args{};
    args.callback = tick;
    args.name = "dh_ble";
    if (esp_timer_create(&args, &ticker) == ESP_OK) esp_timer_start_periodic(ticker, 500 * 1000);
    nimble_port_freertos_init([](void *) { nimble_port_run(); nimble_port_freertos_deinit(); });
}

Status status() {
    std::lock_guard<std::mutex> lock(mu);
    const int64_t now = now_ms();
    Status s;
    s.running = running;
    s.connected = connected;
    s.secure = secure;
    s.bonds = running ? bond_count() : 0;
    s.pairing_open = window.is_open(now);
    s.pairing_left_ms = window.left_ms(now);
    s.confirm_pending = confirm_pending;
    s.digits = digits;
    s.confirm_left_ms = confirm_pending ? std::max<int64_t>(0, kConfirmMs - (now - digits_shown)) : 0;
    s.waiting = requests.waiting();
    return s;
}
std::vector<Source> sources() {
    std::lock_guard<std::mutex> lock(mu);
    return secure ? list : std::vector<Source>{};
}
std::string current() {
    std::lock_guard<std::mutex> lock(mu);
    return secure ? current_in(list, current_id) : std::string();
}
bool request(const std::string &id) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!secure || !subscribed) return false;
        uint32_t req = 0;
        if (!requests.begin(id, list, now_ms(), &req)) return false;
        const std::string json = request_json(req, id);
        os_mbuf *om = ble_hs_mbuf_from_flat(json.data(), static_cast<uint16_t>(json.size()));
        if (!om || ble_gatts_notify_custom(conn, events_handle, om) != 0) { requests.reset(); return false; }
        name = name_of(id);
    }
    set_command("已请求切到 " + name + "，等电脑回答");
    return true;
}
void open_pairing() { std::lock_guard<std::mutex> lock(mu); window.open(now_ms()); }
void close_pairing() {
    std::lock_guard<std::mutex> lock(mu);
    reject_digits_locked();
    window.close();
}
void answer_digits(bool same) {
    std::lock_guard<std::mutex> lock(mu);
    if (!confirm_pending) return;
    ble_sm_io io{};
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = accept_pairing(window, now_ms(), digits_shown, same) ? 1 : 0;
    ble_sm_inject_io(confirm_conn, &io);
    confirm_pending = false;
}
bool forget_all() {
    uint16_t c;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (!running) return true;
        c = conn;
    }
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    const bool ok = ble_store_clear() == 0;
    return ok && bond_count() == 0;
}
}  // namespace dh::ble

#else   // no Bluetooth in this configuration

namespace dh::ble {
void start() {}
Status status() { return {}; }
std::vector<Source> sources() { return {}; }
std::string current() { return ""; }
bool request(const std::string &) { return false; }
void open_pairing() {}
void close_pairing() {}
void answer_digits(bool) {}
bool forget_all() { return true; }
}  // namespace dh::ble
#endif
